/*
 * hidusb.c - HIDUSB.SYS for Windows 95 OSR2.1, stage E1 (see hidusb.h)
 */
#include "hidusb.h"

/* the FDO belongs to HIDCLASS; our extension follows its HID_DEVICE_EXTENSION */
#define MINI_EXT(_dev_) \
    ((PDEVEXT)(((PHID_DEVICE_EXTENSION)(_dev_)->DeviceExtension)->MiniDeviceExtension))

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

static PVOID HidAlloc(ULONG n)
{
    PVOID p = ExAllocatePoolWithTag(NonPagedPool, n, HIDUSB_TAG);
    if (p)
        RtlZeroMemory(p, n);
    return p;
}

/* Hex dump, 16 bytes per DbgPrint line. */
static void LogHex(const char *tag, PUCHAR buf, ULONG len)
{
    static const char hex[] = "0123456789abcdef";
    ULONG off, i, n;
    char line[16 * 3 + 1];

    for (off = 0; off < len; off += 16) {
        n = len - off;
        if (n > 16)
            n = 16;
        for (i = 0; i < n; i++) {
            line[i * 3]     = hex[buf[off + i] >> 4];
            line[i * 3 + 1] = hex[buf[off + i] & 15];
            line[i * 3 + 2] = ' ';
        }
        line[n * 3] = 0;
        HIDDBG(("[HIDUSB] %s %03x: %s\n", tag, off, line));
    }
}

/* Synchronous URB to the USB stack below us. */
static NTSTATUS CallUsbd(PDEVEXT de, PURB urb)
{
    KEVENT event;
    IO_STATUS_BLOCK iosb;
    PIRP irp;
    PIO_STACK_LOCATION next;
    NTSTATUS st;

    KeInitializeEvent(&event, NotificationEvent, FALSE);
    irp = IoBuildDeviceIoControlRequest(IOCTL_INTERNAL_USB_SUBMIT_URB,
                                        de->TopOfStack, NULL, 0, NULL, 0,
                                        TRUE, &event, &iosb);
    if (!irp)
        return STATUS_INSUFFICIENT_RESOURCES;
    next = IoGetNextIrpStackLocation(irp);
    next->Parameters.Others.Argument1 = urb;

    st = IoCallDriver(de->TopOfStack, irp);
    if (st == STATUS_PENDING) {
        /* bounded: a device that was just unplugged may never answer */
        LARGE_INTEGER timeout;
        timeout.QuadPart = -100000000L;                 /* 10 s */
        if (KeWaitForSingleObject(&event, Suspended, KernelMode, FALSE, &timeout)
                == STATUS_TIMEOUT) {
            HIDDBG(("[HIDUSB] URB request timed out, cancelling\n"));
            TRC("U.urb timeout", 0);
            IoCancelIrp(irp);
            timeout.QuadPart = -20000000L;              /* 2 s more */
            if (KeWaitForSingleObject(&event, Suspended, KernelMode, FALSE, &timeout)
                    == STATUS_TIMEOUT)
                return STATUS_IO_TIMEOUT;               /* leave the IRP to USBD */
        }
        st = iosb.Status;
    }
    return st;
}

/* GET_DESCRIPTOR: function selects device / interface recipient. */
static NTSTATUS GetDescriptor(PDEVEXT de, USHORT function, UCHAR type,
                              UCHAR index, USHORT lang, PVOID buf, ULONG len,
                              PULONG got)
{
    PURB urb;
    NTSTATUS st;

    urb = (PURB)HidAlloc(sizeof(struct _URB_CONTROL_DESCRIPTOR_REQUEST));
    if (!urb)
        return STATUS_INSUFFICIENT_RESOURCES;
    UsbBuildGetDescriptorRequest(urb,
        (USHORT)sizeof(struct _URB_CONTROL_DESCRIPTOR_REQUEST),
        type, index, lang, buf, NULL, len, NULL);
    urb->UrbHeader.Function = function;
    st = CallUsbd(de, urb);
    if (got)
        *got = urb->UrbControlDescriptorRequest.TransferBufferLength;
    ExFreePool(urb);
    return st;
}

/* ------------------------------------------------------------------ */
/* interrupt IN polling (one per HID interface)                        */
/* ------------------------------------------------------------------ */

static void DeliverReport(PDEVEXT de, PUCHAR data, ULONG len);
static NTSTATUS NTAPI PollComplete(PDEVICE_OBJECT dev, PIRP irp, PVOID ctx);
static NTSTATUS PollSubmit(PIFSLOT s);

static void FreePollRes(PPOLLRES r)
{
    if (r->Irp) IoFreeIrp(r->Irp);
    if (r->Urb) ExFreePool(r->Urb);
    if (r->Buf) ExFreePool(r->Buf);
    ExFreePool(r);
}

static NTSTATUS PollSubmit(PIFSLOT s)
{
    PDEVEXT de = s->De;
    PPOLLRES r = s->Pr;
    PIRP irp = r->Irp;
    PIO_STACK_LOCATION next;
    CCHAR stack = de->TopOfStack->StackSize;

    IoInitializeIrp(irp, (USHORT)IoSizeOfIrp(stack), stack);
    RtlZeroMemory(r->Urb, sizeof(struct _URB_BULK_OR_INTERRUPT_TRANSFER));
    UsbBuildInterruptOrBulkTransferRequest(r->Urb,
        sizeof(struct _URB_BULK_OR_INTERRUPT_TRANSFER), s->InPipe,
        r->Buf, NULL, s->InMaxPacket,
        USBD_TRANSFER_DIRECTION_IN | USBD_SHORT_TRANSFER_OK, NULL);

    next = IoGetNextIrpStackLocation(irp);
    next->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
    next->Parameters.DeviceIoControl.IoControlCode = IOCTL_INTERNAL_USB_SUBMIT_URB;
    next->Parameters.Others.Argument1 = r->Urb;
    IoSetCompletionRoutine(irp, PollComplete, r, TRUE, TRUE, TRUE);

    InterlockedExchange((PLONG)&s->PollActive, 1);
    return IoCallDriver(de->TopOfStack, irp);
}

/* A report from interface s: in merged mode it gets the merged report id. */
static void DeliverSlot(PIFSLOT s, PUCHAR data, ULONG len)
{
    PDEVEXT de = s->De;
    UCHAR tmp[HID_MAX_REPORT];

    if (!de->Merged) {
        DeliverReport(de, data, len);
        return;
    }
    if (!s->UsesIds) {
        if (len > HID_MAX_REPORT - 1)
            len = HID_MAX_REPORT - 1;
        tmp[0] = s->FixedId;
        RtlCopyMemory(tmp + 1, data, len);
        len++;
    } else {
        if (len < 1)
            return;
        if (len > HID_MAX_REPORT)
            len = HID_MAX_REPORT;
        RtlCopyMemory(tmp, data, len);
        tmp[0] = (UCHAR)(tmp[0] + s->IdAdd);
    }
    DeliverReport(de, tmp, len);
}

static NTSTATUS NTAPI PollComplete(PDEVICE_OBJECT dev, PIRP irp, PVOID ctx)
{
    PPOLLRES r = (PPOLLRES)ctx;
    PIFSLOT s = r->Slot;
    NTSTATUS st = irp->IoStatus.Status;
    ULONG got;

    UNREFERENCED_PARAMETER(dev);

    if (!s) {
        /* PollStop gave up waiting for this IRP and the device is going away: the
         * IRP, URB and buffer are ours to free now, the slot may be gone already */
        FreePollRes(r);
        return STATUS_MORE_PROCESSING_REQUIRED;
    }
    got = r->Urb->UrbBulkOrInterruptTransfer.TransferBufferLength;

    if (NT_SUCCESS(st)) {
        s->PollErrors = 0;
        s->Reports++;
#if HIDUSB_LOG_REPORTS
        /* log changes only (a keyboard repeats its last report) */
        if (got > sizeof(s->LastReport))
            got = sizeof(s->LastReport);
        if (got != s->LastLen ||
            RtlCompareMemory(s->LastReport, r->Buf, got) != got) {
            LogHex(s->Index == 0 ? "report" : s->Index == 1 ? "report1" : "report2",
                   r->Buf, got);
            s->LastLen = got;
            RtlCopyMemory(s->LastReport, r->Buf, got);
        }
#endif
        DeliverSlot(s, r->Buf, r->Urb->UrbBulkOrInterruptTransfer.TransferBufferLength);
    } else {
        s->PollErrors++;
        HIDDBG(("[HIDUSB] poll failed if%u irp=%x urb=%x (%u)\n", s->Index, st,
                (ULONG)r->Urb->UrbHeader.Status, s->PollErrors));
    }

    /* stop on request, on cancel, or after repeated errors (no spinning) */
    if (s->Stopping || st == STATUS_CANCELLED || s->PollErrors >= 3) {
        InterlockedExchange((PLONG)&s->PollActive, 0);
        KeSetEvent(&s->PollIdle, 0, FALSE);
        /* the last touch of the slot is done; if PollStop has gone already, free */
        if (InterlockedExchange(&r->Own, 2) == 1)
            FreePollRes(r);
    } else {
        PollSubmit(s);
    }
    return STATUS_MORE_PROCESSING_REQUIRED;     /* we own the IRP */
}

static void PollStop(PIFSLOT s)
{
    PPOLLRES r = s->Pr;

    if (!r)
        return;
    InterlockedExchange((PLONG)&s->Stopping, 1);
    if (s->PollActive) {
        LARGE_INTEGER timeout;

        IoCancelIrp(r->Irp);
        /*
         * Only a moment.  When the system suspends, the cancel completes after the
         * resume, once the hub has handled it; the PnP thread that carries this
         * REMOVE is the one the hub needs for that, so a long wait here only
         * deadlocks until it times out (seen: 20 s, and the device never came back).
         */
        timeout.QuadPart = -5000000L;                   /* 0.5 s */
        if (KeWaitForSingleObject(&s->PollIdle, Suspended, KernelMode, FALSE, &timeout)
                == STATUS_TIMEOUT) {
            HIDDBG(("[HIDUSB] interrupt IRP not complete yet, it is left to its completion routine\n"));
            s->Pr = NULL;
            r->Slot = NULL;                             /* the completion frees everything */
            if (InterlockedExchange(&r->Own, 1) == 2)
                FreePollRes(r);                         /* it came in just now */
            return;
        }
    }
    s->Pr = NULL;
    r->Slot = NULL;
    if (InterlockedExchange(&r->Own, 1) == 2)
        FreePollRes(r);                                 /* the completion has finished */
    /* else it is about to finish and frees them itself */
}

static NTSTATUS PollStart(PIFSLOT s)
{
    CCHAR stack = s->De->TopOfStack->StackSize;
    PPOLLRES r;

    KeInitializeEvent(&s->PollIdle, NotificationEvent, FALSE);
    s->Stopping = 0;
    s->PollErrors = 0;
    r = (PPOLLRES)HidAlloc(sizeof(POLLRES));
    if (!r)
        return STATUS_INSUFFICIENT_RESOURCES;
    r->Slot = s;
    r->Irp = IoAllocateIrp(stack, FALSE);
    r->Urb = (PURB)HidAlloc(sizeof(struct _URB_BULK_OR_INTERRUPT_TRANSFER));
    r->Buf = (PUCHAR)HidAlloc(s->InMaxPacket ? s->InMaxPacket : 8);
    if (!r->Irp || !r->Urb || !r->Buf) {
        FreePollRes(r);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    s->Pr = r;
    PollSubmit(s);
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* device start / stop                                                 */
/* ------------------------------------------------------------------ */

static void LogDeviceDescriptor(PUSB_DEVICE_DESCRIPTOR d)
{
    HIDDBG(("[HIDUSB] device: VID %04x PID %04x bcdDevice %04x class %02x/%02x/%02x ep0max %u\n",
            d->idVendor, d->idProduct, d->bcdDevice, d->bDeviceClass,
            d->bDeviceSubClass, d->bDeviceProtocol, d->bMaxPacketSize0));
}

static NTSTATUS ReadConfiguration(PDEVEXT de)
{
    ULONG siz = sizeof(USB_CONFIGURATION_DESCRIPTOR) + 512, got;
    NTSTATUS st;

    for (;;) {
        de->ConfigDesc = (PUSB_CONFIGURATION_DESCRIPTOR)HidAlloc(siz);
        if (!de->ConfigDesc)
            return STATUS_INSUFFICIENT_RESOURCES;
        st = GetDescriptor(de, URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE,
                           USB_CONFIGURATION_DESCRIPTOR_TYPE, 0, 0,
                           de->ConfigDesc, siz, &got);
        if (got > 0 && de->ConfigDesc->wTotalLength > siz) {
            siz = de->ConfigDesc->wTotalLength;         /* truncated: retry */
            ExFreePool(de->ConfigDesc);
            de->ConfigDesc = NULL;
            continue;
        }
        return st;
    }
}

/* The HID class descriptor follows its interface descriptor. */
static PUSB_HID_DESCRIPTOR FindHidDescriptor(PUSB_CONFIGURATION_DESCRIPTOR cfg,
                                             PUSB_INTERFACE_DESCRIPTOR ifd)
{
    PUCHAR p = (PUCHAR)ifd + ifd->bLength;
    PUCHAR end = (PUCHAR)cfg + cfg->wTotalLength;

    while (p + 2 <= end && p[0] != 0) {
        if (p[1] == USB_INTERFACE_DESCRIPTOR_TYPE)
            break;                              /* next interface: not found */
        if (p[1] == USB_HID_DESCRIPTOR_TYPE)
            return (PUSB_HID_DESCRIPTOR)p;
        p += p[0];
    }
    return NULL;
}

static void LogConfiguration(PDEVEXT de)
{
    PUCHAR p = (PUCHAR)de->ConfigDesc;
    PUCHAR end = p + de->ConfigDesc->wTotalLength;

    HIDDBG(("[HIDUSB] config: total %u, %u interface(s), value %u\n",
            de->ConfigDesc->wTotalLength, de->ConfigDesc->bNumInterfaces,
            de->ConfigDesc->bConfigurationValue));
    while (p + 2 <= end && p[0] != 0) {
        if (p[1] == USB_INTERFACE_DESCRIPTOR_TYPE) {
            PUSB_INTERFACE_DESCRIPTOR i = (PUSB_INTERFACE_DESCRIPTOR)p;
            HIDDBG(("[HIDUSB]   interface %u alt %u: class %02x/%02x/%02x, %u endpoint(s)\n",
                    i->bInterfaceNumber, i->bAlternateSetting, i->bInterfaceClass,
                    i->bInterfaceSubClass, i->bInterfaceProtocol, i->bNumEndpoints));
        } else if (p[1] == USB_ENDPOINT_DESCRIPTOR_TYPE) {
            PUSB_ENDPOINT_DESCRIPTOR e = (PUSB_ENDPOINT_DESCRIPTOR)p;
            HIDDBG(("[HIDUSB]     endpoint %02x attr %02x maxpacket %u interval %u\n",
                    e->bEndpointAddress, e->bmAttributes, e->wMaxPacketSize,
                    e->bInterval));
        }
        p += p[0];
    }
}

/*
 * OSR2.1's USBD takes whatever follows an interface descriptor as its
 * endpoint descriptors.  A HID interface has the class descriptor (type
 * 0x21) there, which USBD then reads as an endpoint (E1 log: "ep 11, type 1,
 * maxpacket 256, interval 22" = the bytes of the HID descriptor) and the real
 * interrupt endpoint is never configured.  So USBD gets a copy of the
 * configuration descriptor with only configuration/interface/endpoint
 * descriptors in it.
 */
static PUSB_CONFIGURATION_DESCRIPTOR StripClassDescriptors(
    PUSB_CONFIGURATION_DESCRIPTOR cfg)
{
    PUSB_CONFIGURATION_DESCRIPTOR out;
    PUCHAR src = (PUCHAR)cfg;
    PUCHAR end = src + cfg->wTotalLength;
    ULONG n = 0;

    out = (PUSB_CONFIGURATION_DESCRIPTOR)HidAlloc(cfg->wTotalLength);
    if (!out)
        return NULL;
    while (src + 2 <= end && src[0] >= 2 && src + src[0] <= end) {
        UCHAR t = src[1];
        if (t == USB_CONFIGURATION_DESCRIPTOR_TYPE ||
            t == USB_INTERFACE_DESCRIPTOR_TYPE ||
            t == USB_ENDPOINT_DESCRIPTOR_TYPE) {
            RtlCopyMemory((PUCHAR)out + n, src, src[0]);
            n += src[0];
        }
        src += src[0];
    }
    out->wTotalLength = (USHORT)n;
    return out;
}

static NTSTATUS SelectConfiguration(PDEVEXT de)
{
    USBD_INTERFACE_LIST_ENTRY list[HID_MAX_IF + 1];
    PIFSLOT who[HID_MAX_IF];
    PURB urb;
    NTSTATUS st = STATUS_NO_SUCH_DEVICE;
    ULONG i, k, n, take;

    de->SelCfg = StripClassDescriptors(de->ConfigDesc);
    if (!de->SelCfg)
        return STATUS_INSUFFICIENT_RESOURCES;
    HIDDBG(("[HIDUSB] configuration for USBD: %u -> %u bytes\n",
            de->ConfigDesc->wTotalLength, de->SelCfg->wTotalLength));

    take = de->NumSlots;
    for (;;) {
        n = 0;
        for (i = 0; i < take; i++) {
            PIFSLOT s = &de->Slots[i];
            PUSB_INTERFACE_DESCRIPTOR sel = USBD_ParseConfigurationDescriptorEx(
                de->SelCfg, de->SelCfg, s->IfDesc->bInterfaceNumber,
                s->IfDesc->bAlternateSetting, -1, -1, -1);
            if (!sel) {
                HIDDBG(("[HIDUSB] interface %u not found in the stripped configuration\n",
                        s->IfDesc->bInterfaceNumber));
                continue;
            }
            list[n].InterfaceDescriptor = sel;
            list[n].Interface = NULL;
            who[n] = s;
            n++;
        }
        list[n].InterfaceDescriptor = NULL;
        list[n].Interface = NULL;
        if (!n)
            return STATUS_NO_SUCH_DEVICE;

        urb = USBD_CreateConfigurationRequestEx(de->SelCfg, list);
        if (!urb)
            return STATUS_INSUFFICIENT_RESOURCES;
        for (k = 0; k < n; k++)
            for (i = 0; i < list[k].Interface->NumberOfPipes; i++)
                list[k].Interface->Pipes[i].MaximumTransferSize = PAGE_SIZE;
        st = CallUsbd(de, urb);
        if (NT_SUCCESS(st))
            break;
        HIDDBG(("[HIDUSB] SELECT_CONFIGURATION failed %x urb %x (%u interface(s))\n", st,
                (ULONG)urb->UrbHeader.Status, n));
        ExFreePool(urb);
        if (take <= 1)
            return st;
        take = 1;                               /* retry with the first interface only */
        de->NumSlots = 1;
    }

    de->ConfigHandle = urb->UrbSelectConfiguration.ConfigurationHandle;
    HIDDBG(("[HIDUSB] SELECT_CONFIGURATION ok: urb status %x handle %x, %u interface(s)\n",
            (ULONG)urb->UrbHeader.Status, (ULONG)de->ConfigHandle, n));
    for (k = 0; k < n; k++) {
        PUSBD_INTERFACE_INFORMATION ii = list[k].Interface;
        PIFSLOT s = who[k];

        HIDDBG(("[HIDUSB] if%u: %u pipe(s), interface info %u bytes\n",
                (ULONG)ii->InterfaceNumber, ii->NumberOfPipes, (ULONG)ii->Length));
        for (i = 0; i < ii->NumberOfPipes; i++) {
            PUSBD_PIPE_INFORMATION pi = &ii->Pipes[i];
            HIDDBG(("[HIDUSB]   pipe %u: ep %02x type %u maxpacket %u handle %x\n",
                    i, pi->EndpointAddress, (ULONG)pi->PipeType,
                    pi->MaximumPacketSize, (ULONG)pi->PipeHandle));
            if (pi->PipeType == UsbdPipeTypeInterrupt &&
                (pi->EndpointAddress & 0x80) && !s->InPipe) {
                s->InPipe = pi->PipeHandle;
                s->InMaxPacket = pi->MaximumPacketSize;
            }
        }
    }
    ExFreePool(urb);
    return st;
}

/* HID class request (no data stage) to an interface: SET_IDLE, SET_PROTOCOL. */
static NTSTATUS ClassRequest(PDEVEXT de, PIFSLOT s, UCHAR request, USHORT value)
{
    PURB urb = (PURB)HidAlloc(sizeof(struct _URB_CONTROL_VENDOR_OR_CLASS_REQUEST));
    NTSTATUS st;

    if (!urb)
        return STATUS_INSUFFICIENT_RESOURCES;
    UsbBuildVendorRequest(urb, URB_FUNCTION_CLASS_INTERFACE,
        (USHORT)sizeof(struct _URB_CONTROL_VENDOR_OR_CLASS_REQUEST),
        0, 0, request, value, s->IfDesc->bInterfaceNumber, NULL, NULL, 0, NULL);
    st = CallUsbd(de, urb);
    ExFreePool(urb);
    return st;
}

/* HID 1.11 appendix B.1 / B.2 boot interface descriptors.  Used when a boot
 * device refuses GET_DESCRIPTOR(Report) (the OSR2.1 USBD can only ask with a
 * device-recipient request; some receivers STALL it). */
static const UCHAR g_BootMouse[] = {
    0x05,0x01, 0x09,0x02, 0xA1,0x01, 0x09,0x01, 0xA1,0x00,
    0x05,0x09, 0x19,0x01, 0x29,0x03, 0x15,0x00, 0x25,0x01,
    0x95,0x03, 0x75,0x01, 0x81,0x02, 0x95,0x01, 0x75,0x05, 0x81,0x03,
    0x05,0x01, 0x09,0x30, 0x09,0x31, 0x15,0x81, 0x25,0x7F,
    0x75,0x08, 0x95,0x02, 0x81,0x06, 0xC0, 0xC0
};
static const UCHAR g_BootKeyboard[] = {
    0x05,0x01, 0x09,0x06, 0xA1,0x01, 0x05,0x07, 0x19,0xE0, 0x29,0xE7,
    0x15,0x00, 0x25,0x01, 0x75,0x01, 0x95,0x08, 0x81,0x02,
    0x95,0x01, 0x75,0x08, 0x81,0x01,
    0x95,0x05, 0x75,0x01, 0x05,0x08, 0x19,0x01, 0x29,0x05, 0x91,0x02,
    0x95,0x01, 0x75,0x03, 0x91,0x01,
    0x95,0x06, 0x75,0x08, 0x15,0x00, 0x25,0x65, 0x05,0x07, 0x19,0x00, 0x29,0x65, 0x81,0x00,
    0xC0
};

/*
 * Several HID interfaces in one device (keyboard + multimedia keys): HIDCLASS
 * wants one report descriptor, so the report ids are made unique and the
 * descriptors concatenated.  An interface without report ids gets one
 * (Report ID item in front of its descriptor); one with ids has them shifted.
 * Reports travel with the merged id and are routed back by it.
 *
 * Returns the highest merged id used, 0 if the descriptor cannot be merged.
 */
static ULONG MapDescriptor(PIFSLOT s, ULONG nextId)
{
    PUCHAR d = s->ReportDesc;
    ULONG len = s->ReportDescLen, pos, max = 0, sz;
    BOOLEAN found = FALSE;

    for (pos = 0; pos < len; ) {                /* any report id items? */
        UCHAR b = d[pos];
        if (b == 0xFE) {                        /* long item */
            if (pos + 2 > len) break;
            pos += 3 + d[pos + 1];
            continue;
        }
        sz = b & 3; if (sz == 3) sz = 4;
        if (b == 0x85 && pos + 1 < len) {
            found = TRUE;
            if (d[pos + 1] > max) max = d[pos + 1];
        }
        pos += 1 + sz;
    }

    s->UsesIds = found;
    if (!found) {
        if (nextId > 255) return 0;
        s->Mapped = (PUCHAR)HidAlloc(len + 2);
        if (!s->Mapped) return 0;
        s->Mapped[0] = 0x85;
        s->Mapped[1] = (UCHAR)nextId;
        RtlCopyMemory(s->Mapped + 2, d, len);
        s->MappedLen = len + 2;
        s->FixedId = (UCHAR)nextId;
        s->IdLo = s->IdHi = (UCHAR)nextId;
        return nextId;
    }
    if (max == 0 || nextId - 1 + max > 255) return 0;
    s->Mapped = (PUCHAR)HidAlloc(len);
    if (!s->Mapped) return 0;
    RtlCopyMemory(s->Mapped, d, len);
    s->MappedLen = len;
    s->IdAdd = (UCHAR)(nextId - 1);
    for (pos = 0; pos < len; ) {
        UCHAR b = s->Mapped[pos];
        if (b == 0xFE) {
            if (pos + 2 > len) break;
            pos += 3 + s->Mapped[pos + 1];
            continue;
        }
        sz = b & 3; if (sz == 3) sz = 4;
        if (b == 0x85 && pos + 1 < len)
            s->Mapped[pos + 1] = (UCHAR)(s->Mapped[pos + 1] + s->IdAdd);
        pos += 1 + sz;
    }
    s->IdLo = (UCHAR)(1 + s->IdAdd);
    s->IdHi = (UCHAR)(max + s->IdAdd);
    return max + s->IdAdd;
}

/* GET_DESCRIPTOR(Report) with an interface-recipient setup packet sent as a raw
 * control transfer: the only way to ask a particular interface when the USBD
 * has no GET_DESCRIPTOR_FROM_INTERFACE.  May not exist in the OSR2.1 USBD. */
static NTSTATUS ReportDescViaControl(PDEVEXT de, UCHAR ifnum, PUCHAR buf, ULONG len, PULONG got)
{
    PURB urb = (PURB)HidAlloc(sizeof(struct _URB_CONTROL_TRANSFER));
    NTSTATUS st;

    if (!urb)
        return STATUS_INSUFFICIENT_RESOURCES;
    urb->UrbHeader.Length = (USHORT)sizeof(struct _URB_CONTROL_TRANSFER);
    urb->UrbHeader.Function = URB_FUNCTION_CONTROL_TRANSFER;
    urb->UrbControlTransfer.PipeHandle = NULL;          /* the default pipe */
    urb->UrbControlTransfer.TransferFlags = USBD_TRANSFER_DIRECTION_IN | USBD_SHORT_TRANSFER_OK;
    urb->UrbControlTransfer.TransferBufferLength = len;
    urb->UrbControlTransfer.TransferBuffer = buf;
    urb->UrbControlTransfer.SetupPacket[0] = 0x81;      /* device-to-host, standard, interface */
    urb->UrbControlTransfer.SetupPacket[1] = 0x06;      /* GET_DESCRIPTOR */
    urb->UrbControlTransfer.SetupPacket[2] = 0x00;
    urb->UrbControlTransfer.SetupPacket[3] = USB_HID_REPORT_TYPE;
    urb->UrbControlTransfer.SetupPacket[4] = ifnum;
    urb->UrbControlTransfer.SetupPacket[5] = 0x00;
    urb->UrbControlTransfer.SetupPacket[6] = (UCHAR)(len & 0xFF);
    urb->UrbControlTransfer.SetupPacket[7] = (UCHAR)(len >> 8);
    st = CallUsbd(de, urb);
    if (got)
        *got = urb->UrbControlTransfer.TransferBufferLength;
    ExFreePool(urb);
    return st;
}

/* the report descriptor of one interface: the device's, else the boot one */
static void GetReportDescriptor(PDEVEXT de, PIFSLOT s)
{
    NTSTATUS st;
    ULONG got = 0, i, rlen = 0;
    PUSB_INTERFACE_DESCRIPTOR ifd = s->IfDesc;

    if (s->HidDesc) {
        for (i = 0; i < s->HidDesc->bNumDescriptors; i++)
            if (s->HidDesc->Descriptors[i].bDescriptorType == USB_HID_REPORT_TYPE)
                rlen = s->HidDesc->Descriptors[i].wDescriptorLength;
    }

    /* OSR2.1's USBD may not know URB_FUNCTION_GET_DESCRIPTOR_FROM_INTERFACE
     * (E1 log: STATUS_INVALID_PARAMETER); Win98's HIDUSB asks with
     * GET_DESCRIPTOR_FROM_DEVICE and the interface number as language ID,
     * so fall back to that. */
    if (rlen) {
        s->ReportDesc = (PUCHAR)HidAlloc(rlen);
        if (s->ReportDesc) {
            st = GetDescriptor(de, URB_FUNCTION_GET_DESCRIPTOR_FROM_INTERFACE,
                               USB_HID_REPORT_TYPE, 0, ifd->bInterfaceNumber,
                               s->ReportDesc, rlen, &got);
            HIDDBG(("[HIDUSB] if%u report descriptor via FROM_INTERFACE: status %x\n",
                    (ULONG)ifd->bInterfaceNumber, st));
            if (!NT_SUCCESS(st)) {
                RtlZeroMemory(s->ReportDesc, rlen);
                st = GetDescriptor(de, URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE,
                                   USB_HID_REPORT_TYPE, 0, ifd->bInterfaceNumber,
                                   s->ReportDesc, rlen, &got);
                HIDDBG(("[HIDUSB] if%u report descriptor via FROM_DEVICE: status %x\n",
                        (ULONG)ifd->bInterfaceNumber, st));
                /* a device-recipient request cannot name the interface: a device may
                 * answer every interface with the first one's descriptor */
                if (NT_SUCCESS(st)) {
                    ULONG j;
                    for (j = 0; j < s->Index; j++)
                        if (de->Slots[j].ReportDescLen == got && got != rlen &&
                            RtlCompareMemory(de->Slots[j].ReportDesc, s->ReportDesc, got) == got) {
                            HIDDBG(("[HIDUSB] if%u: that is interface %u's descriptor, not its own\n",
                                    (ULONG)ifd->bInterfaceNumber, j));
                            st = STATUS_UNSUCCESSFUL;
                        }
                }
                if (!NT_SUCCESS(st)) {
                    RtlZeroMemory(s->ReportDesc, rlen);
                    st = ReportDescViaControl(de, ifd->bInterfaceNumber, s->ReportDesc, rlen, &got);
                    HIDDBG(("[HIDUSB] if%u report descriptor via control transfer: status %x\n",
                            (ULONG)ifd->bInterfaceNumber, st));
                }
            }
            HIDDBG(("[HIDUSB] if%u report descriptor: %u of %u bytes\n",
                    (ULONG)ifd->bInterfaceNumber, got, rlen));
            if (NT_SUCCESS(st) && got) {
                s->ReportDescLen = got;
                LogHex("report-desc", s->ReportDesc, got);
            }
        }
    }

    if (!s->ReportDescLen && ifd->bInterfaceSubClass == 1 &&
        (ifd->bInterfaceProtocol == 1 || ifd->bInterfaceProtocol == 2)) {
        const UCHAR *bd = (ifd->bInterfaceProtocol == 2) ? g_BootMouse : g_BootKeyboard;
        ULONG bl = (ifd->bInterfaceProtocol == 2) ? sizeof(g_BootMouse) : sizeof(g_BootKeyboard);

        if (s->ReportDesc)
            ExFreePool(s->ReportDesc);
        s->ReportDesc = (PUCHAR)HidAlloc(bl);
        if (s->ReportDesc) {
            RtlCopyMemory(s->ReportDesc, bd, bl);
            s->ReportDescLen = bl;
            HIDDBG(("[HIDUSB] if%u: using the built-in boot %s report descriptor (%u bytes)\n",
                    (ULONG)ifd->bInterfaceNumber,
                    ifd->bInterfaceProtocol == 2 ? "mouse" : "keyboard", bl));
            st = ClassRequest(de, s, 0x0B, 0);          /* SET_PROTOCOL(boot) */
            HIDDBG(("[HIDUSB] SET_PROTOCOL(boot): %x\n", st));
        }
    }
}

/* the report length HIDCLASS reads from the HID descriptor */
static void PatchReportLength(PUSB_HID_DESCRIPTOR hd, ULONG len)
{
    ULONG i;

    if (!hd)
        return;
    for (i = 0; i < hd->bNumDescriptors; i++)
        if (hd->Descriptors[i].bDescriptorType == USB_HID_REPORT_TYPE)
            hd->Descriptors[i].wDescriptorLength = (USHORT)len;
}

static NTSTATUS StartDevice(PDEVEXT de)
{
    NTSTATUS st;
    ULONG got = 0, i, live = 0, nextId = 1, total = 0;
    PUCHAR p, end;

    HIDDBG(("[HIDUSB] start device\n"));

    de->DevDesc = (PUSB_DEVICE_DESCRIPTOR)HidAlloc(sizeof(USB_DEVICE_DESCRIPTOR));
    if (!de->DevDesc)
        return STATUS_INSUFFICIENT_RESOURCES;
    st = GetDescriptor(de, URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE,
                       USB_DEVICE_DESCRIPTOR_TYPE, 0, 0, de->DevDesc,
                       sizeof(USB_DEVICE_DESCRIPTOR), &got);
    if (!NT_SUCCESS(st)) {
        HIDDBG(("[HIDUSB] GET_DESCRIPTOR(device) failed %x\n", st));
        return st;
    }
    LogDeviceDescriptor(de->DevDesc);

    st = ReadConfiguration(de);
    if (!NT_SUCCESS(st)) {
        HIDDBG(("[HIDUSB] GET_DESCRIPTOR(config) failed %x\n", st));
        return st;
    }
    LogConfiguration(de);

    /* every HID interface (alternate setting 0), up to HID_MAX_IF */
    de->NumSlots = 0;
    p = (PUCHAR)de->ConfigDesc;
    end = p + de->ConfigDesc->wTotalLength;
    while (p + 2 <= end && p[0] != 0 && de->NumSlots < HID_MAX_IF) {
        if (p[1] == USB_INTERFACE_DESCRIPTOR_TYPE) {
            PUSB_INTERFACE_DESCRIPTOR ifd = (PUSB_INTERFACE_DESCRIPTOR)p;
            if (ifd->bInterfaceClass == USB_INTERFACE_CLASS_HID && ifd->bAlternateSetting == 0) {
                PIFSLOT s = &de->Slots[de->NumSlots];
                s->De = de;
                s->Index = de->NumSlots;
                s->IfDesc = ifd;
                s->HidDesc = FindHidDescriptor(de->ConfigDesc, ifd);
                HIDDBG(("[HIDUSB] using interface %u (boot subclass %u protocol %u)%s\n",
                        (ULONG)ifd->bInterfaceNumber, (ULONG)ifd->bInterfaceSubClass,
                        (ULONG)ifd->bInterfaceProtocol,
                        s->HidDesc ? "" : ", no HID class descriptor"));
                de->NumSlots++;
            }
        }
        p += p[0];
    }
    if (!de->NumSlots) {
        HIDDBG(("[HIDUSB] no HID interface in configuration\n"));
        return STATUS_NO_SUCH_DEVICE;
    }

    st = SelectConfiguration(de);
    if (!NT_SUCCESS(st))
        return st;

    for (i = 0; i < de->NumSlots; i++) {
        PIFSLOT s = &de->Slots[i];

        if (!s->InPipe)
            continue;                           /* no interrupt IN endpoint */
        GetReportDescriptor(de, s);
        if (!s->ReportDescLen) {
            HIDDBG(("[HIDUSB] if%u: no report descriptor, interface skipped\n",
                    (ULONG)s->IfDesc->bInterfaceNumber));
            continue;
        }
        st = ClassRequest(de, s, 0x0A, 0);      /* SET_IDLE(0): report only changes */
        HIDDBG(("[HIDUSB] if%u SET_IDLE(0): %x\n", (ULONG)s->IfDesc->bInterfaceNumber, st));
        s->Live = TRUE;
        live++;
    }

    if (live > 1) {                             /* merge the descriptors */
        de->Merged = TRUE;
        for (i = 0; i < de->NumSlots; i++) {
            PIFSLOT s = &de->Slots[i];
            ULONG top;

            if (!s->Live)
                continue;
            top = MapDescriptor(s, nextId);
            if (!top) {
                HIDDBG(("[HIDUSB] if%u: report ids cannot be merged, interface skipped\n",
                        (ULONG)s->IfDesc->bInterfaceNumber));
                s->Live = FALSE;
                continue;
            }
            nextId = top + 1;
            total += s->MappedLen;
        }
        de->ReportDesc = (PUCHAR)HidAlloc(total ? total : 1);
        if (!de->ReportDesc)
            return STATUS_INSUFFICIENT_RESOURCES;
        for (i = 0; i < de->NumSlots; i++) {
            PIFSLOT s = &de->Slots[i];
            if (!s->Live)
                continue;
            RtlCopyMemory(de->ReportDesc + de->ReportDescLen, s->Mapped, s->MappedLen);
            de->ReportDescLen += s->MappedLen;
            HIDDBG(("[HIDUSB] if%u -> merged report ids %u..%u (%s)\n",
                    (ULONG)s->IfDesc->bInterfaceNumber, (ULONG)s->IdLo, (ULONG)s->IdHi,
                    s->UsesIds ? "device ids shifted" : "id added"));
        }
        LogHex("merged-desc", de->ReportDesc, de->ReportDescLen);
    }

    for (i = 0; i < de->NumSlots; i++) {
        PIFSLOT s = &de->Slots[i];
        if (!s->Live)
            continue;
        if (!de->HidDesc) {
            de->HidDesc = s->HidDesc;
            if (!de->Merged) {                  /* the one live interface's own descriptor */
                de->ReportDesc = s->ReportDesc;
                de->ReportDescLen = s->ReportDescLen;
                s->ReportDesc = NULL;           /* owned by the device now */
            }
        }
        st = PollStart(s);
        HIDDBG(("[HIDUSB] if%u polling interrupt IN (maxpacket %u): %x\n",
                (ULONG)s->IfDesc->bInterfaceNumber, s->InMaxPacket, st));
    }
    if (de->HidDesc)
        PatchReportLength(de->HidDesc, de->ReportDescLen);
    else
        HIDDBG(("[HIDUSB] no usable HID interface\n"));
    de->Started = TRUE;
    return STATUS_SUCCESS;
}

static void FailPendingReads(PDEVEXT de, NTSTATUS st);

/* deconfigure: send the NULL SELECT_CONFIGURATION (not when the device is gone) */
static void StopDevice(PDEVEXT de, BOOLEAN deconfigure)
{
    PURB urb;
    ULONG i;

    TRC("U.stop begin", deconfigure);
    TRCB(3);
    de->Started = FALSE;
    for (i = 0; i < de->NumSlots; i++) {
        TRC("U.pollstop", i);
        PollStop(&de->Slots[i]);
        TRC("U.pollstop done", i);
    }
    FailPendingReads(de, STATUS_DEVICE_NOT_CONNECTED);
    TRC("U.reads failed", 0);
    TRCB(4);
    if (de->ConfigHandle && deconfigure) {
        urb = (PURB)HidAlloc(sizeof(struct _URB_SELECT_CONFIGURATION));
        if (urb) {
            UsbBuildSelectConfigurationRequest(urb,
                (USHORT)sizeof(struct _URB_SELECT_CONFIGURATION), NULL);
            TRC("U.deconfigure", 0);
            CallUsbd(de, urb);                  /* NULL descriptor = deconfigure */
            TRC("U.deconfigure done", 0);
            TRCB(5);
            ExFreePool(urb);
        }
    }
    de->ConfigHandle = NULL;
    for (i = 0; i < de->NumSlots; i++) {
        PIFSLOT s = &de->Slots[i];
        if (s->ReportDesc) ExFreePool(s->ReportDesc);
        if (s->Mapped)     ExFreePool(s->Mapped);
        RtlZeroMemory(s, sizeof(*s));
    }
    de->NumSlots = 0;
    de->Merged = FALSE;
    if (de->ReportDesc) { ExFreePool(de->ReportDesc); de->ReportDesc = NULL; }
    de->ReportDescLen = 0;
    if (de->SelCfg)     { ExFreePool(de->SelCfg);     de->SelCfg = NULL; }
    if (de->ConfigDesc) { ExFreePool(de->ConfigDesc); de->ConfigDesc = NULL; }
    if (de->DevDesc)    { ExFreePool(de->DevDesc);    de->DevDesc = NULL; }
    de->HidDesc = NULL;
    de->Started = FALSE;
    TRC("U.stop end", 0);
    TRCB(6);
}

/* ------------------------------------------------------------------ */
/* HID minidriver entry points (HIDPORT.H)                             */
/* ------------------------------------------------------------------ */

static NTSTATUS NTAPI SyncComplete(PDEVICE_OBJECT dev, PIRP irp, PVOID ctx)
{
    UNREFERENCED_PARAMETER(dev);
    UNREFERENCED_PARAMETER(irp);
    KeSetEvent((PKEVENT)ctx, 0, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

/* Send an IRP down and wait for the lower drivers to finish with it. */
static NTSTATUS ForwardAndWait(PDEVEXT de, PIRP irp)
{
    KEVENT event;
    NTSTATUS st;

    KeInitializeEvent(&event, NotificationEvent, FALSE);
    IoCopyCurrentIrpStackLocationToNext(irp);
    IoSetCompletionRoutine(irp, SyncComplete, &event, TRUE, TRUE, TRUE);
    st = IoCallDriver(de->TopOfStack, irp);
    if (st == STATUS_PENDING) {
        KeWaitForSingleObject(&event, Suspended, KernelMode, FALSE, NULL);
        st = irp->IoStatus.Status;
    }
    return st;
}

static NTSTATUS PassDown(PDEVEXT de, PIRP irp)
{
    IoSkipCurrentIrpStackLocation(irp);
    return IoCallDriver(de->TopOfStack, irp);
}

static NTSTATUS Complete(PIRP irp, NTSTATUS st, ULONG info)
{
    irp->IoStatus.Status = st;
    irp->IoStatus.Information = info;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return st;
}

/* ------------------------------------------------------------------ */
/* IOCTL_HID_READ_REPORT queue                                         */
/* ------------------------------------------------------------------ */

static VOID NTAPI ReadCancel(PDEVICE_OBJECT dev, PIRP irp)
{
    PDEVEXT de = MINI_EXT(dev);
    KIRQL irql;

    IoReleaseCancelSpinLock(irp->CancelIrql);
    KeAcquireSpinLock(&de->QLock, &irql);
    RemoveEntryList(&irp->Tail.Overlay.ListEntry);
    KeReleaseSpinLock(&de->QLock, irql);
    Complete(irp, STATUS_CANCELLED, 0);
}

/* Take the oldest waiting reader off the list (queue lock held). */
static PIRP TakeReader(PDEVEXT de)
{
    PLIST_ENTRY e;
    PIRP irp;

    while (!IsListEmpty(&de->ReadIrps)) {
        e = de->ReadIrps.Flink;
        RemoveEntryList(e);
        irp = CONTAINING_RECORD(e, IRP, Tail.Overlay.ListEntry);
        if (IoSetCancelRoutine(irp, NULL))
            return irp;                     /* ours to complete */
        InitializeListHead(e);              /* its cancel routine is running */
    }
    return NULL;
}

/* A report came in: complete a waiting reader, or keep it in the ring. */
static void DeliverReport(PDEVEXT de, PUCHAR data, ULONG len)
{
    KIRQL irql;
    PIRP irp;

    if (len > HID_MAX_REPORT)
        len = HID_MAX_REPORT;
    KeAcquireSpinLock(&de->QLock, &irql);
    irp = TakeReader(de);
    if (!irp) {
        ULONG slot;
        if (de->RingCount == HID_RING_SLOTS) {          /* full: drop the oldest */
            de->RingHead = (de->RingHead + 1) % HID_RING_SLOTS;
            de->RingCount--;
        }
        slot = (de->RingHead + de->RingCount) % HID_RING_SLOTS;
        RtlCopyMemory(de->Ring[slot], data, len);
        de->RingLen[slot] = (UCHAR)len;
        de->RingCount++;
    }
    KeReleaseSpinLock(&de->QLock, irql);

    if (irp) {
        ULONG n = IoGetCurrentIrpStackLocation(irp)->Parameters.DeviceIoControl.OutputBufferLength;
        if (n > len)
            n = len;
        RtlCopyMemory(irp->UserBuffer, data, n);
        Complete(irp, STATUS_SUCCESS, n);
    }
}

static void FailPendingReads(PDEVEXT de, NTSTATUS st)
{
    KIRQL irql;
    PIRP irp;

    for (;;) {
        KeAcquireSpinLock(&de->QLock, &irql);
        irp = TakeReader(de);
        KeReleaseSpinLock(&de->QLock, irql);
        if (!irp)
            break;
        Complete(irp, st, 0);
    }
}

static NTSTATUS ReadReport(PDEVEXT de, PIRP irp, ULONG outLen)
{
    KIRQL irql;

    KeAcquireSpinLock(&de->QLock, &irql);
    if (de->RingCount) {                                /* a report is waiting */
        ULONG n = de->RingLen[de->RingHead];
        if (n > outLen)
            n = outLen;
        RtlCopyMemory(irp->UserBuffer, de->Ring[de->RingHead], n);
        de->RingHead = (de->RingHead + 1) % HID_RING_SLOTS;
        de->RingCount--;
        KeReleaseSpinLock(&de->QLock, irql);
        return Complete(irp, STATUS_SUCCESS, n);
    }
    if (!de->Started) {
        KeReleaseSpinLock(&de->QLock, irql);
        return Complete(irp, STATUS_DEVICE_NOT_CONNECTED, 0);
    }

    /* nothing yet: keep the IRP until the next report */
    IoMarkIrpPending(irp);
    InsertTailList(&de->ReadIrps, &irp->Tail.Overlay.ListEntry);
    IoSetCancelRoutine(irp, ReadCancel);
    if (irp->Cancel && IoSetCancelRoutine(irp, NULL)) {
        RemoveEntryList(&irp->Tail.Overlay.ListEntry);
        KeReleaseSpinLock(&de->QLock, irql);
        Complete(irp, STATUS_CANCELLED, 0);
        return STATUS_PENDING;                          /* completed + marked pending */
    }
    KeReleaseSpinLock(&de->QLock, irql);
    return STATUS_PENDING;
}

/* PnP IRPs arrive from HIDCLASS as a layer between it and the bus below. */
static NTSTATUS NTAPI HidUsbPnp(PDEVICE_OBJECT dev, PIRP irp)
{
    PDEVEXT de = MINI_EXT(dev);
    PIO_STACK_LOCATION sl = IoGetCurrentIrpStackLocation(irp);
    NTSTATUS st;

    HIDDBG(("[HIDUSB] PnP minor %x\n", (ULONG)sl->MinorFunction));
    TRC("U.pnp", sl->MinorFunction);
    switch (sl->MinorFunction) {

    case IRP_MN_START_DEVICE:
        st = ForwardAndWait(de, irp);
        if (NT_SUCCESS(st)) {
            st = StartDevice(de);
            if (!NT_SUCCESS(st)) {
                HIDDBG(("[HIDUSB] start failed %x\n", st));
                StopDevice(de, TRUE);
            }
        }
        return Complete(irp, st, 0);

    case IRP_MN_STOP_DEVICE:
        HIDDBG(("[HIDUSB] stop device (%u+%u reports)\n", de->Slots[0].Reports, de->Slots[1].Reports));
        StopDevice(de, TRUE);
        irp->IoStatus.Status = STATUS_SUCCESS;
        return PassDown(de, irp);

    case IRP_MN_SURPRISE_REMOVAL:
    case IRP_MN_REMOVE_DEVICE: {
        /*
         * What this REMOVE looks like, for the trace only.  It is NOT used to change
         * the behaviour: "no QUERY_REMOVE and the device is alive" also fits other
         * cases (a removal from the Device Manager, an unplug whose polling error
         * has not been seen yet), and skipping the bus driver there would break it.
         * The only reliable shutdown signal is the one HIDCLASS gets from the VxD
         * clients at KERNEL32_SHUTDOWN (g_Shutdown); with it HIDCLASS never gets
         * here (see FdoRemove).
         */
        BOOLEAN gone = (BOOLEAN)(sl->MinorFunction == IRP_MN_SURPRISE_REMOVAL);
        ULONG k;

        for (k = 0; k < de->NumSlots; k++)
            if (de->Slots[k].PollErrors)
                gone = TRUE;
        HIDDBG(("[HIDUSB] remove device (gone %u, query seen %u)\n", (ULONG)gone,
                (ULONG)de->QuerySeen));
        TRC("U.remove gone", gone);
        TRC("U.remove query seen", de->QuerySeen);
        StopDevice(de, FALSE);          /* never a URB here */
        irp->IoStatus.Status = STATUS_SUCCESS;
        return PassDown(de, irp);       /* HIDCLASS detaches and deletes the FDO */
    }

    case IRP_MN_QUERY_STOP_DEVICE:
    case IRP_MN_QUERY_REMOVE_DEVICE:
        de->QuerySeen = TRUE;
        irp->IoStatus.Status = STATUS_SUCCESS;
        return PassDown(de, irp);

    case IRP_MN_CANCEL_STOP_DEVICE:
    case IRP_MN_CANCEL_REMOVE_DEVICE:
        de->QuerySeen = FALSE;
        irp->IoStatus.Status = STATUS_SUCCESS;
        return PassDown(de, irp);

    default:
        return PassDown(de, irp);
    }
}

static NTSTATUS NTAPI HidUsbPower(PDEVICE_OBJECT dev, PIRP irp)
{
    HIDDBG(("[HIDUSB] power minor %x\n", (ULONG)IoGetCurrentIrpStackLocation(irp)->MinorFunction));
    TRC("U.pwr", IoGetCurrentIrpStackLocation(irp)->MinorFunction);
    /* HIDCLASS passes power IRPs down itself; OSR2.1 has no power manager. */
    return PassDown(MINI_EXT(dev), irp);
}

/*
 * A report through the control pipe: GET_REPORT (in) or SET_REPORT (out) of the
 * type 1 = input, 2 = output, 3 = feature.  The packet is filled by HIDCLASS as
 * for IOCTL_HID_WRITE_REPORT (with a report id the buffer starts with it, with
 * id 0 it does not); the data goes after the id byte.  *done = data bytes moved.
 */
static NTSTATUS ClassReport(PDEVEXT de, PHID_XFER_PACKET pkt, UCHAR type, BOOLEAN in, PULONG done)
{
    PIFSLOT s = NULL;
    PUCHAR data;
    ULONG dlen, i;
    UCHAR origId;
    PURB urb;
    NTSTATUS st;

    *done = 0;
    for (i = 0; i < de->NumSlots; i++) {                /* which interface owns this id */
        PIFSLOT c = &de->Slots[i];
        if (!c->Live)
            continue;
        if (!de->Merged || (pkt->reportId >= c->IdLo && pkt->reportId <= c->IdHi)) {
            s = c;
            break;
        }
    }
    if (!s)
        return STATUS_INVALID_PARAMETER;
    origId = pkt->reportId;
    if (de->Merged)
        origId = s->UsesIds ? (UCHAR)(pkt->reportId - s->IdAdd) : 0;
    data = pkt->reportBuffer;
    dlen = pkt->reportBufferLen;
    if (pkt->reportId && dlen) {                        /* the id travels in wValue */
        data++;
        dlen--;
    }
    urb = (PURB)HidAlloc(sizeof(struct _URB_CONTROL_VENDOR_OR_CLASS_REQUEST));
    if (!urb)
        return STATUS_INSUFFICIENT_RESOURCES;
    UsbBuildVendorRequest(urb, URB_FUNCTION_CLASS_INTERFACE,
        (USHORT)sizeof(struct _URB_CONTROL_VENDOR_OR_CLASS_REQUEST),
        in ? (USBD_TRANSFER_DIRECTION_IN | USBD_SHORT_TRANSFER_OK) : 0, 0,
        in ? 0x01 : 0x09,                               /* GET_REPORT / SET_REPORT */
        (USHORT)(((USHORT)type << 8) | origId), s->IfDesc->bInterfaceNumber,
        data, NULL, dlen, NULL);
    st = CallUsbd(de, urb);
    if (NT_SUCCESS(st))
        *done = urb->UrbControlVendorClassRequest.TransferBufferLength;
    HIDDBG(("[HIDUSB] %s_REPORT if%u type %u id %x len %x: %x\n", in ? "GET" : "SET",
            (ULONG)s->IfDesc->bInterfaceNumber, (ULONG)type, (ULONG)origId, dlen, st));
    ExFreePool(urb);
    return st;
}

/* a string descriptor as a NUL-terminated wide string; lang 0 = the device's first language */
static NTSTATUS GetStringW(PDEVEXT de, UCHAR index, USHORT lang, PWCHAR out, ULONG outBytes,
                           PULONG written)
{
    UCHAR tmp[256];
    ULONG got = 0, n, chars;
    NTSTATUS st;

    *written = 0;
    if (outBytes < sizeof(WCHAR))
        return STATUS_BUFFER_TOO_SMALL;
    if (!lang) {
        st = GetDescriptor(de, URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE,
                           USB_STRING_DESCRIPTOR_TYPE, 0, 0, tmp, sizeof(tmp), &got);
        lang = (NT_SUCCESS(st) && got >= 4) ? (USHORT)(tmp[2] | (tmp[3] << 8)) : 0x0409;
    }
    st = GetDescriptor(de, URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE,
                       USB_STRING_DESCRIPTOR_TYPE, index, lang, tmp, sizeof(tmp), &got);
    if (!NT_SUCCESS(st))
        return st;
    if (got < 2 || tmp[1] != USB_STRING_DESCRIPTOR_TYPE)
        return STATUS_DEVICE_DATA_ERROR;
    n = tmp[0];
    if (n > got)
        n = got;
    chars = (n >= 2) ? (n - 2) / 2 : 0;
    if ((chars + 1) * sizeof(WCHAR) > outBytes)
        chars = outBytes / sizeof(WCHAR) - 1;
    RtlCopyMemory(out, tmp + 2, chars * sizeof(WCHAR));
    out[chars] = 0;
    *written = (chars + 1) * sizeof(WCHAR);
    return STATUS_SUCCESS;
}

/* IRP_MJ_INTERNAL_DEVICE_CONTROL: the HIDPORT.H requests from HIDCLASS.
 * METHOD_NEITHER: output goes to Irp->UserBuffer. */
static NTSTATUS NTAPI HidUsbIoctl(PDEVICE_OBJECT dev, PIRP irp)
{
    PDEVEXT de = MINI_EXT(dev);
    PIO_STACK_LOCATION sl = IoGetCurrentIrpStackLocation(irp);
    ULONG outLen = sl->Parameters.DeviceIoControl.OutputBufferLength;
    ULONG n, info = 0;
    NTSTATUS st = STATUS_SUCCESS;

    switch (sl->Parameters.DeviceIoControl.IoControlCode) {

    case IOCTL_HID_GET_DEVICE_DESCRIPTOR:
        if (!de->HidDesc) {
            st = STATUS_DEVICE_NOT_READY;
        } else {
            n = de->HidDesc->bLength;
            if (n > outLen) n = outLen;
            RtlCopyMemory(irp->UserBuffer, de->HidDesc, n);
            info = n;
        }
        break;

    case IOCTL_HID_GET_REPORT_DESCRIPTOR:
        if (!de->ReportDesc) {
            st = STATUS_DEVICE_NOT_READY;
        } else {
            n = de->ReportDescLen;
            if (n > outLen) n = outLen;
            RtlCopyMemory(irp->UserBuffer, de->ReportDesc, n);
            info = n;
        }
        break;

    case IOCTL_HID_GET_DEVICE_ATTRIBUTES:
        if (!de->DevDesc) {
            st = STATUS_DEVICE_NOT_READY;
        } else if (outLen < sizeof(HID_DEVICE_ATTRIBUTES)) {
            st = STATUS_BUFFER_TOO_SMALL;
        } else {
            PHID_DEVICE_ATTRIBUTES a = (PHID_DEVICE_ATTRIBUTES)irp->UserBuffer;
            RtlZeroMemory(a, sizeof(*a));
            a->Size = sizeof(*a);
            a->VendorID = de->DevDesc->idVendor;
            a->ProductID = de->DevDesc->idProduct;
            a->VersionNumber = de->DevDesc->bcdDevice;
            info = sizeof(*a);
        }
        break;

    case IOCTL_HID_READ_REPORT:
        return ReadReport(de, irp, outLen);

    case IOCTL_HID_WRITE_REPORT:                /* output report: SET_REPORT(Output) */
    case IOCTL_HIDUSB_SET_FEATURE:              /* SET_REPORT(Feature) */
    case IOCTL_HIDUSB_GET_FEATURE:              /* GET_REPORT(Feature) */
    case IOCTL_HIDUSB_GET_INPUT: {              /* GET_REPORT(Input) */
        ULONG code = sl->Parameters.DeviceIoControl.IoControlCode;
        PHID_XFER_PACKET pkt = (PHID_XFER_PACKET)irp->UserBuffer;

        if (!pkt || outLen < sizeof(*pkt)) {
            st = STATUS_INVALID_PARAMETER;
            break;
        }
        if (!de->Started) {
            st = STATUS_DEVICE_NOT_READY;       /* suspended, or gone */
            break;
        }
        st = ClassReport(de, pkt,
                         (UCHAR)(code == IOCTL_HID_WRITE_REPORT ? 2 :
                                 code == IOCTL_HIDUSB_GET_INPUT ? 1 : 3),
                         (BOOLEAN)(code == IOCTL_HIDUSB_GET_FEATURE || code == IOCTL_HIDUSB_GET_INPUT),
                         &n);
        info = n;
        break; }

    case IOCTL_HID_GET_STRING: {
        /* in (Type3InputBuffer): HID_STRING_ID_I* of the device descriptor, or
         * 0x80000000 | (language id << 8) | string index */
        ULONG sel = (ULONG)sl->Parameters.DeviceIoControl.Type3InputBuffer;
        UCHAR index;
        USHORT lang = 0;

        if (!de->DevDesc || !de->Started) {
            st = STATUS_DEVICE_NOT_READY;
            break;
        }
        if (sel == HID_STRING_ID_IMANUFACTURER)
            index = de->DevDesc->iManufacturer;
        else if (sel == HID_STRING_ID_IPRODUCT)
            index = de->DevDesc->iProduct;
        else if (sel == HID_STRING_ID_ISERIALNUMBER)
            index = de->DevDesc->iSerialNumber;
        else if (sel & 0x80000000UL) {
            index = (UCHAR)(sel & 0xFF);
            lang = (USHORT)((sel >> 8) & 0xFFFF);
        } else {
            st = STATUS_INVALID_PARAMETER;
            break;
        }
        if (!index) {
            st = STATUS_INVALID_PARAMETER;      /* the device has no such string */
            break;
        }
        st = GetStringW(de, index, lang, (PWCHAR)irp->UserBuffer, outLen, &n);
        info = n;
        break; }

    case IOCTL_HIDUSB_RESTART:
        /* back from a suspend: the device was stopped by SURPRISE_REMOVAL, configure it again */
        if (de->Started) {
            HIDDBG(("[HIDUSB] restart: already running\n"));
            break;
        }
        st = StartDevice(de);
        HIDDBG(("[HIDUSB] restart after suspend: %x\n", st));
        if (!NT_SUCCESS(st))
            StopDevice(de, FALSE);
        break;

    case IOCTL_HID_ACTIVATE_DEVICE:
    case IOCTL_HID_DEACTIVATE_DEVICE:
        break;

    default:
        HIDDBG(("[HIDUSB] unsupported internal IOCTL %x\n",
                sl->Parameters.DeviceIoControl.IoControlCode));
        st = STATUS_NOT_SUPPORTED;
        break;
    }
    return Complete(irp, st, info);
}

/* HIDCLASS made the FDO and attached it; we only set up our extension. */
static NTSTATUS NTAPI HidUsbAddDevice(PDRIVER_OBJECT drv, PDEVICE_OBJECT fdo)
{
    PHID_DEVICE_EXTENSION hx = (PHID_DEVICE_EXTENSION)fdo->DeviceExtension;
    PDEVEXT de = MINI_EXT(fdo);

    UNREFERENCED_PARAMETER(drv);
    HIDDBG(("[HIDUSB] AddDevice fdo=%x pdo=%x\n", (ULONG)fdo, (ULONG)hx->PhysicalDeviceObject));
    RtlZeroMemory(de, sizeof(DEVEXT));
    KeInitializeSpinLock(&de->QLock);
    InitializeListHead(&de->ReadIrps);
    de->Self = fdo;
    de->Pdo = hx->PhysicalDeviceObject;
    de->TopOfStack = hx->NextDeviceObject;
    return STATUS_SUCCESS;
}

static VOID NTAPI HidUsbUnload(PDRIVER_OBJECT drv)
{
    UNREFERENCED_PARAMETER(drv);
    HIDDBG(("[HIDUSB] unload\n"));
}

NTSTATUS NTAPI DriverEntry(PDRIVER_OBJECT drv, PUNICODE_STRING regPath)
{
    HID_MINIDRIVER_REGISTRATION reg;

    HIDDBG(("[HIDUSB] DriverEntry (HIDUSB H1, built " __DATE__ " " __TIME__ ")\n"));

    /* everything else is HIDCLASS's once we have registered */
    drv->MajorFunction[IRP_MJ_INTERNAL_DEVICE_CONTROL] = HidUsbIoctl;
    drv->MajorFunction[IRP_MJ_PNP]                     = HidUsbPnp;
    drv->MajorFunction[IRP_MJ_POWER]                   = HidUsbPower;
    drv->DriverExtension->AddDevice                    = HidUsbAddDevice;
    drv->DriverUnload                                  = HidUsbUnload;

    RtlZeroMemory(&reg, sizeof(reg));
    reg.Revision            = HID_REVISION;
    reg.DriverObject        = drv;
    reg.RegistryPath        = regPath;
    reg.DeviceExtensionSize = sizeof(DEVEXT);
    reg.DevicesArePolled    = FALSE;            /* interrupt endpoint, not polled */
    return HidRegisterMinidriver(&reg);
}

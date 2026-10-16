/*
 * hidpdo.c - HIDCLASS.SYS: the child PDO of each top-level collection
 * (what KBDHID.VXD / MOUHID.VXD / HID.DLL talk to).  See hidcls.h.
 */
#include "hidcls.h"

/*
 * The number of a collection (\\.\HIDn, \Device\HIDCOLn): the lowest free one of
 * HID_MAX_PORTS.  It is given back when the PDO is really deleted (the device name
 * is taken until then), so plugging a device in and out again and again never runs
 * out of numbers - the clients only look at 0..HID_MAX_PORTS-1.
 * (A zero-filled KSPIN_LOCK is an initialized one.)
 */
static ULONG       g_SerialMap[(HID_MAX_PORTS + 31) / 32];
static KSPIN_LOCK  g_SerialLock;

static LONG AllocSerial(VOID)
{
    KIRQL irql;
    ULONG n;
    LONG found = -1;

    KeAcquireSpinLock(&g_SerialLock, &irql);
    for (n = 0; n < HID_MAX_PORTS; n++)
        if (!(g_SerialMap[n / 32] & (1UL << (n % 32)))) {
            g_SerialMap[n / 32] |= 1UL << (n % 32);
            found = (LONG)n;
            break;
        }
    KeReleaseSpinLock(&g_SerialLock, irql);
    return found;
}

static VOID FreeSerial(ULONG n)
{
    KIRQL irql;

    if (n >= HID_MAX_PORTS)
        return;
    KeAcquireSpinLock(&g_SerialLock, &irql);
    g_SerialMap[n / 32] &= ~(1UL << (n % 32));
    KeReleaseSpinLock(&g_SerialLock, irql);
}

/* ------------------------------------------------------------------ */
/* small string builders (no CRT, no %ws)                              */
/* ------------------------------------------------------------------ */

static VOID PutStr(PWCHAR b, PULONG n, const char *s)
{
    while (*s)
        b[(*n)++] = (WCHAR)(UCHAR)*s++;
}

static VOID PutHex(PWCHAR b, PULONG n, ULONG v, int digits)
{
    static const char hex[] = "0123456789ABCDEF";
    int i;
    for (i = digits - 1; i >= 0; i--)
        b[(*n)++] = (WCHAR)(UCHAR)hex[(v >> (i * 4)) & 0xF];
}

static VOID PutDec(PWCHAR b, PULONG n, ULONG v)
{
    char tmp[12];
    int i = 0;
    if (v == 0) tmp[i++] = '0';
    while (v) { tmp[i++] = (char)('0' + v % 10); v /= 10; }
    while (i) b[(*n)++] = (WCHAR)(UCHAR)tmp[--i];
}

/* ------------------------------------------------------------------ */
/* creating / dropping the children                                    */
/* ------------------------------------------------------------------ */

static NTSTATUS MakeChild(PHIDFDO x, ULONG index, ULONG count,
                          PHIDP_COLLECTION_DESC cd)
{
    PDEVICE_OBJECT pdo;
    PHIDPDO p;
    UNICODE_STRING name;
    ULONG n;
    NTSTATUS st;
    ULONG serial;
    LONG sn = AllocSerial();
    WCHAR nm[40];

    if (sn < 0) {
        HIDDBG(("[HIDCLASS] no free \\\\.\\HIDn: all %u numbers are in use\n", (ULONG)HID_MAX_PORTS));
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    serial = (ULONG)sn;
    n = 0;
    PutStr(nm, &n, "\\Device\\HIDCOL");
    PutDec(nm, &n, serial);
    nm[n] = 0;
    name.Buffer = nm;
    name.Length = (USHORT)(n * 2);
    name.MaximumLength = (USHORT)(n * 2 + 2);

    st = IoCreateDevice(x->Self->DriverObject, sizeof(HIDPDO), &name,
                        FILE_DEVICE_UNKNOWN, 0, FALSE, &pdo);
    if (!NT_SUCCESS(st)) {
        FreeSerial(serial);
        return st;
    }
    p = (PHIDPDO)pdo->DeviceExtension;
    RtlZeroMemory(p, sizeof(HIDPDO));
    p->Kind = KIND_PDO;
    p->Self = pdo;
    p->Fdo = x;
    p->Serial = serial;
    p->Index = index;
    p->NumCollections = count;
    p->Attr = x->Attr;
    p->UsagePage = cd->UsagePage;
    p->Usage = cd->Usage;
    p->InputLength = cd->InputLength;
    p->OutputLength = cd->OutputLength;
    p->FeatureLength = cd->FeatureLength;
    p->PreparsedLength = cd->PreparsedDataLength;
    p->Preparsed = (PHIDP_PREPARSED_DATA)ExAllocatePoolWithTag(NonPagedPool,
                        cd->PreparsedDataLength, HIDCLS_TAG);
    if (!p->Preparsed) {
        IoDeleteDevice(pdo);
        FreeSerial(serial);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlCopyMemory(p->Preparsed, cd->PreparsedData, cd->PreparsedDataLength);
    p->Present = TRUE;
    KeInitializeSpinLock(&p->QLock);
    InitializeListHead(&p->Reads);
    InitializeListHead(&p->Clients);

    RtlCopyMemory(p->DeviceName, nm, n * 2 + 2);
    n = 0;
    PutStr(p->LinkName, &n, "\\DosDevices\\HID");
    PutDec(p->LinkName, &n, serial);
    p->LinkName[n] = 0;

    pdo->Flags |= DO_POWER_PAGABLE | DO_BUFFERED_IO;
    pdo->Flags &= ~DO_DEVICE_INITIALIZING;
    x->Child[index] = pdo;
    HIDDBG(("[HIDCLASS] collection %u: usage page %04x usage %04x, reports in/out/feature %u/%u/%u bytes -> \\\\.\\HID%u (PDO %x)\n",
            index, (ULONG)cd->UsagePage, (ULONG)cd->Usage, (ULONG)cd->InputLength,
            (ULONG)cd->OutputLength, (ULONG)cd->FeatureLength, serial, (ULONG)pdo));
    return STATUS_SUCCESS;
}

NTSTATUS HidCreateChildren(PHIDFDO x)
{
    UCHAR hdbuf[64];
    PHID_DESCRIPTOR hd = (PHID_DESCRIPTOR)hdbuf;
    PUCHAR rep = NULL;
    ULONG replen = 0, i;
    HIDP_DEVICE_DESC desc;
    NTSTATUS st;

    RtlZeroMemory(&x->Attr, sizeof(x->Attr));
    x->Attr.Size = sizeof(x->Attr);
    st = HidCallMini(x, IOCTL_HID_GET_DEVICE_ATTRIBUTES, &x->Attr, sizeof(x->Attr), NULL, 0);
    if (!NT_SUCCESS(st)) {
        HIDDBG(("[HIDCLASS] GET_DEVICE_ATTRIBUTES failed %x\n", st));
        return st;
    }
    HIDDBG(("[HIDCLASS] device VID %04x PID %04x version %04x\n",
            (ULONG)x->Attr.VendorID, (ULONG)x->Attr.ProductID, (ULONG)x->Attr.VersionNumber));

    RtlZeroMemory(hdbuf, sizeof(hdbuf));
    st = HidCallMini(x, IOCTL_HID_GET_DEVICE_DESCRIPTOR, hdbuf, sizeof(hdbuf), NULL, 0);
    if (!NT_SUCCESS(st)) {
        HIDDBG(("[HIDCLASS] GET_DEVICE_DESCRIPTOR failed %x\n", st));
        return st;
    }
    for (i = 0; i < hd->bNumDescriptors && i < 6; i++) {
        if (hd->DescriptorList[i].bReportType == HID_REPORT_DESCRIPTOR_TYPE) {
            replen = hd->DescriptorList[i].wReportLength;
            break;
        }
    }
    if (!replen) {
        HIDDBG(("[HIDCLASS] no report descriptor in the HID descriptor\n"));
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    rep = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, replen, HIDCLS_TAG);
    if (!rep)
        return STATUS_INSUFFICIENT_RESOURCES;
    st = HidCallMini(x, IOCTL_HID_GET_REPORT_DESCRIPTOR, rep, replen, NULL, 0);
    if (!NT_SUCCESS(st)) {
        HIDDBG(("[HIDCLASS] GET_REPORT_DESCRIPTOR failed %x\n", st));
        ExFreePool(rep);
        return st;
    }

    st = HidP_GetCollectionDescription(rep, replen, NonPagedPool, &desc);
    ExFreePool(rep);
    if (!NT_SUCCESS(st)) {
        HIDDBG(("[HIDCLASS] report descriptor does not parse: %x\n", st));
        return st;
    }
    HIDDBG(("[HIDCLASS] %u top-level collection(s)\n", desc.CollectionDescLength));

    /* report id -> collection, and the read buffer sized for the biggest report */
    RtlFillMemory(x->IdToCol, sizeof(x->IdToCol), 0xFF);
    x->UsesIds = FALSE;
    for (i = 0; i < desc.ReportIDsLength; i++) {
        x->IdToCol[desc.ReportIDs[i].ReportID] = (UCHAR)(desc.ReportIDs[i].CollectionNumber - 1);
        if (desc.ReportIDs[i].ReportID)
            x->UsesIds = TRUE;
    }
    x->ReadBufLen = 8;
    for (i = 0; i < desc.CollectionDescLength; i++)
        if (desc.CollectionDesc[i].InputLength > x->ReadBufLen)
            x->ReadBufLen = desc.CollectionDesc[i].InputLength;
    if (x->ReadBufLen > HID_MAX_REPORT)
        x->ReadBufLen = HID_MAX_REPORT;
    if (!x->ReadBuf)
        x->ReadBuf = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, x->ReadBufLen, HIDCLS_TAG);
    if (!x->ReadIrp)
        x->ReadIrp = IoAllocateIrp(x->Self->StackSize, FALSE);
    if (!x->ReadBuf || !x->ReadIrp) {
        HidP_FreeCollectionDescription(&desc);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    x->NumChildren = 0;
    for (i = 0; i < desc.CollectionDescLength && i < HID_MAX_COLLECTIONS; i++) {
        st = MakeChild(x, i, desc.CollectionDescLength, &desc.CollectionDesc[i]);
        if (!NT_SUCCESS(st))
            break;
        x->NumChildren = i + 1;
    }
    HidP_FreeCollectionDescription(&desc);
    if (!NT_SUCCESS(st)) {
        HidDropChildren(x);
        return st;
    }
    return STATUS_SUCCESS;
}

/*
 * Child PDOs that PnP still references when they are removed.  IoDeleteDevice
 * bugchecks (0x36, DEVICE_REFERENCE_COUNT_NOT_ZERO) on a referenced device
 * object, and PnP lets go of its reference only after the REMOVE has returned.
 * So such a PDO waits here and is deleted by a later sweep (next device, next
 * removal) once the count is 0.
 */
#define HID_MAX_ZOMBIES     16
static PDEVICE_OBJECT g_Zombie[HID_MAX_ZOMBIES];

VOID HidSweepZombies(VOID)
{
    ULONG i;

    for (i = 0; i < HID_MAX_ZOMBIES; i++) {
        PDEVICE_OBJECT pdo = g_Zombie[i];
        if (pdo && pdo->ReferenceCount == 0) {
            ULONG serial = ((PHIDPDO)pdo->DeviceExtension)->Serial;
            g_Zombie[i] = NULL;
            HIDDBG(("[HIDCLASS] delete old collection PDO %x (\\\\.\\HID%u free again)\n", (ULONG)pdo, serial));
            IoDeleteDevice(pdo);
            FreeSerial(serial);
        }
    }
}

static VOID FreeChild(PDEVICE_OBJECT pdo)
{
    PHIDPDO p = (PHIDPDO)pdo->DeviceExtension;
    ULONG i;

    if (p->Zombie)
        return;                                 /* already waiting for the sweep */
    HidSweepZombies();
    if (p->LinkMade) {
        UNICODE_STRING link;
        RtlInitUnicodeString(&link, p->LinkName);
        IoDeleteSymbolicLink(&link);
        p->LinkMade = FALSE;
    }
    if (p->Preparsed) {
        ExFreePool(p->Preparsed);
        p->Preparsed = NULL;
    }
    while (!IsListEmpty(&p->Clients)) {
        PLIST_ENTRY ce = RemoveHeadList(&p->Clients);
        ExFreePool(CONTAINING_RECORD(ce, HIDOPEN, Link));
    }
    HIDDBG(("[HIDCLASS] delete collection PDO %x (reference count %d)\n",
            (ULONG)pdo, pdo->ReferenceCount));
    TRC("H.delete PDO", pdo);
    TRC("H.delete PDO ref", pdo->ReferenceCount);
    if (pdo->ReferenceCount != 0) {
        for (i = 0; i < HID_MAX_ZOMBIES; i++)
            if (!g_Zombie[i]) {
                g_Zombie[i] = pdo;
                p->Zombie = TRUE;
                HIDDBG(("[HIDCLASS] PDO %x is still referenced: deleted later\n", (ULONG)pdo));
                return;
            }
        HIDDBG(("[HIDCLASS] PDO %x is still referenced: kept\n", (ULONG)pdo));
        return;                                 /* list full: leave it, no blue screen */
    }
    {
        ULONG serial = p->Serial;               /* the extension goes with the device */

        IoDeleteDevice(pdo);
        FreeSerial(serial);                     /* \\.\HIDn is free again */
    }
}

/* The parent is going away: children PnP still knows about stay until it
 * removes them (marked gone); the others are deleted now. */
VOID HidDropChildren(PHIDFDO x)
{
    ULONG i;

    for (i = 0; i < x->NumChildren; i++) {
        PDEVICE_OBJECT pdo = x->Child[i];
        PHIDPDO p;
        if (!pdo)
            continue;
        p = (PHIDPDO)pdo->DeviceExtension;
        x->Child[i] = NULL;
        p->Fdo = NULL;
        p->Present = FALSE;
        if (!p->Reported || p->RemoveSeen)      /* PnP does not know it / has removed it */
            FreeChild(pdo);
    }
    x->NumChildren = 0;
}

/* ------------------------------------------------------------------ */
/* PnP                                                                 */
/* ------------------------------------------------------------------ */

/* compatible ID for the collection's top-level usage (98 HIDDEV.INF style) */
static const char *SystemId(USAGE page, USAGE usage)
{
    if (page == HID_USAGE_PAGE_GENERIC) {
        if (usage == HID_USAGE_GENERIC_KEYBOARD) return "HID_DEVICE_SYSTEM_KEYBOARD";
        if (usage == HID_USAGE_GENERIC_MOUSE)    return "HID_DEVICE_SYSTEM_MOUSE";
        if (usage == HID_USAGE_GENERIC_SYSTEM_CTL) return "HID_DEVICE_SYSTEM_CONTROL";
    }
    if (page == HID_USAGE_PAGE_CONSUMER && usage == 0x01)         /* Consumer Control */
        return "HID_DEVICE_SYSTEM_CONSUMER";
    return NULL;
}

static VOID PutDeviceId(PHIDPDO p, PWCHAR b, PULONG n, BOOLEAN withRev)
{
    PutStr(b, n, "HID\\VID_");
    PutHex(b, n, p->Attr.VendorID, 4);
    PutStr(b, n, "&PID_");
    PutHex(b, n, p->Attr.ProductID, 4);
    if (withRev) {
        PutStr(b, n, "&REV_");
        PutHex(b, n, p->Attr.VersionNumber, 4);
    }
    if (p->NumCollections > 1) {
        PutStr(b, n, "&Col");
        PutHex(b, n, p->Index + 1, 2);
    }
}

static NTSTATUS QueryId(PHIDPDO p, PIRP irp)
{
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(irp);
    WCHAR b[300];
    ULONG n = 0;
    const char *sys;
    PWCHAR out;

    switch (s->Parameters.QueryId.IdType) {
    case BusQueryDeviceID:
        PutDeviceId(p, b, &n, FALSE);
        b[n++] = 0;
        break;
    case BusQueryHardwareIDs:
        PutDeviceId(p, b, &n, TRUE);
        b[n++] = 0;
        PutDeviceId(p, b, &n, FALSE);
        b[n++] = 0;
        b[n++] = 0;
        break;
    case BusQueryCompatibleIDs:
        sys = SystemId(p->UsagePage, p->Usage);
        if (sys) {
            PutStr(b, &n, sys);
            b[n++] = 0;
        }
        PutStr(b, &n, "HID_DEVICE_UP:");
        PutHex(b, &n, p->UsagePage, 4);
        PutStr(b, &n, "_U:");
        PutHex(b, &n, p->Usage, 4);
        b[n++] = 0;
        /*
         * The catch-all "HID_DEVICE" (W95HID.INF: "HID-compliant device", no driver) is
         * left off the keyboard / mouse / consumer nodes.  Windows re-uses an already
         * registered driver that matches ANY compatible ID, so with the catch-all in
         * their list a second keyboard or mouse picked the driverless entry.
         */
        if (!sys || (p->UsagePage == HID_USAGE_PAGE_GENERIC && p->Usage == HID_USAGE_GENERIC_SYSTEM_CTL)) {
            PutStr(b, &n, "HID_DEVICE");
            b[n++] = 0;
        }
        b[n++] = 0;
        break;
    case BusQueryInstanceID:
        PutHex(b, &n, p->Index, 2);
        b[n++] = 0;
        break;
    default:
        return irp->IoStatus.Status;
    }

    out = (PWCHAR)ExAllocatePoolWithTag(PagedPool, n * sizeof(WCHAR), HIDCLS_TAG);
    if (!out)
        return STATUS_INSUFFICIENT_RESOURCES;
    RtlCopyMemory(out, b, n * sizeof(WCHAR));
    irp->IoStatus.Information = (ULONG)out;
    return STATUS_SUCCESS;
}

static NTSTATUS QueryCaps(PHIDPDO p, PIRP irp)
{
    PDEVICE_CAPABILITIES c =
        IoGetCurrentIrpStackLocation(irp)->Parameters.DeviceCapabilities.Capabilities;
    ULONG i;

    UNREFERENCED_PARAMETER(p);
    c->Removable = FALSE;
    c->UniqueID = FALSE;
    c->Address = 0;
    c->UINumber = 0;
    c->DeviceState[PowerSystemWorking] = PowerDeviceD0;
    for (i = PowerSystemSleeping1; i < PowerSystemMaximum; i++)
        c->DeviceState[i] = PowerDeviceD3;
    return STATUS_SUCCESS;
}

NTSTATUS HidPdoPnp(PDEVICE_OBJECT dev, PIRP irp)
{
    PHIDPDO p = (PHIDPDO)dev->DeviceExtension;
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(irp);
    NTSTATUS st = irp->IoStatus.Status;

    switch (s->MinorFunction) {
    case IRP_MN_START_DEVICE:
        if (!p->LinkMade) {
            UNICODE_STRING name, link;
            RtlInitUnicodeString(&name, p->DeviceName);
            RtlInitUnicodeString(&link, p->LinkName);
            st = IoCreateSymbolicLink(&link, &name);
            if (NT_SUCCESS(st))
                p->LinkMade = TRUE;
            HIDDBG(("[HIDCLASS] collection %u START: \\\\.\\HID%u link %x\n",
                    p->Index, p->Serial, st));
        }
        p->Started = TRUE;
        st = STATUS_SUCCESS;
        break;

    case IRP_MN_QUERY_STOP_DEVICE:
    case IRP_MN_CANCEL_STOP_DEVICE:
    case IRP_MN_STOP_DEVICE:
    case IRP_MN_QUERY_REMOVE_DEVICE:
    case IRP_MN_CANCEL_REMOVE_DEVICE:
        st = STATUS_SUCCESS;
        break;

    case IRP_MN_SURPRISE_REMOVAL:           /* the suspend: the FDO keeps this PDO, see FdoPnp */
        st = STATUS_SUCCESS;
        break;

    case IRP_MN_REMOVE_DEVICE:
        HIDDBG(("[HIDCLASS] collection %u REMOVE (present=%u, PDO %x, reference count %d)\n",
                p->Index, (ULONG)p->Present, (ULONG)dev, dev->ReferenceCount));
        p->Started = FALSE;
        p->RemoveSeen = TRUE;
        HidComplete(irp, STATUS_SUCCESS, 0);
        if (!p->Present)
            FreeChild(dev);
        return STATUS_SUCCESS;

    case IRP_MN_QUERY_ID:
        st = QueryId(p, irp);
        break;

    case IRP_MN_QUERY_CAPABILITIES:
        st = QueryCaps(p, irp);
        break;

    case IRP_MN_QUERY_DEVICE_RELATIONS:
        if (s->Parameters.QueryDeviceRelations.Type == TargetDeviceRelation) {
            PDEVICE_RELATIONS rel = (PDEVICE_RELATIONS)ExAllocatePoolWithTag(PagedPool,
                                            sizeof(DEVICE_RELATIONS), HIDCLS_TAG);
            if (!rel) {
                st = STATUS_INSUFFICIENT_RESOURCES;
                break;
            }
            rel->Count = 1;
            rel->Objects[0] = dev;
            ObReferenceObject(dev);
            irp->IoStatus.Information = (ULONG)rel;
            st = STATUS_SUCCESS;
        }
        break;
    }
    return HidComplete(irp, st, irp->IoStatus.Information);
}

NTSTATUS HidPdoPower(PDEVICE_OBJECT dev, PIRP irp)
{
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(irp);
    NTSTATUS st = irp->IoStatus.Status;

    UNREFERENCED_PARAMETER(dev);
    if (s->MinorFunction == IRP_MN_SET_POWER || s->MinorFunction == IRP_MN_QUERY_POWER)
        st = STATUS_SUCCESS;
    else if (s->MinorFunction == IRP_MN_WAIT_WAKE)
        st = STATUS_NOT_SUPPORTED;
    /* no PoStartNextPowerIrp on OSR2.1 */
    return HidComplete(irp, st, 0);
}

/* ------------------------------------------------------------------ */
/* client requests                                                     */
/* ------------------------------------------------------------------ */

/* the client of an open handle (queue lock held) */
static PHIDOPEN FindClient(PHIDPDO p, PFILE_OBJECT fo)
{
    PLIST_ENTRY e;

    for (e = p->Clients.Flink; e != &p->Clients; e = e->Flink) {
        PHIDOPEN c = CONTAINING_RECORD(e, HIDOPEN, Link);
        if (c->Fo == fo)
            return c;
    }
    return NULL;
}

NTSTATUS HidPdoCreate(PDEVICE_OBJECT dev, PIRP irp)
{
    PHIDPDO p = (PHIDPDO)dev->DeviceExtension;
    PHIDFDO x = p->Fdo;

    if (!p->Started || !x)
        return HidComplete(irp, STATUS_DEVICE_NOT_CONNECTED, 0);
    {
        PHIDOPEN c = (PHIDOPEN)ExAllocatePoolWithTag(NonPagedPool, sizeof(HIDOPEN), HIDCLS_TAG);
        KIRQL irql;

        if (!c)
            return HidComplete(irp, STATUS_INSUFFICIENT_RESOURCES, 0);
        RtlZeroMemory(c, sizeof(*c));
        c->Fo = IoGetCurrentIrpStackLocation(irp)->FileObject;
        KeAcquireSpinLock(&p->QLock, &irql);
        InsertTailList(&p->Clients, &c->Link);
        KeReleaseSpinLock(&p->QLock, irql);
    }
    InterlockedIncrement(&p->OpenCount);
    if (InterlockedIncrement(&x->OpenTotal) == 1) {
        HidCallMini(x, IOCTL_HID_ACTIVATE_DEVICE, NULL, 0, NULL, 0);
        x->WantReads = TRUE;
        HidPumpReads(x);
    }
    HIDDBG(("[HIDCLASS] \\\\.\\HID%u opened (%d)\n", p->Serial, p->OpenCount));
    return HidComplete(irp, STATUS_SUCCESS, 0);
}

NTSTATUS HidPdoClose(PDEVICE_OBJECT dev, PIRP irp)
{
    PHIDPDO p = (PHIDPDO)dev->DeviceExtension;
    PHIDFDO x = p->Fdo;
    PFILE_OBJECT fo = IoGetCurrentIrpStackLocation(irp)->FileObject;
    PHIDOPEN c;
    KIRQL irql;

    KeAcquireSpinLock(&p->QLock, &irql);
    c = FindClient(p, fo);
    if (c)
        RemoveEntryList(&c->Link);
    KeReleaseSpinLock(&p->QLock, irql);
    if (c)
        ExFreePool(c);
    InterlockedDecrement(&p->OpenCount);
    if (x && InterlockedDecrement(&x->OpenTotal) == 0)
        HidDeactivate(x);
    HIDDBG(("[HIDCLASS] \\\\.\\HID%u closed (%d)\n", p->Serial, p->OpenCount));
    return HidComplete(irp, STATUS_SUCCESS, 0);
}

/* ---- reports waiting for / waiting on the clients ---- */

static VOID NTAPI ReadCancel(PDEVICE_OBJECT dev, PIRP irp)
{
    PHIDPDO p = (PHIDPDO)dev->DeviceExtension;
    KIRQL irql;

    IoReleaseCancelSpinLock(irp->CancelIrql);
    KeAcquireSpinLock(&p->QLock, &irql);
    RemoveEntryList(&irp->Tail.Overlay.ListEntry);
    KeReleaseSpinLock(&p->QLock, irql);
    HidComplete(irp, STATUS_CANCELLED, 0);
}

static PIRP TakeReaderOf(PHIDPDO p, PFILE_OBJECT fo);

/* every open handle gets its own copy of the report: to its waiting read, or
 * into its own ring (a second reader must not steal the key-up from the keyboard VxD) */
#define HID_MAX_CLIENTS 8

static VOID PdoQueueReport(PHIDPDO p, PUCHAR data, ULONG len)
{
    KIRQL irql;
    PIRP irps[HID_MAX_CLIENTS];
    ULONG n = 0, i;
    PLIST_ENTRY e;

    if (len > HID_MAX_REPORT)
        len = HID_MAX_REPORT;
    KeAcquireSpinLock(&p->QLock, &irql);
    for (e = p->Clients.Flink; e != &p->Clients; e = e->Flink) {
        PHIDOPEN c = CONTAINING_RECORD(e, HIDOPEN, Link);
        PIRP irp = (n < HID_MAX_CLIENTS) ? TakeReaderOf(p, c->Fo) : NULL;

        if (irp) {
            irps[n++] = irp;
        } else {
            ULONG slot;
            if (c->RingCount >= (c->RingLimit ? c->RingLimit : HID_RING_SLOTS)) {   /* full: drop the oldest */
                c->RingHead = (c->RingHead + 1) % HID_RING_SLOTS;
                c->RingCount--;
            }
            slot = (c->RingHead + c->RingCount) % HID_RING_SLOTS;
            RtlCopyMemory(c->Ring[slot], data, len);
            c->RingLen[slot] = (UCHAR)len;
            c->RingCount++;
        }
    }
    KeReleaseSpinLock(&p->QLock, irql);

    for (i = 0; i < n; i++) {
        ULONG m = IoGetCurrentIrpStackLocation(irps[i])->Parameters.Read.Length;
        if (m > len)
            m = len;
        RtlCopyMemory(irps[i]->AssociatedIrp.SystemBuffer, data, m);
        HidComplete(irps[i], STATUS_SUCCESS, m);
    }
}

/* a report from the minidriver: to the collection that owns its report id */
VOID HidDeliver(PHIDFDO x, PUCHAR data, ULONG len)
{
    ULONG col = 0;
    PDEVICE_OBJECT pdo;
    UCHAR tmp[HID_MAX_REPORT];

    /* clients see the NT convention: byte 0 is the report id, 0 for a device
     * without ids (the minidriver's report then has no id byte at all) */
    if (!x->UsesIds) {
        if (len > HID_MAX_REPORT - 1)
            len = HID_MAX_REPORT - 1;
        tmp[0] = 0;
        RtlCopyMemory(tmp + 1, data, len);
        data = tmp;
        len++;
    }
    if (x->UsesIds && x->NumChildren > 1) {
        col = x->IdToCol[data[0]];
        if (col == 0xFF)
            return;
    }
    if (col >= x->NumChildren)
        return;
    pdo = x->Child[col];
    if (pdo)
        PdoQueueReport((PHIDPDO)pdo->DeviceExtension, data, len);
}

/* a waiting reader of the given file object (any, if fo is NULL), off the list
 * (queue lock held) */
static PIRP TakeReaderOf(PHIDPDO p, PFILE_OBJECT fo)
{
    PLIST_ENTRY e;
    PIRP irp;

restart:
    for (e = p->Reads.Flink; e != &p->Reads; e = e->Flink) {
        irp = CONTAINING_RECORD(e, IRP, Tail.Overlay.ListEntry);
        if (fo && IoGetCurrentIrpStackLocation(irp)->FileObject != fo)
            continue;
        RemoveEntryList(e);
        if (IoSetCancelRoutine(irp, NULL))
            return irp;
        InitializeListHead(e);                  /* its cancel routine is running */
        goto restart;
    }
    return NULL;
}

/*
 * A client closed its handle: only ITS waiting reads end.  The collection is
 * shared (the keyboard / mouse VxD holds one open and a test program may open the
 * same one); cancelling every waiting read made the VxD think its device was gone
 * and close its handle, and the mouse died.
 */
NTSTATUS HidPdoCleanup(PDEVICE_OBJECT dev, PIRP irp)
{
    PHIDPDO p = (PHIDPDO)dev->DeviceExtension;
    PFILE_OBJECT fo = IoGetCurrentIrpStackLocation(irp)->FileObject;
    KIRQL irql;
    PIRP r;

    HIDDBG(("[HIDCLASS] \\\\.\\HID%u cleanup, file object %x\n", p->Serial, (ULONG)fo));
    for (;;) {
        KeAcquireSpinLock(&p->QLock, &irql);
        r = TakeReaderOf(p, fo);
        KeReleaseSpinLock(&p->QLock, irql);
        if (!r)
            break;
        HidComplete(r, STATUS_CANCELLED, 0);
    }
    return HidComplete(irp, STATUS_SUCCESS, 0);
}

NTSTATUS HidPdoRead(PDEVICE_OBJECT dev, PIRP irp)
{
    PHIDPDO p = (PHIDPDO)dev->DeviceExtension;
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(irp);
    ULONG want = s->Parameters.Read.Length;
    KIRQL irql;
    PHIDOPEN c;

    if (!p->Started || !p->Fdo)
        return HidComplete(irp, STATUS_DEVICE_NOT_CONNECTED, 0);
    if (want < p->InputLength)
        return HidComplete(irp, STATUS_INVALID_BUFFER_SIZE, 0);

    KeAcquireSpinLock(&p->QLock, &irql);
    c = FindClient(p, s->FileObject);
    if (c && c->RingCount) {
        ULONG n = c->RingLen[c->RingHead];
        if (n > want)
            n = want;
        RtlCopyMemory(irp->AssociatedIrp.SystemBuffer, c->Ring[c->RingHead], n);
        c->RingHead = (c->RingHead + 1) % HID_RING_SLOTS;
        c->RingCount--;
        KeReleaseSpinLock(&p->QLock, irql);
        return HidComplete(irp, STATUS_SUCCESS, n);
    }
    IoMarkIrpPending(irp);
    InsertTailList(&p->Reads, &irp->Tail.Overlay.ListEntry);
    IoSetCancelRoutine(irp, ReadCancel);
    if (irp->Cancel && IoSetCancelRoutine(irp, NULL)) {
        RemoveEntryList(&irp->Tail.Overlay.ListEntry);
        KeReleaseSpinLock(&p->QLock, irql);
        HidComplete(irp, STATUS_CANCELLED, 0);
        return STATUS_PENDING;
    }
    KeReleaseSpinLock(&p->QLock, irql);
    return STATUS_PENDING;
}

NTSTATUS HidPdoWrite(PDEVICE_OBJECT dev, PIRP irp)
{
    PHIDPDO p = (PHIDPDO)dev->DeviceExtension;
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(irp);
    ULONG len = s->Parameters.Write.Length;
    PUCHAR buf = (PUCHAR)irp->AssociatedIrp.SystemBuffer;
    HID_XFER_PACKET pkt;
    NTSTATUS st;

    /* NT convention (the HIDP_CAPS lengths include the report id byte): the
     * buffer starts with the report id, 0 when the device has none.  The
     * minidriver gets the id in the packet; with id 0 the buffer starts after it. */
    if (!p->Started || !p->Fdo)
        return HidComplete(irp, STATUS_DEVICE_NOT_CONNECTED, 0);
    if (!p->OutputLength)
        return HidComplete(irp, STATUS_NOT_SUPPORTED, 0);
    if (!buf || len < 1 || len < p->OutputLength)
        return HidComplete(irp, STATUS_INVALID_BUFFER_SIZE, 0);
    pkt.reportId = buf[0];
    if (pkt.reportId) {
        pkt.reportBuffer = buf;
        pkt.reportBufferLen = len;
    } else {
        pkt.reportBuffer = buf + 1;
        pkt.reportBufferLen = len - 1;
    }
    st = HidCallMini(p->Fdo, IOCTL_HID_WRITE_REPORT, &pkt, sizeof(pkt), NULL, 0);
    return HidComplete(irp, st, NT_SUCCESS(st) ? len : 0);
}

NTSTATUS HidPdoIoctl(PDEVICE_OBJECT dev, PIRP irp)
{
    PHIDPDO p = (PHIDPDO)dev->DeviceExtension;
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(irp);
    ULONG outLen = s->Parameters.DeviceIoControl.OutputBufferLength;

    switch (s->Parameters.DeviceIoControl.IoControlCode) {

    case IOCTL_HID_GET_COLLECTION_INFORMATION: {           /* buffered */
        PHID_COLLECTION_INFORMATION ci = (PHID_COLLECTION_INFORMATION)irp->AssociatedIrp.SystemBuffer;
        if (outLen < sizeof(HID_COLLECTION_INFORMATION))
            return HidComplete(irp, STATUS_BUFFER_TOO_SMALL, 0);
        RtlZeroMemory(ci, sizeof(*ci));
        ci->DescriptorSize = p->PreparsedLength;
        ci->Polled = p->Fdo ? p->Fdo->Mini->Polled : FALSE;
        ci->VendorID = p->Attr.VendorID;
        ci->ProductID = p->Attr.ProductID;
        ci->VersionNumber = p->Attr.VersionNumber;
        return HidComplete(irp, STATUS_SUCCESS, sizeof(*ci));
    }

    case IOCTL_HID_GET_COLLECTION_DESCRIPTOR:              /* METHOD_NEITHER */
        if (outLen < p->PreparsedLength)
            return HidComplete(irp, STATUS_INVALID_BUFFER_SIZE, 0);
        RtlCopyMemory(irp->UserBuffer, p->Preparsed, p->PreparsedLength);
        return HidComplete(irp, STATUS_SUCCESS, p->PreparsedLength);

    case IOCTL_HIDCLS_LOG: {                               /* buffered, ASCII line */
        PCHAR text = (PCHAR)irp->AssociatedIrp.SystemBuffer;
        ULONG inLen = s->Parameters.DeviceIoControl.InputBufferLength, i;
        char line[200];
        for (i = 0; i < inLen && i < sizeof(line) - 1 && text[i]; i++)
            line[i] = text[i];
        line[i] = 0;
        HIDDBG(("[HIDCLIENT] %s\n", line));
        return HidComplete(irp, STATUS_SUCCESS, 0);
    }

    case IOCTL_HIDCLS_GET_FEATURE:
    case IOCTL_HIDCLS_SET_FEATURE:
    case IOCTL_HIDCLS_GET_INPUT: {          /* buffered: one buffer for in and out */
        ULONG code = s->Parameters.DeviceIoControl.IoControlCode;
        BOOLEAN get = (BOOLEAN)(code != IOCTL_HIDCLS_SET_FEATURE);
        PUCHAR buf = (PUCHAR)irp->AssociatedIrp.SystemBuffer;
        ULONG inLen = s->Parameters.DeviceIoControl.InputBufferLength;
        ULONG need = (code == IOCTL_HIDCLS_GET_INPUT) ? p->InputLength : p->FeatureLength;
        HID_XFER_PACKET pkt;
        NTSTATUS st;

        if (!p->Started || !p->Fdo || !p->Fdo->Started)
            return HidComplete(irp, STATUS_DEVICE_NOT_CONNECTED, 0);
        if (!need)
            return HidComplete(irp, STATUS_NOT_SUPPORTED, 0);
        if (!buf || inLen < 1 || (get ? outLen : inLen) < need)
            return HidComplete(irp, STATUS_INVALID_BUFFER_SIZE, 0);
        /* the packet as for WriteFile: with a report id the buffer starts with it */
        pkt.reportId = buf[0];
        if (pkt.reportId) {
            pkt.reportBuffer = buf;
            pkt.reportBufferLen = need;
        } else {
            pkt.reportBuffer = buf + 1;
            pkt.reportBufferLen = need - 1;
        }
        st = HidCallMini(p->Fdo,
                         code == IOCTL_HIDCLS_SET_FEATURE ? IOCTL_HIDUSB_SET_FEATURE :
                         code == IOCTL_HIDCLS_GET_INPUT   ? IOCTL_HIDUSB_GET_INPUT :
                                                            IOCTL_HIDUSB_GET_FEATURE,
                         &pkt, sizeof(pkt), NULL, 0);
        return HidComplete(irp, st, (NT_SUCCESS(st) && get) ? need : 0);
    }

    case IOCTL_HIDCLS_GET_STRING: {         /* in: a selector, out: the wide string */
        PUCHAR buf = (PUCHAR)irp->AssociatedIrp.SystemBuffer;
        ULONG inLen = s->Parameters.DeviceIoControl.InputBufferLength;
        ULONG sel, id, n;
        NTSTATUS st;

        if (!p->Started || !p->Fdo || !p->Fdo->Started)
            return HidComplete(irp, STATUS_DEVICE_NOT_CONNECTED, 0);
        if (!buf || inLen < sizeof(ULONG) || outLen < sizeof(WCHAR))
            return HidComplete(irp, STATUS_INVALID_BUFFER_SIZE, 0);
        sel = *(PULONG)buf;
        if (sel == HIDSTR_MANUFACTURER)      id = HID_STRING_ID_IMANUFACTURER;
        else if (sel == HIDSTR_PRODUCT)      id = HID_STRING_ID_IPRODUCT;
        else if (sel == HIDSTR_SERIALNUMBER) id = HID_STRING_ID_ISERIALNUMBER;
        else if (sel & 0x100UL)              /* the minidriver's form: 0x80000000 | lang << 8 | index */
            id = 0x80000000UL | (((sel >> 16) & 0xFFFFUL) << 8) | (sel & 0xFFUL);
        else
            return HidComplete(irp, STATUS_INVALID_PARAMETER, 0);
        st = HidCallMini(p->Fdo, IOCTL_HID_GET_STRING, buf, outLen, (PVOID)id, 0);
        if (!NT_SUCCESS(st))
            return HidComplete(irp, st, 0);
        for (n = 0; n + 1 < outLen / sizeof(WCHAR) && ((PWCHAR)buf)[n]; n++)
            ;
        return HidComplete(irp, STATUS_SUCCESS, (n + 1) * sizeof(WCHAR));
    }

    case IOCTL_HIDCLS_GET_PREPARSED:
        if (!p->Preparsed || outLen < p->PreparsedLength)
            return HidComplete(irp, STATUS_INVALID_BUFFER_SIZE, 0);
        RtlCopyMemory(irp->AssociatedIrp.SystemBuffer, p->Preparsed, p->PreparsedLength);
        return HidComplete(irp, STATUS_SUCCESS, p->PreparsedLength);

    case IOCTL_HIDCLS_FLUSH_QUEUE: {
        KIRQL irql;

        PHIDOPEN c;

        KeAcquireSpinLock(&p->QLock, &irql);
        c = FindClient(p, s->FileObject);
        if (c) {
            c->RingHead = 0;
            c->RingCount = 0;
        }
        KeReleaseSpinLock(&p->QLock, irql);
        return HidComplete(irp, STATUS_SUCCESS, 0);
    }

    case IOCTL_HIDCLS_GET_NUM_BUFFERS: {
        PHIDOPEN c;
        ULONG v = HID_RING_SLOTS;
        KIRQL irql;

        if (outLen < sizeof(ULONG))
            return HidComplete(irp, STATUS_BUFFER_TOO_SMALL, 0);
        KeAcquireSpinLock(&p->QLock, &irql);
        c = FindClient(p, s->FileObject);
        if (c && c->RingLimit)
            v = c->RingLimit;
        KeReleaseSpinLock(&p->QLock, irql);
        *(PULONG)irp->AssociatedIrp.SystemBuffer = v;
        return HidComplete(irp, STATUS_SUCCESS, sizeof(ULONG));
    }

    case IOCTL_HIDCLS_SET_NUM_BUFFERS: {
        PHIDOPEN c;
        KIRQL irql;
        ULONG v;

        if (s->Parameters.DeviceIoControl.InputBufferLength < sizeof(ULONG))
            return HidComplete(irp, STATUS_BUFFER_TOO_SMALL, 0);
        v = *(PULONG)irp->AssociatedIrp.SystemBuffer;
        if (v < 1 || v > HID_RING_SLOTS)
            return HidComplete(irp, STATUS_INVALID_PARAMETER, 0);
        KeAcquireSpinLock(&p->QLock, &irql);
        c = FindClient(p, s->FileObject);
        if (c)
            c->RingLimit = v;
        KeReleaseSpinLock(&p->QLock, irql);
        return HidComplete(irp, STATUS_SUCCESS, 0);
    }

    default:
        return HidComplete(irp, STATUS_NOT_SUPPORTED, 0);
    }
}

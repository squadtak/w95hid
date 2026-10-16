/*
 * hidclass.c - HIDCLASS.SYS for Windows 95 OSR2.1: minidriver registration,
 * the FDO (one per HID device) and the dispatch routines.  See hidcls.h.
 */
#include "hidcls.h"

static MINIREC *g_Minis = NULL;
static LONG g_Shutdown;             /* set by IRP_MJ_SHUTDOWN: no bus I/O any more */

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

NTSTATUS HidComplete(PIRP Irp, NTSTATUS Status, ULONG Information)
{
    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = Information;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}

NTSTATUS HidPassDown(PHIDFDO x, PIRP Irp)
{
    IoSkipCurrentIrpStackLocation(Irp);
    return IoCallDriver(x->Hdr.NextDeviceObject, Irp);
}

static MINIREC *FindMini(PDRIVER_OBJECT drv)
{
    MINIREC *m;
    for (m = g_Minis; m; m = m->Next)
        if (m->Driver == drv)
            return m;
    return NULL;
}

static ULONG KindOf(PDEVICE_OBJECT dev)
{
    return ((PHIDFDO)dev->DeviceExtension)->Kind;
}

/*
 * Send a minidriver IOCTL (HIDPORT.H, METHOD_NEITHER) to the FDO and wait.
 * The minidriver reads/writes Irp->UserBuffer and the stack's
 * Output/InputBufferLength.
 */
NTSTATUS HidCallMini(PHIDFDO x, ULONG Code, PVOID OutBuf, ULONG OutLen,
                     PVOID InBuf, ULONG InLen)
{
    return HidCallMiniEx(x, Code, OutBuf, OutLen, InBuf, InLen, NULL);
}

/* ... and the number of bytes the minidriver put into the output buffer */
NTSTATUS HidCallMiniEx(PHIDFDO x, ULONG Code, PVOID OutBuf, ULONG OutLen,
                       PVOID InBuf, ULONG InLen, PULONG Info)
{
    KEVENT event;
    IO_STATUS_BLOCK iosb;
    PIRP irp;
    NTSTATUS st;

    KeInitializeEvent(&event, NotificationEvent, FALSE);
    irp = IoBuildDeviceIoControlRequest(Code, x->Self, InBuf, InLen, OutBuf,
                                        OutLen, TRUE, &event, &iosb);
    if (!irp)
        return STATUS_INSUFFICIENT_RESOURCES;
    TRC("H.mini call", Code);
    st = IoCallDriver(x->Self, irp);
    if (st == STATUS_PENDING) {
        TRC("H.mini wait", Code);
        KeWaitForSingleObject(&event, Suspended, KernelMode, FALSE, NULL);
        st = iosb.Status;
    }
    TRC("H.mini done", st);
    if (Info)
        *Info = iosb.Information;
    return st;
}


/* ------------------------------------------------------------------ */
/* the continuous report read (IOCTL_HID_READ_REPORT to the minidriver) */
/* ------------------------------------------------------------------ */

static NTSTATUS NTAPI ReadDone(PDEVICE_OBJECT dev, PIRP irp, PVOID ctx)
{
    PHIDFDO x = (PHIDFDO)ctx;
    NTSTATUS st = irp->IoStatus.Status;

    UNREFERENCED_PARAMETER(dev);
    x->Reading = FALSE;
    if (NT_SUCCESS(st)) {
        if (irp->IoStatus.Information)
            HidDeliver(x, x->ReadBuf, irp->IoStatus.Information);
        HidPumpReads(x);                        /* next report */
    }
    /* on an error (device stopped or gone) the loop ends until the next START */
    return STATUS_MORE_PROCESSING_REQUIRED;     /* the IRP is reused */
}

static VOID SubmitRead(PHIDFDO x)
{
    PIRP irp = x->ReadIrp;
    PIO_STACK_LOCATION next;
    CCHAR stack = x->Self->StackSize;

    IoInitializeIrp(irp, (USHORT)IoSizeOfIrp(stack), stack);
    irp->UserBuffer = x->ReadBuf;
    next = IoGetNextIrpStackLocation(irp);
    next->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
    next->Parameters.DeviceIoControl.IoControlCode = IOCTL_HID_READ_REPORT;
    next->Parameters.DeviceIoControl.OutputBufferLength = x->ReadBufLen;
    next->Parameters.DeviceIoControl.InputBufferLength = 0;
    IoSetCompletionRoutine(irp, ReadDone, x, TRUE, TRUE, TRUE);
    x->Reading = TRUE;
    IoCallDriver(x->Self, irp);                 /* may complete right here */
}

/*
 * Keep one read down in the minidriver while a client has a collection open.
 * The minidriver may complete a read inside IoCallDriver; ReadDone then calls
 * this again, so a flag makes the inner call just ask the outer loop to go on
 * (no recursion).
 */
VOID HidPumpReads(PHIDFDO x)
{
    for (;;) {
        if (InterlockedExchange(&x->Pumping, 1)) {
            InterlockedExchange(&x->Again, 1);
            return;
        }
        do {
            InterlockedExchange(&x->Again, 0);
            if (x->WantReads && !x->Removing && !x->Reading && x->ReadIrp && x->Started)
                SubmitRead(x);
        } while (x->Again);
        InterlockedExchange(&x->Pumping, 0);
        if (!x->Again)
            return;
    }
}

VOID HidDeactivate(PHIDFDO x)
{
    x->WantReads = FALSE;
    HidCallMini(x, IOCTL_HID_DEACTIVATE_DEVICE, NULL, 0, NULL, 0);
}

/* ------------------------------------------------------------------ */
/* FDO PnP                                                             */
/* ------------------------------------------------------------------ */

static NTSTATUS NTAPI SyncComplete(PDEVICE_OBJECT dev, PIRP irp, PVOID ctx)
{
    UNREFERENCED_PARAMETER(dev);
    UNREFERENCED_PARAMETER(irp);
    KeSetEvent((PKEVENT)ctx, 0, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

/*
 * Run the minidriver's PnP handler as a layer below this one and wait for
 * the whole stack beneath us to finish with the IRP.  The IRP stack pointer
 * is pushed one location, so the minidriver sees "its" location and sets up
 * its own forwarding to NextDeviceObject; our completion routine is stored
 * in the location we pushed past.
 */
static NTSTATUS CallMiniPnp(PHIDFDO x, PIRP irp)
{
    KEVENT event;

    KeInitializeEvent(&event, NotificationEvent, FALSE);
    IoCopyCurrentIrpStackLocationToNext(irp);
    IoSetCompletionRoutine(irp, SyncComplete, &event, TRUE, TRUE, TRUE);
    IoSetNextIrpStackLocation(irp);
    x->Mini->MiniPnp(x->Self, irp);
    KeWaitForSingleObject(&event, Suspended, KernelMode, FALSE, NULL);
    return irp->IoStatus.Status;
}

/* BusRelations: we are the bus driver of the collection PDOs, so we answer
 * it ourselves (a list from drivers above us, if any, is kept) and never block
 * waiting for the PDO below. */
static NTSTATUS FdoBusRelations(PHIDFDO x, PIRP irp)
{
    PDEVICE_RELATIONS old, rel;
    ULONG oldCount, i, n, size;

    old = (PDEVICE_RELATIONS)irp->IoStatus.Information;
    oldCount = old ? old->Count : 0;

    n = 0;
    for (i = 0; i < x->NumChildren; i++)
        if (x->Child[i])
            n++;
    size = sizeof(DEVICE_RELATIONS) + (oldCount + n) * sizeof(PDEVICE_OBJECT);
    rel = (PDEVICE_RELATIONS)ExAllocatePoolWithTag(PagedPool, size, HIDCLS_TAG);
    if (!rel)
        return HidComplete(irp, STATUS_INSUFFICIENT_RESOURCES, 0);

    rel->Count = 0;
    for (i = 0; i < oldCount; i++)
        rel->Objects[rel->Count++] = old->Objects[i];
    for (i = 0; i < x->NumChildren; i++) {
        if (x->Child[i]) {
            ((PHIDPDO)x->Child[i]->DeviceExtension)->Reported = TRUE;
            ObReferenceObject(x->Child[i]);
            rel->Objects[rel->Count++] = x->Child[i];
        }
    }
    if (old)
        ExFreePool(old);
    HIDDBG(("[HIDCLASS] BusRelations: %u collection PDO(s) reported\n", n));
    return HidComplete(irp, STATUS_SUCCESS, (ULONG)rel);
}

static NTSTATUS FdoStart(PHIDFDO x, PIRP irp)
{
    NTSTATUS st = CallMiniPnp(x, irp);          /* minidriver + the bus below */

    if (NT_SUCCESS(st)) {
        st = HidCreateChildren(x);
        if (NT_SUCCESS(st)) {
            x->Started = TRUE;
            if (x->OpenTotal) {
                x->WantReads = TRUE;
                HidPumpReads(x);
            }
            IoInvalidateDeviceRelations(x->Hdr.PhysicalDeviceObject, BusRelations);
        } else {
            HIDDBG(("[HIDCLASS] collections failed: %x\n", st));
        }
    } else {
        HIDDBG(("[HIDCLASS] minidriver START failed: %x\n", st));
    }
    return HidComplete(irp, st, 0);
}

/*
 * REMOVE_DEVICE.  The usual WDM pattern: do not wait for the lower driver
 * (on Win9x it may complete the IRP only after this dispatch returns, and a
 * wait here freezes the system); send it down through the minidriver, then
 * detach and delete.  Nothing of ours is touched when the IRP completes.
 */
static NTSTATUS FdoRemove(PHIDFDO x, PIRP irp)
{
    PDEVICE_OBJECT fdo = x->Self;
    PDEVICE_OBJECT next = x->Hdr.NextDeviceObject;
    NTSTATUS st;

    HIDDBG(("[HIDCLASS] remove FDO %x\n", (ULONG)fdo));
    TRC("H.rm0", fdo);
    TRCB(1);
    if (g_Shutdown) {
        /*
         * System shutdown: the USB stack no longer completes anything (a URB, or
         * the hub driver's own REMOVE handling, never returns).  Touch nothing,
         * the machine is going down; the bus driver below just does not hear
         * about this device.
         */
        TRC("H.rm shutdown: nothing done", 0);
        return HidComplete(irp, STATUS_SUCCESS, 0);
    }
    x->Removing = TRUE;
    x->Started = FALSE;
    x->WantReads = FALSE;
    HidDropChildren(x);
    TRC("H.rm1 children dropped", 0);
    TRCB(2);

    /*
     * Detach BEFORE the IRP goes down: the bus driver deletes the PDO while
     * handling this very REMOVE, and Win9x NTKERN bugchecks (0x36
     * DEVICE_REFERENCE_COUNT_NOT_ZERO) if our attachment still references it.
     * x->Hdr.NextDeviceObject stays valid for the minidriver to pass the IRP.
     */
    IoDetachDevice(next);
    TRC("H.rm2 detached", 0);

    IoCopyCurrentIrpStackLocationToNext(irp);
    IoSetNextIrpStackLocation(irp);
    st = x->Mini->MiniPnp(fdo, irp);        /* stops the device, passes the IRP down */
    TRC("H.rm3 minidriver returned", st);
    TRCB(7);

    /* the minidriver has failed our pending read: nothing is down any more */
    if (x->ReadIrp && !x->Reading) {
        IoFreeIrp(x->ReadIrp);
        x->ReadIrp = NULL;
    }
    if (x->ReadBuf && !x->Reading) {
        ExFreePool(x->ReadBuf);
        x->ReadBuf = NULL;
    }
    HIDDBG(("[HIDCLASS] delete FDO %x (reference count %d)\n", (ULONG)fdo, fdo->ReferenceCount));
    TRC("H.delete FDO", fdo);
    TRC("H.delete FDO ref", fdo->ReferenceCount);
    if (fdo->ReferenceCount != 0)       /* see FreeChild: no 0x36 */
        HIDDBG(("[HIDCLASS] FDO %x is still referenced: kept\n", (ULONG)fdo));
    else
        IoDeleteDevice(fdo);
    TRC("H.rm4 deleted", 0);
    TRCB(8);
    return st;
}

/*
 * Back from a suspend: ask the minidriver to configure the device again and let
 * the clients' pending reads see reports again.  Fails quietly when the bus is
 * not ready yet; the next BusRelations query tries once more.
 */
static void TryRestart(PHIDFDO x)
{
    NTSTATUS st;
    ULONG i;

    st = HidCallMini(x, IOCTL_HIDUSB_RESTART, NULL, 0, NULL, 0);
    HIDDBG(("[HIDCLASS] restart after suspend: %x\n", st));
    TRC("H.restart", st);
    if (!NT_SUCCESS(st))
        return;
    x->Suspended = FALSE;
    x->Started = TRUE;
    for (i = 0; i < x->NumChildren; i++)
        if (x->Child[i])
            ((PHIDPDO)x->Child[i]->DeviceExtension)->Started = TRUE;
    if (x->OpenTotal) {
        x->WantReads = TRUE;
        HidPumpReads(x);
    }
}

static NTSTATUS FdoPnp(PHIDFDO x, PIRP irp)
{
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(irp);

    switch (s->MinorFunction) {
    case IRP_MN_START_DEVICE:
        return FdoStart(x, irp);

    case IRP_MN_REMOVE_DEVICE:
        return FdoRemove(x, irp);

    case IRP_MN_SURPRISE_REMOVAL:
        /*
         * Sent when the system suspends and again when the hub re-enumerates the
         * device on resume.  The unplug of a device is a plain REMOVE_DEVICE, so
         * this is the suspend.  NTKERN does not follow it with a REMOVE and does
         * not call AddDevice / START again afterwards (tried: tearing the FDO down
         * leaves the mouse dead until it is replugged, and the child PDO is then
         * still referenced by PnP).  So the stack stays: the minidriver stops the
         * device (its transfers are abandoned), the clients keep their reads
         * pending, and the restart comes with the next BusRelations query, see
         * TryRestart.
         */
        if (g_Shutdown)
            return FdoRemove(x, irp);
        TRC("H.suspend", 0);
        x->Suspended = TRUE;
        x->Started = FALSE;
        break;

    case IRP_MN_QUERY_DEVICE_RELATIONS:
        if (s->Parameters.QueryDeviceRelations.Type == BusRelations) {
            if (x->Suspended)
                TryRestart(x);              /* the bus has re-enumerated the device */
            return FdoBusRelations(x, irp);
        }
        break;

    case IRP_MN_STOP_DEVICE:
        x->Started = FALSE;
        break;
    }

    /* everything else goes through the minidriver (which passes it down) */
    IoCopyCurrentIrpStackLocationToNext(irp);
    IoSetNextIrpStackLocation(irp);
    return x->Mini->MiniPnp(x->Self, irp);
}

/* ------------------------------------------------------------------ */
/* dispatch (the driver object's slots, shared by FDO and PDOs)        */
/* ------------------------------------------------------------------ */

/* called through IOCTL_HIDEXT_SHUTDOWN by HIDEXT.VXD (KERNEL32_SHUTDOWN) */
VOID HidSetShutdown(VOID)
{
    InterlockedExchange(&g_Shutdown, 1);
    TRC("H.shutdown (hidext)", 0);
}

static NTSTATUS NTAPI HidDispatchPnp(PDEVICE_OBJECT dev, PIRP irp)
{
    TRC("H.pnp", ((KindOf(dev) == KIND_PDO) ? 0x100UL : 0UL) |
                 IoGetCurrentIrpStackLocation(irp)->MinorFunction);
    HIDDBG(("[HIDCLASS] %s PnP minor %x\n", KindOf(dev) == KIND_PDO ? "PDO" : "FDO",
            (ULONG)IoGetCurrentIrpStackLocation(irp)->MinorFunction));
    if (KindOf(dev) == KIND_PDO)
        return HidPdoPnp(dev, irp);
    return FdoPnp((PHIDFDO)dev->DeviceExtension, irp);
}

static NTSTATUS NTAPI HidDispatchPower(PDEVICE_OBJECT dev, PIRP irp)
{
    PIO_STACK_LOCATION ps = IoGetCurrentIrpStackLocation(irp);

    TRC("H.pwr", ((KindOf(dev) == KIND_PDO) ? 0x10000UL : 0UL) |
                 ((ULONG)ps->MinorFunction << 8) | (ULONG)ps->Parameters.Power.State.SystemState);
    HIDDBG(("[HIDCLASS] %s power minor %x type %u state %u\n", KindOf(dev) == KIND_PDO ? "PDO" : "FDO",
            (ULONG)ps->MinorFunction, (ULONG)ps->Parameters.Power.Type,
            (ULONG)ps->Parameters.Power.State.SystemState));
    if (KindOf(dev) == KIND_PDO)
        return HidPdoPower(dev, irp);
    /* OSR2.1 has no PoCallDriver / PoStartNextPowerIrp: pass it down. */
    return HidPassDown((PHIDFDO)dev->DeviceExtension, irp);
}

static NTSTATUS NTAPI HidDispatchCreate(PDEVICE_OBJECT dev, PIRP irp)
{
    if (KindOf(dev) == KIND_CTL)
        return HidComplete(irp, STATUS_SUCCESS, 0);
    if (KindOf(dev) == KIND_PDO)
        return HidPdoCreate(dev, irp);
    return HidComplete(irp, STATUS_NOT_SUPPORTED, 0);
}

static NTSTATUS NTAPI HidDispatchClose(PDEVICE_OBJECT dev, PIRP irp)
{
    if (KindOf(dev) == KIND_PDO)
        return HidPdoClose(dev, irp);
    return HidComplete(irp, STATUS_SUCCESS, 0);
}

static NTSTATUS NTAPI HidDispatchCleanup(PDEVICE_OBJECT dev, PIRP irp)
{
    if (KindOf(dev) == KIND_PDO)
        return HidPdoCleanup(dev, irp);
    return HidComplete(irp, STATUS_SUCCESS, 0);
}

static NTSTATUS NTAPI HidDispatchRead(PDEVICE_OBJECT dev, PIRP irp)
{
    if (KindOf(dev) == KIND_PDO)
        return HidPdoRead(dev, irp);
    return HidComplete(irp, STATUS_NOT_SUPPORTED, 0);
}

static NTSTATUS NTAPI HidDispatchWrite(PDEVICE_OBJECT dev, PIRP irp)
{
    if (KindOf(dev) == KIND_PDO)
        return HidPdoWrite(dev, irp);
    return HidComplete(irp, STATUS_NOT_SUPPORTED, 0);
}

static NTSTATUS NTAPI HidDispatchIoctl(PDEVICE_OBJECT dev, PIRP irp)
{
    if (KindOf(dev) == KIND_CTL) {
        if (IoGetCurrentIrpStackLocation(irp)->Parameters.DeviceIoControl.IoControlCode
                == IOCTL_HIDEXT_SHUTDOWN) {
            HidSetShutdown();
            return HidComplete(irp, STATUS_SUCCESS, 0);
        }
        return HidComplete(irp, STATUS_NOT_SUPPORTED, 0);
    }
    if (KindOf(dev) == KIND_PDO)
        return HidPdoIoctl(dev, irp);
    return HidComplete(irp, STATUS_NOT_SUPPORTED, 0);
}

static NTSTATUS NTAPI HidDispatchOther(PDEVICE_OBJECT dev, PIRP irp)
{
    UNREFERENCED_PARAMETER(dev);
    return HidComplete(irp, irp->IoStatus.Status, 0);
}

/* ------------------------------------------------------------------ */
/* AddDevice                                                           */
/* ------------------------------------------------------------------ */

static NTSTATUS NTAPI HidAddDevice(PDRIVER_OBJECT drv, PDEVICE_OBJECT pdo)
{
    MINIREC *m = FindMini(drv);
    PDEVICE_OBJECT fdo, next;
    PHIDFDO x;
    NTSTATUS st;

    if (!m)
        return STATUS_UNSUCCESSFUL;
    HIDDBG(("[HIDCLASS] AddDevice pdo=%x\n", (ULONG)pdo));
    HidSweepZombies();

    st = IoCreateDevice(drv, HIDFDO_SIZE + m->MiniExtSize, NULL,
                        FILE_DEVICE_UNKNOWN, 0, FALSE, &fdo);
    if (!NT_SUCCESS(st))
        return st;
    x = (PHIDFDO)fdo->DeviceExtension;
    RtlZeroMemory(x, HIDFDO_SIZE + m->MiniExtSize);
    x->Kind = KIND_FDO;
    x->Self = fdo;
    x->Mini = m;
    x->Hdr.PhysicalDeviceObject = pdo;
    x->Hdr.MiniDeviceExtension = (PUCHAR)x + HIDFDO_SIZE;

    next = IoAttachDeviceToDeviceStack(fdo, pdo);
    if (!next) {
        IoDeleteDevice(fdo);
        return STATUS_UNSUCCESSFUL;
    }
    x->Hdr.NextDeviceObject = next;

    /* the minidriver is run as one more layer of this device object */
    fdo->StackSize++;
    fdo->Flags |= (next->Flags & (DO_POWER_PAGABLE | DO_BUFFERED_IO | DO_DIRECT_IO));

    st = m->MiniAddDevice(drv, fdo);
    if (!NT_SUCCESS(st)) {
        HIDDBG(("[HIDCLASS] minidriver AddDevice failed %x\n", st));
        IoDetachDevice(next);
        IoDeleteDevice(fdo);
        return st;
    }
    fdo->Flags &= ~DO_DEVICE_INITIALIZING;
    return STATUS_SUCCESS;
}

/*
 * The control device that HIDEXT.VXD talks to at KERNEL32_SHUTDOWN
 * (\DosDevices\HIDSHUT0).  It belongs to the first minidriver's driver object, so
 * it is served by the dispatch routines above (KIND_CTL).
 */
static BOOLEAN g_CtlMade;

static void MakeShutdownDevice(PDRIVER_OBJECT drv)
{
    UNICODE_STRING name, link;
    PDEVICE_OBJECT dev;
    PHIDFDO x;
    NTSTATUS st;

    if (g_CtlMade)
        return;
    RtlInitUnicodeString(&name, L"\\Device\\HidShut0");
    RtlInitUnicodeString(&link, L"\\DosDevices\\HIDSHUT0");
    st = IoCreateDevice(drv, HIDFDO_SIZE, &name, FILE_DEVICE_UNKNOWN, 0, FALSE, &dev);
    if (!NT_SUCCESS(st)) {
        HIDDBG(("[HIDCLASS] HIDSHUT0 device failed %x\n", st));
        return;
    }
    x = (PHIDFDO)dev->DeviceExtension;
    RtlZeroMemory(x, HIDFDO_SIZE);
    x->Kind = KIND_CTL;
    x->Self = dev;
    dev->Flags |= DO_BUFFERED_IO;
    st = IoCreateSymbolicLink(&link, &name);
    if (!NT_SUCCESS(st)) {
        HIDDBG(("[HIDCLASS] HIDSHUT0 link failed %x\n", st));
        IoDeleteDevice(dev);
        return;
    }
    dev->Flags &= ~DO_DEVICE_INITIALIZING;
    g_CtlMade = TRUE;
    HIDDBG(("[HIDCLASS] \\\\.\\HIDSHUT0 ready\n"));
}

/* ------------------------------------------------------------------ */
/* HidRegisterMinidriver                                               */
/* ------------------------------------------------------------------ */

NTSTATUS NTAPI HidRegisterMinidriver(PHID_MINIDRIVER_REGISTRATION reg)
{
    PDRIVER_OBJECT drv;
    MINIREC *m;

    if (!reg || !reg->DriverObject)
        return STATUS_INVALID_PARAMETER;
    drv = reg->DriverObject;
    if (FindMini(drv))
        return STATUS_SUCCESS;
    m = (MINIREC *)ExAllocatePoolWithTag(NonPagedPool, sizeof(MINIREC), HIDCLS_TAG);
    if (!m)
        return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(m, sizeof(MINIREC));
    m->Driver = drv;
    m->MiniExtSize = (reg->DeviceExtensionSize + 7) & ~7UL;
    m->Polled = reg->DevicesArePolled;
    m->MiniPnp = drv->MajorFunction[IRP_MJ_PNP];
    m->MiniPower = drv->MajorFunction[IRP_MJ_POWER];
    m->MiniAddDevice = drv->DriverExtension->AddDevice;
    if (!m->MiniPnp)
        m->MiniPnp = HidDispatchOther;
    m->Next = g_Minis;
    g_Minis = m;

    /* from here on HIDCLASS handles the driver's IRPs; the minidriver keeps
     * only IRP_MJ_INTERNAL_DEVICE_CONTROL (its IOCTL handler) */
    drv->MajorFunction[IRP_MJ_CREATE]         = HidDispatchCreate;
    drv->MajorFunction[IRP_MJ_CLOSE]          = HidDispatchClose;
    drv->MajorFunction[IRP_MJ_CLEANUP]        = HidDispatchCleanup;
    drv->MajorFunction[IRP_MJ_READ]           = HidDispatchRead;
    drv->MajorFunction[IRP_MJ_WRITE]          = HidDispatchWrite;
    drv->MajorFunction[IRP_MJ_DEVICE_CONTROL] = HidDispatchIoctl;
    drv->MajorFunction[IRP_MJ_SYSTEM_CONTROL] = HidDispatchOther;
    drv->MajorFunction[IRP_MJ_PNP]            = HidDispatchPnp;
    drv->MajorFunction[IRP_MJ_POWER]          = HidDispatchPower;
    drv->DriverExtension->AddDevice           = HidAddDevice;
    MakeShutdownDevice(drv);
    HIDDBG(("[HIDCLASS] minidriver registered: driver %x, extension %u bytes\n",
            (ULONG)drv, m->MiniExtSize));
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI DriverEntry(PDRIVER_OBJECT drv, PUNICODE_STRING regPath)
{
    UNREFERENCED_PARAMETER(drv);
    UNREFERENCED_PARAMETER(regPath);
    HIDDBG(("[HIDCLASS] loaded (HIDCLASS H2, built " __DATE__ " " __TIME__ ")\n"));
    return STATUS_SUCCESS;
}

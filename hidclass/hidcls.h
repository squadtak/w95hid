/*
 * hidcls.h - HIDCLASS.SYS for Windows 95 OSR2.1 (stage H1)
 *
 * The HID class driver, written to the 98DDK interface (hidport.h /
 * hidclass.h): a HID minidriver (HIDUSB.SYS, DDK HIDMINI, ...) calls
 * HidRegisterMinidriver() from its DriverEntry and HIDCLASS takes over the
 * driver object:
 *
 *   - HIDCLASS's AddDevice creates the device's FDO (attached to the PDO
 *     the bus driver made) and calls the minidriver's AddDevice with it.
 *   - PnP/power IRPs reach HIDCLASS first; for the FDO it runs the
 *     minidriver's handler as a layer of its own (IRP stack location pushed),
 *     so the minidriver does the talking to the bus below.
 *   - After START, HIDCLASS reads the descriptors through the minidriver's
 *     IRP_MJ_INTERNAL_DEVICE_CONTROL (HIDPORT.H IOCTLs), parses them with
 *     HIDPARSE.SYS and creates one child PDO per top-level collection.
 *   - Clients (KBDHID.VXD, MOUHID.VXD, HID.DLL) open a collection as
 *     \\.\HIDn (a symbolic link: OSR2.1 has no device interfaces).
 *
 * OSR2.1 has no power manager exports, no device interfaces and no driver
 * object extensions; this file's code never uses them.
 */
#ifndef _HIDCLS_H
#define _HIDCLS_H

#include <wdm.h>
#include <hidusage.h>
#include <hidpi.h>
#include <hidpddi.h>
#include <hidport.h>

#include "..\hidtrace.h"       /* COM1 / speaker breadcrumbs for the shutdown hang */

#define HIDCLS_TAG          'lCdH'

/* One DbgPrint per message (DebugView). */
#ifndef HIDCLS_NODEBUG
#define HIDDBG(_x_)         DbgPrint _x_
#else
#define HIDDBG(_x_)
#endif

#define KIND_FDO            0x4F444648UL        /* 'HFDO' */
#define KIND_PDO            0x4F445048UL        /* 'HPDO' */
#define KIND_CTL            0x4C544348UL        /* 'HCTL': the HIDSHUT0 control device */

#define HID_MAX_COLLECTIONS 16
#define HID_RING_SLOTS      16      /* reports kept per collection until read */
#define HID_MAX_PORTS       64      /* \\.\HID0 .. \\.\HID63 (the clients look for these numbers) */
#define HID_MAX_REPORT      72      /* 64 (full-speed interrupt endpoint maximum) + the report id byte */

/*
 * The private IOCTLs of the collection devices (LOG: a text line for DebugView,
 * a VxD has no DbgPrint of its own) and of the control device \DosDevices\HIDSHUT0
 * that HIDEXT.VXD tells about the shutdown ("from now on no bus I/O"), and the
 * feature / string / queue requests of HID.DLL: see hidpriv.h.
 */
#include "..\hidpriv.h"
VOID HidSetShutdown(VOID);

/*
 * HIDUSB.SYS only (our own minidriver; others just fail it): after a suspend /
 * resume the device is configured again.  Same value in hidusb.h.
 */
#define IOCTL_HIDUSB_RESTART    HID_CTL_CODE(0x40)

/*
 * Class requests to the device through its control pipe, for HID.DLL (also
 * HIDUSB.SYS only).  They take a HID_XFER_PACKET in Irp->UserBuffer, filled as
 * for IOCTL_HID_WRITE_REPORT: with a report id the buffer starts with it, with
 * id 0 it does not.  GET_* fill the buffer.
 */
#define IOCTL_HIDUSB_GET_FEATURE    HID_CTL_CODE(0x41)
#define IOCTL_HIDUSB_SET_FEATURE    HID_CTL_CODE(0x42)
#define IOCTL_HIDUSB_GET_INPUT      HID_CTL_CODE(0x43)

/* a registered minidriver (one per minidriver driver object) */
typedef struct _MINIREC {
    struct _MINIREC    *Next;
    PDRIVER_OBJECT      Driver;
    PDRIVER_DISPATCH    MiniPnp;            /* the minidriver's own handlers */
    PDRIVER_DISPATCH    MiniPower;
    PDRIVER_ADD_DEVICE  MiniAddDevice;
    ULONG               MiniExtSize;
    BOOLEAN             Polled;
} MINIREC;

/*
 * The FDO's device extension starts with HID_DEVICE_EXTENSION (what the
 * minidriver's GET_MINIDRIVER_DEVICE_EXTENSION expects); the minidriver's
 * own extension follows the HIDFDO.
 */
typedef struct _HIDFDO {
    HID_DEVICE_EXTENSION    Hdr;                /* must be first */
    ULONG                   Kind;               /* KIND_FDO (same offset in HIDPDO) */
    PDEVICE_OBJECT          Self;
    MINIREC                *Mini;
    HID_DEVICE_ATTRIBUTES   Attr;
    ULONG                   NumChildren;
    PDEVICE_OBJECT          Child[HID_MAX_COLLECTIONS];
    BOOLEAN                 Started;
    BOOLEAN                 Removing;
    BOOLEAN                 Suspended;          /* SURPRISE_REMOVAL seen: waiting for the bus to come back */
    BOOLEAN                 UsesIds;           /* reports start with a report id byte */
    BOOLEAN                 WantReads;          /* someone has a collection open */
    UCHAR                   IdToCol[256];       /* report id -> collection index (0xFF: none) */
    LONG                    OpenTotal;          /* opens over all collections */

    /* the continuous IOCTL_HID_READ_REPORT to the minidriver */
    PIRP                    ReadIrp;
    PUCHAR                  ReadBuf;
    ULONG                   ReadBufLen;
    BOOLEAN                 Reading;            /* the IRP is down in the minidriver */
    UCHAR                   Pad[3];
    LONG                    Pumping;
    LONG                    Again;
} HIDFDO, *PHIDFDO;

/* the minidriver's extension follows the FDO's own, 8-aligned */
#define HIDFDO_SIZE         ((sizeof(HIDFDO) + 7) & ~7UL)

/* a collection's child PDO */
typedef struct _HIDOPEN {
    LIST_ENTRY              Link;
    PFILE_OBJECT            Fo;                 /* the open it belongs to */
    ULONG                   RingHead, RingCount;
    ULONG                   RingLimit;          /* reports kept for a slow reader (1..HID_RING_SLOTS) */
    UCHAR                   RingLen[HID_RING_SLOTS];
    UCHAR                   Ring[HID_RING_SLOTS][HID_MAX_REPORT];
} HIDOPEN, *PHIDOPEN;

typedef struct _HIDPDO {
    HID_DEVICE_EXTENSION    Hdr;                /* unused: keeps Kind at the same offset */
    ULONG                   Kind;               /* KIND_PDO */
    PDEVICE_OBJECT          Self;
    PHIDFDO                 Fdo;                /* NULL once the FDO is gone */
    ULONG                   Serial;             /* global number, names the device and the link */
    ULONG                   Index;              /* collection number 0.. */
    ULONG                   NumCollections;     /* of the parent device */
    HID_DEVICE_ATTRIBUTES   Attr;
    USAGE                   UsagePage, Usage;
    USHORT                  InputLength, OutputLength, FeatureLength;
    USHORT                  PreparsedLength;
    PHIDP_PREPARSED_DATA    Preparsed;          /* own copy */
    BOOLEAN                 Present;            /* still in the parent's BusRelations */
    BOOLEAN                 Reported;           /* PnP has been told about it */
    BOOLEAN                 Started;
    BOOLEAN                 LinkMade;
    BOOLEAN                 RemoveSeen;         /* PnP has sent REMOVE_DEVICE to it */
    BOOLEAN                 Zombie;             /* waiting in the zombie list for its last reference */
    LONG                    OpenCount;

    /* reports from the minidriver: waiting for a reader / readers waiting */
    KSPIN_LOCK              QLock;
    LIST_ENTRY              Reads;
    LIST_ENTRY              Clients;            /* HIDOPEN: one per open handle, each gets every report */

    WCHAR                   DeviceName[40];
    WCHAR                   LinkName[40];
} HIDPDO, *PHIDPDO;

/* hidclass.c */
VOID     HidPumpReads(PHIDFDO x);
VOID     HidDeactivate(PHIDFDO x);
NTSTATUS HidComplete(PIRP Irp, NTSTATUS Status, ULONG Information);
NTSTATUS HidPassDown(PHIDFDO x, PIRP Irp);

/* hidpdo.c */
NTSTATUS HidCreateChildren(PHIDFDO x);
VOID     HidDropChildren(PHIDFDO x);
VOID     HidSweepZombies(VOID);
NTSTATUS HidPdoPnp(PDEVICE_OBJECT Dev, PIRP Irp);
NTSTATUS HidPdoPower(PDEVICE_OBJECT Dev, PIRP Irp);
NTSTATUS HidPdoCreate(PDEVICE_OBJECT Dev, PIRP Irp);
NTSTATUS HidPdoClose(PDEVICE_OBJECT Dev, PIRP Irp);
NTSTATUS HidPdoRead(PDEVICE_OBJECT Dev, PIRP Irp);
NTSTATUS HidPdoWrite(PDEVICE_OBJECT Dev, PIRP Irp);
NTSTATUS HidPdoIoctl(PDEVICE_OBJECT Dev, PIRP Irp);
NTSTATUS HidPdoCleanup(PDEVICE_OBJECT Dev, PIRP Irp);
VOID     HidDeliver(PHIDFDO x, PUCHAR Data, ULONG Length);

/* minidriver IOCTL helper (hidclass.c) */
NTSTATUS HidCallMini(PHIDFDO x, ULONG Code, PVOID OutBuf, ULONG OutLen, PVOID InBuf, ULONG InLen);
NTSTATUS HidCallMiniEx(PHIDFDO x, ULONG Code, PVOID OutBuf, ULONG OutLen, PVOID InBuf, ULONG InLen,
                       PULONG Info);

#endif

/*
 * hidusb.h - HIDUSB.SYS for Windows 95 OSR2.1 (E1: bring-up version)
 *
 * Stage E1 only proves that the driver loads on the OSR2.1 USB stack,
 * binds to a USB HID interface, reads the descriptors and the interrupt-IN
 * reports and logs them with DbgPrint (DebugView).  Later stages turn this
 * into the HID minidriver for HIDCLASS.SYS (HIDPORT.H).
 *
 * Win95 OSR2.1 has no power manager exports (PoCallDriver,
 * PoStartNextPowerIrp), so power IRPs are simply passed down.
 */
#ifndef _HIDUSB_H
#define _HIDUSB_H

#include <wdm.h>
#include <usb100.h>
#include <usbdi.h>
#include <usbdlib.h>
#include <hidport.h>

#include "..\hidtrace.h"       /* COM1 / speaker breadcrumbs for the shutdown hang */

#define HIDUSB_TAG      'BSUH'

/* HIDCLASS asks for this after a suspend / resume (same value in hidcls.h) */
#define IOCTL_HIDUSB_RESTART    HID_CTL_CODE(0x40)

/* class requests through the control pipe for HID.DLL (same values in hidcls.h):
 * a HID_XFER_PACKET in Irp->UserBuffer as for IOCTL_HID_WRITE_REPORT */
#define IOCTL_HIDUSB_GET_FEATURE    HID_CTL_CODE(0x41)
#define IOCTL_HIDUSB_SET_FEATURE    HID_CTL_CODE(0x42)
#define IOCTL_HIDUSB_GET_INPUT      HID_CTL_CODE(0x43)

#define HID_RING_SLOTS  8
#define HID_MAX_REPORT  64      /* full-speed interrupt endpoint maximum */

/* 1 = DbgPrint every report that differs from the previous one (a mouse makes
 * one per movement and slows the machine down; for debugging only) */
#ifndef HIDUSB_LOG_REPORTS
#define HIDUSB_LOG_REPORTS  0
#endif

/* One DbgPrint per message: DebugView shows each call as its own line. */
#ifndef HIDUSB_NODEBUG
#define HIDDBG(_x_)     DbgPrint _x_
#else
#define HIDDBG(_x_)
#endif

/* USB HID class 1.0, 6.2.1 */
#define USB_INTERFACE_CLASS_HID     0x03
#define USB_HID_DESCRIPTOR_TYPE     0x21
#define USB_HID_REPORT_TYPE         0x22

#pragma pack(push, 1)
typedef struct _HID_DESCRIPTOR_ENTRY {
    UCHAR  bDescriptorType;
    USHORT wDescriptorLength;
} HID_DESCRIPTOR_ENTRY;

typedef struct _USB_HID_DESCRIPTOR {
    UCHAR  bLength;
    UCHAR  bDescriptorType;         /* 0x21 */
    USHORT bcdHID;
    UCHAR  bCountryCode;
    UCHAR  bNumDescriptors;
    HID_DESCRIPTOR_ENTRY Descriptors[1];
} USB_HID_DESCRIPTOR, *PUSB_HID_DESCRIPTOR;
#pragma pack(pop)

#define HID_MAX_IF      4       /* HID interfaces of one device that we take */

struct _DEVEXT;
struct _IFSLOT;

/*
 * What one interrupt IN IRP needs, kept apart from the device extension so that
 * the IRP can outlive the FDO.  During a suspend the cancel of a pending IRP
 * completes only when the hub has handled the resume; waiting for it there
 * blocks the PnP thread that hub needs (deadlock).  PollStop therefore waits
 * a moment, then lets the completion routine free all of this when it comes.
 */
typedef struct _POLLRES {
    struct _IFSLOT *Slot;       /* NULL once PollStop has given up on the IRP */
    PIRP            Irp;
    PURB            Urb;
    PUCHAR          Buf;
    LONG            Own;        /* 0, 1 = PollStop done with it, 2 = completion done; the 2nd to arrive frees */
} POLLRES, *PPOLLRES;

/* One HID interface of the device: its own report descriptor and its own
 * interrupt IN pipe.  With more than one live interface the descriptors are
 * merged into one for HIDCLASS, every interface getting its own report ids. */
typedef struct _IFSLOT {
    struct _DEVEXT                 *De;
    ULONG                           Index;
    PUSB_INTERFACE_DESCRIPTOR       IfDesc;         /* points into ConfigDesc */
    PUSB_HID_DESCRIPTOR             HidDesc;        /* points into ConfigDesc */
    PUCHAR                          ReportDesc;     /* as the device has it (or the built-in boot one) */
    ULONG                           ReportDescLen;
    PUCHAR                          Mapped;         /* with report ids made unique (merged mode) */
    ULONG                           MappedLen;
    BOOLEAN                         Live;           /* has a descriptor and an interrupt IN pipe */
    BOOLEAN                         UsesIds;        /* the device's own report ids */
    UCHAR                           FixedId;        /* merged id of an id-less interface */
    UCHAR                           IdAdd;          /* merged id = device id + IdAdd */
    UCHAR                           IdLo, IdHi;     /* merged ids that belong to this interface */

    /* interrupt IN polling */
    USBD_PIPE_HANDLE                InPipe;
    ULONG                           InMaxPacket;
    PPOLLRES                        Pr;             /* the IRP, its URB and buffer */
    LONG                            PollActive;     /* 1 while an IRP is down in USBD */
    LONG                            Stopping;
    ULONG                           PollErrors;
    ULONG                           Reports;
    ULONG                           LastLen;
    UCHAR                           LastReport[16];
    KEVENT                          PollIdle;       /* signalled when PollActive drops to 0 */
} IFSLOT, *PIFSLOT;

typedef struct _DEVEXT {
    PDEVICE_OBJECT  Self;
    PDEVICE_OBJECT  Pdo;
    PDEVICE_OBJECT  TopOfStack;
    BOOLEAN         Started;
    BOOLEAN         QuerySeen;          /* a QUERY_STOP / QUERY_REMOVE came in before */

    PUSB_DEVICE_DESCRIPTOR          DevDesc;
    PUSB_CONFIGURATION_DESCRIPTOR   ConfigDesc;
    PUSB_CONFIGURATION_DESCRIPTOR   SelCfg;         /* ConfigDesc minus class descriptors, for USBD */
    USBD_CONFIGURATION_HANDLE       ConfigHandle;

    IFSLOT          Slots[HID_MAX_IF];
    ULONG           NumSlots;
    BOOLEAN         Merged;                         /* more than one live interface */
    PUSB_HID_DESCRIPTOR             HidDesc;        /* of the first live interface; its report length is patched */
    PUCHAR          ReportDesc;                     /* what HIDCLASS gets */
    ULONG           ReportDescLen;

    /* IOCTL_HID_READ_REPORT: reports waiting for a reader / readers waiting */
    KSPIN_LOCK      QLock;
    LIST_ENTRY      ReadIrps;
    ULONG           RingHead, RingCount;
    UCHAR           RingLen[HID_RING_SLOTS];
    UCHAR           Ring[HID_RING_SLOTS][HID_MAX_REPORT];
} DEVEXT, *PDEVEXT;

#endif

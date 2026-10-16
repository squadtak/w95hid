/*
 * hidpriv.h - the IOCTLs HIDCLASS's collection devices (\\.\HIDn) understand
 * beyond the NT codes of HIDCLASS.H, and the control device of HIDEXT.VXD.
 *
 * Used by HIDCLASS.SYS, HID.DLL, HIDEXT.VXD's clients and the test tools.  The
 * NT codes for the feature report, the strings and the queue are METHOD_*_DIRECT,
 * which the Win95 NTKERN is not known to map for a Win32 caller; these are
 * METHOD_BUFFERED (input and output share one buffer; DeviceIoControl may pass
 * the same pointer for both) and are what HID.DLL uses.
 *
 * Needs CTL_CODE (winioctl.h or wdm.h) before it.
 */
#ifndef _HIDPRIV_H
#define _HIDPRIV_H

#define HIDPRIV_CODE(_n_)   CTL_CODE(FILE_DEVICE_UNKNOWN, 0x900 + (_n_), METHOD_BUFFERED, FILE_ANY_ACCESS)

/* a text line for DebugView (HIDCLASS prints it as "[HIDCLIENT] ...") */
#define IOCTL_HIDCLS_LOG            HIDPRIV_CODE(0x00)      /* 0x00222400 */

/* HIDEXT.VXD -> HIDCLASS's control device \DosDevices\HIDSHUT0: the system is going down */
#define IOCTL_HIDEXT_SHUTDOWN       HIDPRIV_CODE(0x02)      /* 0x00222408 */

/*
 * Reports.  A report buffer is in the NT form: byte 0 is the report id (0 when
 * the device has no ids) and the length includes it (HIDP_CAPS lengths).
 */
/* in: byte 0 = report id; out: the feature report (length = FeatureReportByteLength) */
#define IOCTL_HIDCLS_GET_FEATURE    HIDPRIV_CODE(0x10)
/* in: the feature report to send */
#define IOCTL_HIDCLS_SET_FEATURE    HIDPRIV_CODE(0x11)
/* in: byte 0 = report id; out: the current input report, asked for with GET_REPORT */
#define IOCTL_HIDCLS_GET_INPUT      HIDPRIV_CODE(0x12)

/*
 * Strings.  in: a DWORD, out: a NUL-terminated wide string.
 * 1 = manufacturer, 2 = product, 3 = serial number, or HIDSTR_INDEX(index, langid).
 */
#define IOCTL_HIDCLS_GET_STRING     HIDPRIV_CODE(0x13)
#define HIDSTR_MANUFACTURER         1
#define HIDSTR_PRODUCT              2
#define HIDSTR_SERIALNUMBER         3
#define HIDSTR_INDEX(_i_, _lang_)   (0x100UL | ((unsigned long)(_i_) & 0xFF) | ((unsigned long)(_lang_) << 16))

/* out: the preparsed data (HID_COLLECTION_INFORMATION.DescriptorSize bytes) */
#define IOCTL_HIDCLS_GET_PREPARSED  HIDPRIV_CODE(0x14)

/* drop the reports waiting to be read */
#define IOCTL_HIDCLS_FLUSH_QUEUE    HIDPRIV_CODE(0x15)

/* out / in: a DWORD, the number of input reports kept for a reader that is slow */
#define IOCTL_HIDCLS_GET_NUM_BUFFERS HIDPRIV_CODE(0x16)
#define IOCTL_HIDCLS_SET_NUM_BUFFERS HIDPRIV_CODE(0x17)

#endif

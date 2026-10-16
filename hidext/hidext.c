/*
 * hidext.c - HIDEXT.VXD for Windows 95 OSR2.1, see hidext.asm
 *
 * At KERNEL32_SHUTDOWN: tell every WDM driver that owns a control device
 * \DosDevices\HIDSHUT0..7 that the system is going down (IOCTL_HIDEXT_SHUTDOWN).
 * No CRT, no VMM heap: everything lives on the stack.
 */
typedef unsigned long   DWORD;
typedef DWORD           HANDLE;
typedef unsigned short  USHORT;
typedef unsigned char   UCHAR;
typedef void            *PVOID;

typedef struct { long Status; DWORD Information; } IOSB;
typedef struct { USHORT Length, MaximumLength; USHORT *Buffer; } USTR;
typedef struct {
    DWORD Length; HANDLE RootDirectory; USTR *ObjectName; DWORD Attributes;
    PVOID SecurityDescriptor; PVOID SecurityQos;
} OBJATTR;

extern long __stdcall NtKernCreateFile(HANDLE *h, DWORD access, OBJATTR *oa, IOSB *iosb,
                                       PVOID alloc, DWORD attr, DWORD share, DWORD disp,
                                       DWORD opts, PVOID ea, DWORD ealen);
extern long __stdcall NtKernClose(HANDLE h);
extern long __stdcall NtKernDeviceIoControl(HANDLE h, PVOID event, PVOID apc, PVOID ctx,
                                            IOSB *iosb, DWORD code, PVOID in, DWORD inlen,
                                            PVOID out, DWORD outlen);

#pragma code_seg("_LTEXT", "LCODE")
#pragma data_seg("_LDATA", "LCODE")

#define TRC_VXD
#include "..\hidtrace.h"        /* COM1 breadcrumbs */

#define IOCTL_HIDEXT_SHUTDOWN   0x00222408UL    /* CTL_CODE(0x22, 0x902, BUFFERED, ANY) */
#define VMM_KERNEL32_SHUTDOWN   0x32
#define HIDSHUT_MAX             8

static void NotifyShutdown(void)
{
    static const char prefix[] = "\\DosDevices\\HIDSHUT";
    USHORT name[32];
    USTR us;
    OBJATTR oa;
    IOSB iosb;
    HANDLE h;
    DWORD n, len, i;

    for (n = 0; n < HIDSHUT_MAX; n++) {
        len = 0;
        for (i = 0; prefix[i]; i++)
            name[len++] = (USHORT)prefix[i];
        name[len++] = (USHORT)('0' + n);
        name[len] = 0;
        us.Length = (USHORT)(len * 2);
        us.MaximumLength = (USHORT)(len * 2 + 2);
        us.Buffer = name;
        oa.Length = sizeof(oa);
        oa.RootDirectory = 0;
        oa.ObjectName = &us;
        oa.Attributes = 0x40;                   /* OBJ_CASE_INSENSITIVE */
        oa.SecurityDescriptor = 0;
        oa.SecurityQos = 0;

        h = 0;
        if (NtKernCreateFile(&h, 0x80100080UL, &oa, &iosb, 0, 0x80, 3, 1, 0, 0, 0) < 0 || !h)
            continue;
        NtKernDeviceIoControl(h, 0, 0, 0, &iosb, IOCTL_HIDEXT_SHUTDOWN, 0, 0, 0, 0);
        TRC("X.shutdown sent", n);
        NtKernClose(h);
    }
}

/* every control message the VMM sends (hidext.asm passes the number in) */
void __stdcall HidExtCtl(DWORD msg)
{
    if (msg == VMM_KERNEL32_SHUTDOWN) {
        TRC("X.KERNEL32_SHUTDOWN", 0);
        NotifyShutdown();
    }
}

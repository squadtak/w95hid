/*
 * hiddll.c - HID.DLL for Windows 95 OSR2.1: the user-mode HID API (HIDSDI.H)
 *
 * HidD_*  talk to a collection device of HIDCLASS (\\.\HIDn) with DeviceIoControl.
 *         They use the METHOD_BUFFERED requests of hidpriv.h, because the Win95
 *         NTKERN is not known to map the METHOD_*_DIRECT codes of HIDCLASS.H for
 *         a Win32 caller.
 * HidP_*  are the very functions of HIDPARSE.SYS (hidparse.c, built in here).
 *
 * Win95 has no device interfaces, so there is no SetupDi enumeration: a program
 * opens \\.\HID0 .. \\.\HID63, or asks HidX_OpenDevice / HidX_GetDevicePath
 * (extensions of this DLL).  Report buffers follow the NT form: byte 0 is the
 * report id (0 when the device has none) and the length includes it.
 *
 * Build: see makefile (MSVC 5.0, static C runtime: the DLL must not need MSVCRT).
 */
#include <windows.h>
#include <winioctl.h>
#include <stdlib.h>
#include <string.h>
#include "hidsdi.h"
#include "..\hidpriv.h"

/* HIDCLASS.H: HID_BUFFER_CTL_CODE(106) */
#define IOCTL_HID_GET_COLLECTION_INFORMATION \
    CTL_CODE(FILE_DEVICE_KEYBOARD, 106, METHOD_BUFFERED, FILE_ANY_ACCESS)

typedef struct {
    ULONG   DescriptorSize;
    BOOLEAN Polled;
    UCHAR   Reserved1[1];
    USHORT  VendorID;
    USHORT  ProductID;
    USHORT  VersionNumber;
} COLLECTION_INFO;

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH)
        DisableThreadLibraryCalls(inst);
    return TRUE;
}

static BOOL Ioctl(HANDLE h, DWORD code, LPVOID in, DWORD inLen, LPVOID out, DWORD outLen)
{
    DWORD got = 0;
    return DeviceIoControl(h, code, in, inLen, out, outLen, &got, NULL);
}

/* ------------------------------------------------------------------ */
/* HidD_*                                                              */
/* ------------------------------------------------------------------ */

static BOOL GetInfo(HANDLE h, COLLECTION_INFO *ci)
{
    memset(ci, 0, sizeof(*ci));
    return Ioctl(h, IOCTL_HID_GET_COLLECTION_INFORMATION, NULL, 0, ci, sizeof(*ci));
}

BOOLEAN __stdcall HidD_GetAttributes(HANDLE h, PHIDD_ATTRIBUTES a)
{
    COLLECTION_INFO ci;

    if (!a || !GetInfo(h, &ci))
        return FALSE;
    a->Size = sizeof(*a);
    a->VendorID = ci.VendorID;
    a->ProductID = ci.ProductID;
    a->VersionNumber = ci.VersionNumber;
    return TRUE;
}

/* the class of HID devices: the same GUID as on NT (Windows 95 does not use it) */
void __stdcall HidD_GetHidGuid(LPGUID g)
{
    static const GUID hid = { 0x4D1E55B2L, 0xF16F, 0x11CF,
                              { 0x88, 0xCB, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30 } };
    *g = hid;
}

BOOLEAN __stdcall HidD_GetPreparsedData(HANDLE h, PHIDP_PREPARSED_DATA *ppd)
{
    COLLECTION_INFO ci;
    PVOID buf;

    if (!ppd || !GetInfo(h, &ci) || !ci.DescriptorSize)
        return FALSE;
    buf = malloc(ci.DescriptorSize);
    if (!buf) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }
    if (!Ioctl(h, IOCTL_HIDCLS_GET_PREPARSED, NULL, 0, buf, ci.DescriptorSize)) {
        free(buf);
        return FALSE;
    }
    *ppd = (PHIDP_PREPARSED_DATA)buf;
    return TRUE;
}

BOOLEAN __stdcall HidD_FreePreparsedData(PHIDP_PREPARSED_DATA ppd)
{
    free(ppd);
    return TRUE;
}

BOOLEAN __stdcall HidD_FlushQueue(HANDLE h)
{
    return (BOOLEAN)Ioctl(h, IOCTL_HIDCLS_FLUSH_QUEUE, NULL, 0, NULL, 0);
}

BOOLEAN __stdcall HidD_GetNumInputBuffers(HANDLE h, PULONG n)
{
    return (BOOLEAN)Ioctl(h, IOCTL_HIDCLS_GET_NUM_BUFFERS, NULL, 0, n, sizeof(*n));
}

BOOLEAN __stdcall HidD_SetNumInputBuffers(HANDLE h, ULONG n)
{
    return (BOOLEAN)Ioctl(h, IOCTL_HIDCLS_SET_NUM_BUFFERS, &n, sizeof(n), NULL, 0);
}

BOOLEAN __stdcall HidD_GetConfiguration(HANDLE h, PHIDD_CONFIGURATION c, ULONG len)
{
    if (!c || len < sizeof(*c)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    c->cookie = NULL;
    c->size = sizeof(*c);
    return HidD_GetNumInputBuffers(h, &c->RingBufferSize);
}

BOOLEAN __stdcall HidD_SetConfiguration(HANDLE h, PHIDD_CONFIGURATION c, ULONG len)
{
    if (!c || len < sizeof(*c)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    return HidD_SetNumInputBuffers(h, c->RingBufferSize);
}

/* the buffer holds the report id on the way in and the whole report on the way out */
BOOLEAN __stdcall HidD_GetFeature(HANDLE h, PVOID buf, ULONG len)
{
    return (BOOLEAN)Ioctl(h, IOCTL_HIDCLS_GET_FEATURE, buf, len, buf, len);
}

BOOLEAN __stdcall HidD_SetFeature(HANDLE h, PVOID buf, ULONG len)
{
    return (BOOLEAN)Ioctl(h, IOCTL_HIDCLS_SET_FEATURE, buf, len, NULL, 0);
}

BOOLEAN __stdcall HidD_GetPhysicalDescriptor(HANDLE h, PVOID buf, ULONG len)
{
    (void)h; (void)buf; (void)len;
    SetLastError(ERROR_NOT_SUPPORTED);
    return FALSE;
}

/* a string as a wide, NUL-terminated string; the selector goes in through the same buffer */
static BOOLEAN GetStr(HANDLE h, DWORD sel, PVOID buf, ULONG len)
{
    LPBYTE tmp;
    DWORD got = 0;
    BOOL ok;

    if (!buf || len < sizeof(WCHAR)) {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    tmp = (LPBYTE)LocalAlloc(LMEM_FIXED, len < sizeof(DWORD) ? sizeof(DWORD) : len);
    if (!tmp) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }
    *(DWORD *)tmp = sel;
    ok = DeviceIoControl(h, IOCTL_HIDCLS_GET_STRING, tmp, sizeof(DWORD), tmp, len, &got, NULL);
    if (ok)
        memcpy(buf, tmp, got < len ? got : len);
    LocalFree(tmp);
    return (BOOLEAN)ok;
}

BOOLEAN __stdcall HidD_GetManufacturerString(HANDLE h, PVOID buf, ULONG len)
{
    return GetStr(h, HIDSTR_MANUFACTURER, buf, len);
}

BOOLEAN __stdcall HidD_GetProductString(HANDLE h, PVOID buf, ULONG len)
{
    return GetStr(h, HIDSTR_PRODUCT, buf, len);
}

BOOLEAN __stdcall HidD_GetSerialNumberString(HANDLE h, PVOID buf, ULONG len)
{
    return GetStr(h, HIDSTR_SERIALNUMBER, buf, len);
}

BOOLEAN __stdcall HidD_GetIndexedString(HANDLE h, ULONG index, PVOID buf, ULONG len)
{
    if (index == 0 || index > 255) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    return GetStr(h, HIDSTR_INDEX(index, 0), buf, len);
}

/* ------------------------------------------------------------------ */
/* Windows 95 extensions: finding the devices without SetupDi          */
/* ------------------------------------------------------------------ */

static void DevPath(DWORD index, LPSTR path)
{
    wsprintfA(path, "\\\\.\\HID%lu", index);
}

/* opens collection index (0..63) for reading and writing; INVALID_HANDLE_VALUE if there is none */
HANDLE __stdcall HidX_OpenDevice(DWORD index)
{
    char path[32];

    DevPath(index, path);
    return CreateFileA(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                       NULL, OPEN_EXISTING, 0, NULL);
}

/* TRUE (and the path) when \\.\HID<index> exists */
BOOL __stdcall HidX_GetDevicePath(DWORD index, LPSTR path, DWORD size)
{
    HANDLE h;

    if (!path || size < 16) {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    h = HidX_OpenDevice(index);
    if (h == INVALID_HANDLE_VALUE)
        return FALSE;
    CloseHandle(h);
    DevPath(index, path);
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* HidP_GetButtonCaps / HidP_GetValueCaps as functions (hidpi.h has macros) */
/* ------------------------------------------------------------------ */
#undef HidP_GetButtonCaps
#undef HidP_GetValueCaps

NTSTATUS __stdcall HidP_GetButtonCaps(HIDP_REPORT_TYPE type, PHIDP_BUTTON_CAPS caps,
                                      PUSHORT len, PHIDP_PREPARSED_DATA pd)
{
    return HidP_GetSpecificButtonCaps(type, 0, 0, 0, caps, len, pd);
}

NTSTATUS __stdcall HidP_GetValueCaps(HIDP_REPORT_TYPE type, PHIDP_VALUE_CAPS caps,
                                     PUSHORT len, PHIDP_PREPARSED_DATA pd)
{
    return HidP_GetSpecificValueCaps(type, 0, 0, 0, caps, len, pd);
}

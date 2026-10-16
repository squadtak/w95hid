/*
 * mouhid.c - MOUHID.VXD for Windows 95 OSR2.1: the mouse client of HIDCLASS
 *
 *   1. On load (INIT) and on every PNP_NEW_DEVNODE, after a short delay, a work
 *      item looks for \DosDevices\HID0..HID63 (the collections HIDCLASS made).
 *   2. A collection whose top-level usage is Generic Desktop / Keyboard is kept
 *      open; its preparsed data (IOCTL_HID_GET_COLLECTION_DESCRIPTOR) is what
 *      HidP_GetUsages needs.
 *   3. One asynchronous read is outstanding per mouse.  The completion (APC)
 *      turns the report into key makes and breaks, sends them to VKD as set-1
 *      scan codes and queues a work item that issues the next read.
 *   4. A USB mouse does not auto-repeat: a timer repeats the last pressed key.
 *
 * NTKERN calls (NtKern*) are made from work items only; the APC and the timer
 * callback do memory work and VKD calls, which are safe at any time.
 */
#include <stdarg.h>
#include "../hidparse/hpvxd.h"
#include "hidusage.h"
#include "hidpi.h"

typedef unsigned long   DWORD;
typedef DWORD           HANDLE;

/* ---- NT-ish structures used with NtKern* ---------------------------- */
typedef struct { long Status; DWORD Information; } IOSB;
typedef struct { USHORT Length, MaximumLength; USHORT *Buffer; } USTR;
typedef struct {
    DWORD Length; HANDLE RootDirectory; USTR *ObjectName; DWORD Attributes;
    PVOID SecurityDescriptor; PVOID SecurityQos;
} OBJATTR;
typedef struct { PVOID l0, l1; PVOID Routine; PVOID Param; } WORKITEM;   /* 16 bytes */
typedef void (__stdcall *APCROUTINE)(PVOID ctx, IOSB *iosb, DWORD reserved);

/* ---- services provided by kbdhid.asm -------------------------------- */
extern long  __stdcall NtKernCreateFile(HANDLE *h, DWORD access, OBJATTR *oa, IOSB *iosb,
                                        PVOID alloc, DWORD attr, DWORD share, DWORD disp,
                                        DWORD opts, PVOID ea, DWORD ealen);
extern long  __stdcall NtKernClose(HANDLE h);
extern long  __stdcall NtKernReadFile(HANDLE h, PVOID event, APCROUTINE apc, PVOID ctx,
                                      IOSB *iosb, PVOID buf, DWORD len, PVOID off, PVOID key);
extern long  __stdcall NtKernDeviceIoControl(HANDLE h, PVOID event, PVOID apc, PVOID ctx,
                                             IOSB *iosb, DWORD code, PVOID in, DWORD inlen,
                                             PVOID out, DWORD outlen);
extern long  __stdcall NtKernQueueWorkItem(WORKITEM *wi, DWORD type);
extern PVOID VmmHeapAlloc(DWORD size, DWORD flags);          /* cdecl */
extern DWORD VmmHeapFree(PVOID p, DWORD flags);              /* cdecl */
extern DWORD __stdcall SetTimer(DWORD ms, DWORD ref);
extern void  __stdcall CancelTimer(DWORD handle);
extern DWORD __stdcall IrqSave(void);
extern void  __stdcall IrqRestore(DWORD flags);
extern void  __stdcall PostPointer(DWORD buttons, DWORD dx, DWORD dy, DWORD wheel);
extern DWORD __stdcall GetSysTime(void);                     /* ms since Windows started */
extern DWORD __stdcall HidExtReady(void);                    /* HIDEXT.VXD saw KERNEL32_INITIALIZED */

#pragma code_seg("_LTEXT", "LCODE")
#pragma data_seg("_LDATA", "LCODE")

#define TRC_VXD
#include "..\hidtrace.h"        /* COM1 breadcrumbs for the shutdown hang */

#define HEAPZEROINIT        1
#define IOCTL_GET_COLLECTION_INFORMATION    0x000B01A8UL    /* HID_BUFFER_CTL_CODE(106) */
#define IOCTL_GET_COLLECTION_DESCRIPTOR     0x000B0193UL    /* HID_CTL_CODE(100)        */
#define IOCTL_HIDCLS_LOG                    0x00222400UL    /* CTL_CODE(0x22, 0x900, BUFFERED) */

#define MAX_HID             64
#define MAXKEYS             16
#define TREF_SCAN           1


#define ACT_REISSUE         1
#define ACT_CLOSE           2

typedef struct _KBD {
    struct _KBD *Next;
    DWORD    Serial;
    HANDLE   h;
    IOSB     iosb;                      /* in use while a read is down */
    DWORD    off[2];                    /* LARGE_INTEGER offset for the read */
    PVOID    ppd;                       /* preparsed data (HidP_*) */
    DWORD    inLen;                     /* InputReportByteLength */
    UCHAR    buf[72];                   /* NT convention: byte 0 is the report id */
    DWORD    prevButtons;
    WORKITEM wi;
    DWORD    wiQueued;
    DWORD    action;
    DWORD    closing;
} MOU;

static MOU      *g_List;
static DWORD     g_Busy;                /* outstanding reads, work items, timers */
static DWORD     g_Exiting;
/*
 * VMOUSE is not ready for pointer messages early in the boot: with the USB mouse
 * attached at power-on and moved, VMD_Post_Pointer_Message ended in a divide
 * error inside VMOUSE ("exception 00 ... in vxd vmouse").  Reports are dropped
 * until KERNEL32_INITIALIZED (0x31) has been seen.  This VxD is dynamic: after a
 * hot plug it is loaded again and never sees that message, so the static
 * HIDEXT.VXD keeps the fact for it (HidExtReady).  Without HIDEXT, uptime
 * decides.
 */
static DWORD     g_Ready;
#define VMM_KERNEL32_INITIALIZED    0x31
#define VMM_SYS_DYNAMIC_DEVICE_INIT 0x1B
#define READY_UPTIME_MS             90000UL
static WORKITEM  g_ScanItem;
static DWORD     g_ScanQueued;
static DWORD     g_ScanRetries;
static DWORD     g_ScanTimer;
static DWORD     g_LogHandle;           /* any open collection: where logs go */

/* ------------------------------------------------------------------ */
/* helpers (no CRT)                                                    */
/* ------------------------------------------------------------------ */

/* the compiler must not turn these loops into calls to themselves
   (#pragma function needs a prior declaration) */
void *memset(void *d, int c, unsigned int n);
void *memcpy(void *d, const void *s, unsigned int n);
#pragma function(memset, memcpy)

void *memset(void *d, int c, unsigned int n)
{
    unsigned char *p = (unsigned char *)d;
    while (n--) *p++ = (unsigned char)c;
    return d;
}

void *memcpy(void *d, const void *s, unsigned int n)
{
    unsigned char *dd = (unsigned char *)d;
    const unsigned char *ss = (const unsigned char *)s;
    while (n--) *dd++ = *ss++;
    return d;
}

void KbdCopy(void *d, const void *s, unsigned long n)  { memcpy(d, s, (unsigned int)n); }
void KbdZero(void *d, unsigned long n)                 { memset(d, 0, (unsigned int)n); }
void *KbdAlloc(unsigned long n)                        { return VmmHeapAlloc(n, HEAPZEROINIT); }
void KbdFree(void *p)                                  { if (p) VmmHeapFree(p, 0); }

/* tiny printf: %x %d %s %c %% (with optional 0 and width for %x) */
static void Format(char *out, int max, const char *fmt, va_list ap)
{
    int n = 0;
#define PUT(_c_) do { if (n < max - 1) out[n++] = (char)(_c_); } while (0)
    for (; *fmt; fmt++) {
        int width = 0, zero = 0;
        if (*fmt != '%') { PUT(*fmt); continue; }
        fmt++;
        if (*fmt == '0') { zero = 1; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        switch (*fmt) {
        case 'x': case 'd': {
            unsigned long v = va_arg(ap, unsigned long);
            unsigned long base = (*fmt == 'x') ? 16 : 10;
            char tmp[12];
            int t = 0;
            if (*fmt == 'd' && (long)v < 0) { PUT('-'); v = (unsigned long)(-(long)v); }
            if (v == 0) tmp[t++] = '0';
            while (v) { tmp[t++] = "0123456789abcdef"[v % base]; v /= base; }
            while (width-- > t) PUT(zero ? '0' : ' ');
            while (t) PUT(tmp[--t]);
            break; }
        case 's': { const char *s = va_arg(ap, const char *); while (s && *s) PUT(*s++); break; }
        case 'c': PUT(va_arg(ap, int)); break;
        case '%': PUT('%'); break;
        case 0: fmt--; break;
        default: PUT('%'); PUT(*fmt); break;
        }
    }
    out[n] = 0;
#undef PUT
}

/* written through the collection device: HIDCLASS prints it to DebugView */
static void LogTo(HANDLE h, const char *fmt, ...)
{
    char line[120];
    IOSB iosb;
    DWORD len = 0;
    va_list ap;

    if (!h)
        return;
    va_start(ap, fmt);
    Format(line, sizeof(line), fmt, ap);
    va_end(ap);
    while (line[len]) len++;
    NtKernDeviceIoControl(h, 0, 0, 0, &iosb, IOCTL_HIDCLS_LOG, line, len, 0, 0);
}

#define LOG  LogTo

/* ------------------------------------------------------------------ */
/* report -> pointer message                                           */
/* ------------------------------------------------------------------ */
static void Process(MOU *k)
{
    USAGE cur[8];
    ULONG n = 8, i;
    LONG dx = 0, dy = 0, wheel = 0;
    UCHAR b = 0;
    NTSTATUS st;

    st = HidP_GetUsages(HidP_Input, HID_USAGE_PAGE_BUTTON, 0, cur, &n, (PHIDP_PREPARSED_DATA)k->ppd,
                        (PCHAR)k->buf, k->inLen);
    if (st == HIDP_STATUS_BUFFER_TOO_SMALL)
        n = 8;                                  /* 8+ buttons: the first ones are enough */
    else if (!NT_SUCCESS(st))
        n = 0;
    for (i = 0; i < n && i < 8; i++) {
        if (cur[i] == 1) b |= 0x20;             /* left */
        else if (cur[i] == 2) b |= 0x10;        /* right (bit4: b3 in the "b1 b3 b2 b4" byte) */
        else if (cur[i] == 3) b |= 0x08;        /* middle (bit3) */
        else if (cur[i] == 4) b |= 0x04;        /* fourth */
    }
    if (HidP_GetScaledUsageValue(HidP_Input, HID_USAGE_PAGE_GENERIC, 0, HID_USAGE_GENERIC_X, &dx,
                                 (PHIDP_PREPARSED_DATA)k->ppd, (PCHAR)k->buf, k->inLen) != HIDP_STATUS_SUCCESS)
        dx = 0;
    if (HidP_GetScaledUsageValue(HidP_Input, HID_USAGE_PAGE_GENERIC, 0, HID_USAGE_GENERIC_Y, &dy,
                                 (PHIDP_PREPARSED_DATA)k->ppd, (PCHAR)k->buf, k->inLen) != HIDP_STATUS_SUCCESS)
        dy = 0;
    /* the wheel (Generic Desktop / Wheel).  It only does something with a wheel-aware VMOUSE
     * (Microsoft IntelliPoint).  What goes into ECX is the raw count, as a PS/2 IntelliMouse
     * packet has it, a signed 4-bit value (-8..7, up = positive): the IntelliPoint mini driver
     * (MSMOUSE.VXD) hooks the pointer message, takes ECX as such a nibble (bit 3 = sign,
     * the upper bits are overwritten) and scales it to WHEEL_DELTA (120) units itself.
     * (Passing 120 made +120 and -120 the same nibble: both directions scrolled down.) */
    if (HidP_GetScaledUsageValue(HidP_Input, HID_USAGE_PAGE_GENERIC, 0, HID_USAGE_GENERIC_WHEEL, &wheel,
                                 (PHIDP_PREPARSED_DATA)k->ppd, (PCHAR)k->buf, k->inLen) != HIDP_STATUS_SUCCESS)
        wheel = 0;
    if (wheel) {
        static int logged;
        if (logged < 16) {                      /* the first few wheel events, for DebugView */
            logged++;
            LOG(g_LogHandle, "MOUHID: wheel %d notch(es) -> VMOUSE ECX %d", (int)wheel,
                (int)(wheel > 7 ? 7 : wheel < -7 ? -7 : wheel));
        }
    }
    if (wheel > 7)
        wheel = 7;
    else if (wheel < -7)
        wheel = -7;
    if (!g_Ready) {
        if (!HidExtReady() && GetSysTime() < READY_UPTIME_MS)
            return;                             /* VMOUSE is still starting: drop it */
        g_Ready = 1;
    }
    if (dx || dy || wheel || b != k->prevButtons) {
        k->prevButtons = b;
        PostPointer(b, (DWORD)dx, (DWORD)dy, (DWORD)wheel);
    }
}

/* ------------------------------------------------------------------ */
/* work items and the read loop                                        */
/* ------------------------------------------------------------------ */
static void __stdcall ReadApc(PVOID ctx, IOSB *iosb, DWORD reserved);
static void __stdcall MouWork(PVOID ctx);
static void __stdcall ScanWork(PVOID ctx);

static void Busy(int d)
{
    DWORD fl = IrqSave();
    g_Busy += d;
    IrqRestore(fl);
}

static void QueueAction(MOU *k, DWORD act)
{
    DWORD fl = IrqSave();
    int queue = 0;
    if (act == ACT_CLOSE || !k->action)
        k->action = act;
    if (!k->wiQueued) {
        k->wiQueued = 1;
        queue = 1;
        g_Busy++;
    }
    IrqRestore(fl);
    if (queue) {
        memset(&k->wi, 0, sizeof(k->wi));
        k->wi.Routine = (PVOID)MouWork;
        k->wi.Param = k;
        NtKernQueueWorkItem(&k->wi, 0);
    }
}

static void IssueRead(MOU *k)
{
    long st;

    k->off[0] = k->off[1] = 0;
    Busy(1);
    st = NtKernReadFile(k->h, 0, ReadApc, k, &k->iosb, k->buf, k->inLen, k->off, 0);
    if (st < 0) {
        LOG(g_LogHandle, "MOUHID: ReadFile failed %x", (DWORD)st);
        Busy(-1);
        k->closing = 1;
        QueueAction(k, ACT_CLOSE);
    }
}

/* the read finished (or failed): runs in whatever context NTKERN delivers it */
static void __stdcall ReadApc(PVOID ctx, IOSB *iosb, DWORD reserved)
{
    MOU *k = (MOU *)ctx;

    (void)reserved;
    Busy(-1);
    if (iosb->Status < 0 || g_Exiting || k->closing) {
        k->closing = 1;
        QueueAction(k, ACT_CLOSE);
        return;
    }
    Process(k);
    QueueAction(k, ACT_REISSUE);
}

static void CloseMou(MOU *k)
{
    MOU **pp;
    HANDLE h = k->h;

    for (pp = &g_List; *pp; pp = &(*pp)->Next)
        if (*pp == k) { *pp = k->Next; break; }
    if (g_LogHandle == h)
        g_LogHandle = g_List ? g_List->h : 0;
    LOG(h ? h : g_LogHandle, "MOUHID: mouse \\\\.\\HID%d closed", k->Serial);
    if (h)
        NtKernClose(h);
    KbdFree(k->ppd);
    KbdFree(k);
}

static void __stdcall MouWork(PVOID ctx)
{
    MOU *k = (MOU *)ctx;
    DWORD act;

    {
        DWORD fl = IrqSave();
        act = k->action;
        k->action = 0;
        k->wiQueued = 0;
        g_Busy--;
        IrqRestore(fl);
    }
    if (act == ACT_CLOSE || k->closing)
        CloseMou(k);
    else if (act == ACT_REISSUE && !g_Exiting)
        IssueRead(k);
}

/* ------------------------------------------------------------------ */
/* finding mouses                                                   */
/* ------------------------------------------------------------------ */
static MOU *FindMou(DWORD serial)
{
    MOU *k;
    for (k = g_List; k; k = k->Next)
        if (k->Serial == serial)
            return k;
    return 0;
}

static void TryOpen(DWORD n)
{
    static const char prefix[] = "\\DosDevices\\HID";
    USHORT name[40];
    USTR us;
    OBJATTR oa;
    IOSB iosb;
    HANDLE h = 0;
    DWORD len = 0, i;
    UCHAR ci[12];
    DWORD ppdLen;
    PVOID ppd;
    HIDP_CAPS caps;
    MOU *k;

    for (i = 0; prefix[i]; i++) name[len++] = (USHORT)prefix[i];
    if (n >= 10) name[len++] = (USHORT)('0' + n / 10);
    name[len++] = (USHORT)('0' + n % 10);
    name[len] = 0;
    us.Length = (USHORT)(len * 2);
    us.MaximumLength = (USHORT)(len * 2 + 2);
    us.Buffer = name;
    oa.Length = sizeof(oa);
    oa.RootDirectory = 0;
    oa.ObjectName = &us;
    oa.Attributes = 0x40;                       /* OBJ_CASE_INSENSITIVE */
    oa.SecurityDescriptor = 0;
    oa.SecurityQos = 0;

    if (NtKernCreateFile(&h, 0x80100080UL, &oa, &iosb, 0, 0x80, 3, 1, 0, 0, 0) < 0 || !h)
        return;

    memset(ci, 0, sizeof(ci));
    if (NtKernDeviceIoControl(h, 0, 0, 0, &iosb, IOCTL_GET_COLLECTION_INFORMATION,
                              0, 0, ci, sizeof(ci)) < 0)
        goto reject;
    ppdLen = *(DWORD *)ci;
    if (ppdLen < 16 || ppdLen > 8192)
        goto reject;
    ppd = KbdAlloc(ppdLen);
    if (!ppd)
        goto reject;
    if (NtKernDeviceIoControl(h, 0, 0, 0, &iosb, IOCTL_GET_COLLECTION_DESCRIPTOR,
                              0, 0, ppd, ppdLen) < 0 ||
        HidP_GetCaps((PHIDP_PREPARSED_DATA)ppd, &caps) != HIDP_STATUS_SUCCESS ||
        caps.UsagePage != HID_USAGE_PAGE_GENERIC || caps.Usage != HID_USAGE_GENERIC_MOUSE ||
        caps.InputReportByteLength == 0 || caps.InputReportByteLength > 72) {
        KbdFree(ppd);
        goto reject;
    }

    k = (MOU *)KbdAlloc(sizeof(MOU));
    if (!k) {
        KbdFree(ppd);
        goto reject;
    }
    k->Serial = n;
    k->h = h;
    k->ppd = ppd;
    k->inLen = caps.InputReportByteLength;
    k->Next = g_List;
    g_List = k;
    g_LogHandle = h;
    LOG(h, "MOUHID: mouse \\\\.\\HID%d opened, input report %d bytes", n, k->inLen);
    {
        HIDP_VALUE_CAPS vc;
        USHORT nvc = 1;

        if (HidP_GetSpecificValueCaps(HidP_Input, HID_USAGE_PAGE_GENERIC, 0, HID_USAGE_GENERIC_WHEEL,
                                      &vc, &nvc, (PHIDP_PREPARSED_DATA)ppd) == HIDP_STATUS_SUCCESS && nvc)
            LOG(h, "MOUHID: wheel found: report id %d, %d bit, logical %d..%d", (int)vc.ReportID,
                (int)vc.BitSize, (int)vc.LogicalMin, (int)vc.LogicalMax);
        else
            LOG(h, "MOUHID: no wheel in this mouse's reports");
    }
    IssueRead(k);
    return;

reject:
    NtKernClose(h);
}

static void ArmScan(DWORD ms)
{
    DWORD fl = IrqSave();
    if (!g_ScanTimer && !g_Exiting) {
        g_ScanTimer = SetTimer(ms, TREF_SCAN);
        g_Busy++;
    }
    IrqRestore(fl);
}

static void __stdcall ScanWork(PVOID ctx)
{
    DWORD n, found = 0;

    (void)ctx;
    {
        DWORD fl = IrqSave();
        g_ScanQueued = 0;
        g_Busy--;
        IrqRestore(fl);
    }
    if (g_Exiting)
        return;
    for (n = 0; n < MAX_HID; n++)
        if (!FindMou(n)) {
            TryOpen(n);
            if (FindMou(n))
                found++;
        }
    if (!found && g_ScanRetries) {              /* the node may still be starting */
        g_ScanRetries--;
        ArmScan(500);
    }
}

/* ------------------------------------------------------------------ */
/* entry points called from kbdhid.asm                                 */
/* ------------------------------------------------------------------ */
void __stdcall MouTimer(DWORD ref)
{
    if (ref == TREF_SCAN) {
        int queue = 0;
        DWORD fl = IrqSave();
        g_ScanTimer = 0;
        g_Busy--;
        if (!g_ScanQueued && !g_Exiting) {
            g_ScanQueued = 1;
            queue = 1;
            g_Busy++;
        }
        IrqRestore(fl);
        if (queue) {
            memset(&g_ScanItem, 0, sizeof(g_ScanItem));
            g_ScanItem.Routine = (PVOID)ScanWork;
            NtKernQueueWorkItem(&g_ScanItem, 0);
        }
    }
}

/* every control message the VMM sends to this VxD (mouhid.asm calls this first) */
/* (the shutdown notice to HIDCLASS is HIDEXT.VXD's job) */
void __stdcall MouCtl(DWORD msg)
{
    TRC("M.ctl", msg);
    if (msg == VMM_KERNEL32_INITIALIZED) {
        g_Ready = 1;
    } else if (msg == VMM_SYS_DYNAMIC_DEVICE_INIT &&
               (HidExtReady() || GetSysTime() >= READY_UPTIME_MS)) {
        g_Ready = 1;                            /* loaded after the boot */
    }
}

long __stdcall MouInit(void)
{
    g_Exiting = 0;
    g_ScanRetries = 6;
    ArmScan(300);
    return 0;
}

long __stdcall MouNewDevnode(void)
{
    g_Exiting = 0;                              /* an earlier EXIT may have been refused */
    g_ScanRetries = 6;
    ArmScan(300);
    return 0;
}

/* returns 1 while there is still work outstanding: do not unload yet */
long __stdcall MouExit(void)
{
    MOU *k;

    g_Exiting = 1;
    if (g_ScanTimer) {
        CancelTimer(g_ScanTimer);
        g_ScanTimer = 0;
        g_Busy--;
    }
    for (k = g_List; k; k = k->Next) {
        k->closing = 1;
        if (k->h) {
            HANDLE h = k->h;
            k->h = 0;                           /* closing cancels the read; its APC frees k */
            TRC("M.exit close", h);
            NtKernClose(h);
            TRC("M.exit close done", 0);
        }
    }
    TRC("M.exit end", g_Busy);
    return (g_Busy || g_List) ? 1 : 0;
}

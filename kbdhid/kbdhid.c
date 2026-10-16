/*
 * kbdhid.c - KBDHID.VXD for Windows 95 OSR2.1: the keyboard client of HIDCLASS
 *
 *   1. On load (INIT) and on every PNP_NEW_DEVNODE, after a short delay, a work
 *      item looks for \DosDevices\HID0..HID63 (the collections HIDCLASS made).
 *   2. A collection whose top-level usage is Generic Desktop / Keyboard is kept
 *      open; its preparsed data (IOCTL_HID_GET_COLLECTION_DESCRIPTOR) is what
 *      HidP_GetUsages needs.
 *   3. One asynchronous read is outstanding per keyboard.  The completion (APC)
 *      turns the report into key makes and breaks, sends them to VKD as set-1
 *      scan codes and queues a work item that issues the next read.
 *   4. A USB keyboard does not auto-repeat: a timer repeats the last pressed key.
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
extern DWORD __stdcall SendScanByte(DWORD code);
extern DWORD __stdcall GetLeds(void);
extern long  __stdcall NtKernWriteFile(HANDLE h, PVOID event, PVOID apc, PVOID ctx, IOSB *iosb,
                                       PVOID buf, DWORD len, PVOID off, PVOID key);

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
#define TREF_REPEAT         2
#define TREF_LED            3
#define LED_POLL_MS         150
#define KIND_KEYBOARD       0
#define KIND_CONSUMER       1

#define REPEAT_DELAY_MS     500
#define REPEAT_RATE_MS      60

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
    USHORT   prev[MAXKEYS];             /* usages down in the last report */
    DWORD    nprev;
    WORKITEM wi;
    DWORD    wiQueued;
    DWORD    action;
    DWORD    closing;
    DWORD    kind;                      /* KIND_* */
    DWORD    outLen;                    /* OutputReportByteLength (keyboard LEDs), 0 = none */
    UCHAR    ledId;                     /* report id of the LED output report, 0 = none */
    DWORD    ledSent;                   /* LED bits last written, 0xFF = never */
    DWORD    ledBusy;                   /* an LED work item is queued or running */
    WORKITEM wiLed;
} KBD;

static KBD      *g_List;
static DWORD     g_Busy;                /* outstanding reads, work items, timers */
static DWORD     g_Exiting;
static WORKITEM  g_ScanItem;
static DWORD     g_ScanQueued;
static DWORD     g_ScanRetries;
static DWORD     g_ScanTimer;
static DWORD     g_RepTimer;            /* VMM time-out handle, 0 = none */
static DWORD     g_LedTimer;            /* LED poll time-out handle, 0 = none */
static USHORT    g_RepUsage;            /* key being repeated, 0 = none */
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
/* HID usage -> PC set 1 scan code                                     */
/* ------------------------------------------------------------------ */
#define SC_PRINT    0xF1        /* Print Screen: E0 2A E0 37 / E0 B7 E0 AA */
#define SC_PAUSE    0xF2        /* Pause: E1 1D 45 E1 9D C5, no break      */

typedef struct { UCHAR usage; UCHAR prefix; UCHAR code; } MAP;

static const MAP g_Map[] = {
    {0x04,0,0x1E},{0x05,0,0x30},{0x06,0,0x2E},{0x07,0,0x20},{0x08,0,0x12},{0x09,0,0x21},
    {0x0A,0,0x22},{0x0B,0,0x23},{0x0C,0,0x17},{0x0D,0,0x24},{0x0E,0,0x25},{0x0F,0,0x26},
    {0x10,0,0x32},{0x11,0,0x31},{0x12,0,0x18},{0x13,0,0x19},{0x14,0,0x10},{0x15,0,0x13},
    {0x16,0,0x1F},{0x17,0,0x14},{0x18,0,0x16},{0x19,0,0x2F},{0x1A,0,0x11},{0x1B,0,0x2D},
    {0x1C,0,0x15},{0x1D,0,0x2C},                                    /* A..Z */
    {0x1E,0,0x02},{0x1F,0,0x03},{0x20,0,0x04},{0x21,0,0x05},{0x22,0,0x06},{0x23,0,0x07},
    {0x24,0,0x08},{0x25,0,0x09},{0x26,0,0x0A},{0x27,0,0x0B},        /* 1..9 0 */
    {0x28,0,0x1C},{0x29,0,0x01},{0x2A,0,0x0E},{0x2B,0,0x0F},{0x2C,0,0x39},
    {0x2D,0,0x0C},{0x2E,0,0x0D},{0x2F,0,0x1A},{0x30,0,0x1B},{0x31,0,0x2B},
    {0x32,0,0x2B},{0x33,0,0x27},{0x34,0,0x28},{0x35,0,0x29},{0x36,0,0x33},
    {0x37,0,0x34},{0x38,0,0x35},{0x39,0,0x3A},
    {0x3A,0,0x3B},{0x3B,0,0x3C},{0x3C,0,0x3D},{0x3D,0,0x3E},{0x3E,0,0x3F},{0x3F,0,0x40},
    {0x40,0,0x41},{0x41,0,0x42},{0x42,0,0x43},{0x43,0,0x44},{0x44,0,0x57},{0x45,0,0x58},
    {0x46,0,SC_PRINT},{0x47,0,0x46},{0x48,0,SC_PAUSE},
    {0x49,0xE0,0x52},{0x4A,0xE0,0x47},{0x4B,0xE0,0x49},{0x4C,0xE0,0x53},{0x4D,0xE0,0x4F},
    {0x4E,0xE0,0x51},{0x4F,0xE0,0x4D},{0x50,0xE0,0x4B},{0x51,0xE0,0x50},{0x52,0xE0,0x48},
    {0x53,0,0x45},{0x54,0xE0,0x35},{0x55,0,0x37},{0x56,0,0x4A},{0x57,0,0x4E},{0x58,0xE0,0x1C},
    {0x59,0,0x4F},{0x5A,0,0x50},{0x5B,0,0x51},{0x5C,0,0x4B},{0x5D,0,0x4C},{0x5E,0,0x4D},
    {0x5F,0,0x47},{0x60,0,0x48},{0x61,0,0x49},{0x62,0,0x52},{0x63,0,0x53},
    {0x64,0,0x56},{0x65,0xE0,0x5D},{0x66,0xE0,0x5E},{0x67,0,0x59},
    {0x68,0,0x64},{0x69,0,0x65},{0x6A,0,0x66},{0x6B,0,0x67},{0x6C,0,0x68},{0x6D,0,0x69},
    {0x6E,0,0x6A},{0x6F,0,0x6B},{0x70,0,0x6C},{0x71,0,0x6D},{0x72,0,0x6E},{0x73,0,0x76},
    {0x87,0,0x73},{0x88,0,0x70},{0x89,0,0x7D},{0x8A,0,0x79},{0x8B,0,0x7B},   /* Japanese */
    {0xE0,0,0x1D},{0xE1,0,0x2A},{0xE2,0,0x38},{0xE3,0xE0,0x5B},
    {0xE4,0xE0,0x1D},{0xE5,0,0x36},{0xE6,0xE0,0x38},{0xE7,0xE0,0x5C}
};

static const MAP *LookupMap(USHORT usage)
{
    DWORD i;
    for (i = 0; i < sizeof(g_Map) / sizeof(g_Map[0]); i++)
        if (g_Map[i].usage == usage)
            return &g_Map[i];
    return 0;
}

static void SendBytes(const UCHAR *b, int n)
{
    while (n--)
        SendScanByte(*b++);
}

static void KeyMake(USHORT usage)
{
    const MAP *m = LookupMap(usage);
    if (!m)
        return;
    if (m->code == SC_PRINT) { static const UCHAR s[] = {0xE0,0x2A,0xE0,0x37}; SendBytes(s, 4); return; }
    if (m->code == SC_PAUSE) { static const UCHAR s[] = {0xE1,0x1D,0x45,0xE1,0x9D,0xC5}; SendBytes(s, 6); return; }
    if (m->prefix)
        SendScanByte(m->prefix);
    SendScanByte(m->code);
}

static void KeyBreak(USHORT usage)
{
    const MAP *m = LookupMap(usage);
    if (!m || m->code == SC_PAUSE)
        return;
    if (m->code == SC_PRINT) { static const UCHAR s[] = {0xE0,0xB7,0xE0,0xAA}; SendBytes(s, 4); return; }
    if (m->prefix)
        SendScanByte(m->prefix);
    SendScanByte(m->code | 0x80);
}

/* Consumer Control usages (Fn media keys) -> the E0-prefixed set-1 codes of
 * the Microsoft Natural/Multimedia keyboards.  Windows 95 itself ignores most
 * of them; keyboard utilities and the shell extensions of IE4 use them. */
typedef struct { USHORT usage; UCHAR code; } CMAP;

static const CMAP g_CMap[] = {
    {0xE2,0x20},    /* Mute */            {0xEA,0x2E},    /* Volume Down */
    {0xE9,0x30},    /* Volume Up */       {0xCD,0x22},    /* Play/Pause */
    {0xB7,0x24},    /* Stop */            {0xB5,0x19},    /* Next Track */
    {0xB6,0x10},    /* Previous Track */  {0x183,0x6D},   /* Media Select */
    {0x18A,0x6C},   /* Mail */            {0x192,0x21},   /* Calculator */
    {0x194,0x6B},   /* My Computer */     {0x221,0x65},   /* Search */
    {0x223,0x32},   /* Web Home */        {0x224,0x6A},   /* Back */
    {0x225,0x69},   /* Forward */         {0x226,0x68},   /* Stop (browser) */
    {0x227,0x67},   /* Refresh */         {0x22A,0x66}    /* Favorites */
};

static void ConsKey(USHORT usage, int make)
{
    DWORD i;
    for (i = 0; i < sizeof(g_CMap) / sizeof(g_CMap[0]); i++)
        if (g_CMap[i].usage == usage) {
            SendScanByte(0xE0);
            SendScanByte(make ? g_CMap[i].code : (g_CMap[i].code | 0x80));
            return;
        }
}

/* ------------------------------------------------------------------ */
/* typematic repeat                                                    */
/* ------------------------------------------------------------------ */
static void RepeatStop(void)
{
    DWORD fl = IrqSave();
    DWORD t = g_RepTimer;
    g_RepTimer = 0;
    g_RepUsage = 0;
    IrqRestore(fl);
    if (t)
        CancelTimer(t);
}

static void RepeatStart(USHORT usage)
{
    DWORD fl = IrqSave();
    DWORD old = g_RepTimer;
    g_RepUsage = usage;
    g_RepTimer = 0;
    IrqRestore(fl);
    if (old)
        CancelTimer(old);
    fl = IrqSave();
    if (g_RepUsage == usage && !g_Exiting)
        g_RepTimer = SetTimer(REPEAT_DELAY_MS, TREF_REPEAT);
    IrqRestore(fl);
}

/* ------------------------------------------------------------------ */
/* report -> key makes / breaks                                        */
/* ------------------------------------------------------------------ */
static int InList(const USHORT *l, DWORD n, USHORT u)
{
    DWORD i;
    for (i = 0; i < n; i++)
        if (l[i] == u)
            return 1;
    return 0;
}

static void Process(KBD *k)
{
    USAGE cur[MAXKEYS];
    USHORT now[MAXKEYS];
    ULONG n = MAXKEYS, i, nn = 0;
    NTSTATUS st;

    st = HidP_GetUsages(HidP_Input,
                        (USAGE)(k->kind == KIND_CONSUMER ? HID_USAGE_PAGE_CONSUMER : HID_USAGE_PAGE_KEYBOARD),
                        0, cur, &n, (PHIDP_PREPARSED_DATA)k->ppd, (PCHAR)k->buf, k->inLen);
    if (st == HIDP_STATUS_BUFFER_TOO_SMALL)
        return;                                 /* more keys than we track */
    if (!NT_SUCCESS(st) && st != HIDP_STATUS_USAGE_NOT_FOUND)
        return;
    for (i = 0; i < n && nn < MAXKEYS; i++)
        if (cur[i] >= (k->kind == KIND_CONSUMER ? 1 : 4))   /* keyboard 0..3: no event / rollover */
            now[nn++] = cur[i];

    for (i = 0; i < k->nprev; i++)              /* releases first */
        if (!InList(now, nn, k->prev[i])) {
            if (k->kind == KIND_CONSUMER) {
                ConsKey(k->prev[i], 0);
            } else {
                KeyBreak(k->prev[i]);
                if (g_RepUsage == k->prev[i])
                    RepeatStop();
            }
        }
    for (i = 0; i < nn; i++)                    /* then presses */
        if (!InList(k->prev, k->nprev, now[i])) {
            if (k->kind == KIND_CONSUMER) {
                ConsKey(now[i], 1);
            } else {
                KeyMake(now[i]);
                if (now[i] < 0xE0)              /* modifiers do not repeat */
                    RepeatStart(now[i]);
            }
        }
    for (i = 0; i < nn; i++)
        k->prev[i] = now[i];
    k->nprev = nn;
}

/* ------------------------------------------------------------------ */
/* work items and the read loop                                        */
/* ------------------------------------------------------------------ */
static void __stdcall ReadApc(PVOID ctx, IOSB *iosb, DWORD reserved);
static void __stdcall KbdWork(PVOID ctx);
static void __stdcall ScanWork(PVOID ctx);

static void Busy(int d)
{
    DWORD fl = IrqSave();
    g_Busy += d;
    IrqRestore(fl);
}

static void QueueAction(KBD *k, DWORD act)
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
        k->wi.Routine = (PVOID)KbdWork;
        k->wi.Param = k;
        NtKernQueueWorkItem(&k->wi, 0);
    }
}

static void IssueRead(KBD *k)
{
    long st;

    k->off[0] = k->off[1] = 0;
    Busy(1);
    st = NtKernReadFile(k->h, 0, ReadApc, k, &k->iosb, k->buf, k->inLen, k->off, 0);
    if (st < 0) {
        LOG(g_LogHandle, "KBDHID: ReadFile failed %x", (DWORD)st);
        Busy(-1);
        k->closing = 1;
        QueueAction(k, ACT_CLOSE);
    }
}

/* the read finished (or failed): runs in whatever context NTKERN delivers it */
static void __stdcall ReadApc(PVOID ctx, IOSB *iosb, DWORD reserved)
{
    KBD *k = (KBD *)ctx;

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

static void CloseKbd(KBD *k)
{
    KBD **pp;
    DWORD fl;
    HANDLE h = k->h;

    fl = IrqSave();
    for (pp = &g_List; *pp; pp = &(*pp)->Next)
        if (*pp == k) { *pp = k->Next; break; }
    IrqRestore(fl);
    if (g_LogHandle == h)
        g_LogHandle = g_List ? g_List->h : 0;
    RepeatStop();
    LOG(h ? h : g_LogHandle, "KBDHID: keyboard \\\\.\\HID%d closed", k->Serial);
    if (h)
        NtKernClose(h);
    KbdFree(k->ppd);
    KbdFree(k);
}

static void __stdcall KbdWork(PVOID ctx)
{
    KBD *k = (KBD *)ctx;
    DWORD act;

    {
        DWORD fl = IrqSave();
        act = k->action;
        k->action = 0;
        k->wiQueued = 0;
        g_Busy--;
        IrqRestore(fl);
    }
    if ((act == ACT_CLOSE || k->closing) && k->ledBusy)
        QueueAction(k, ACT_CLOSE);              /* an LED write is still using k */
    else if (act == ACT_CLOSE || k->closing)
        CloseKbd(k);
    else if (act == ACT_REISSUE && !g_Exiting)
        IssueRead(k);
}

/* ------------------------------------------------------------------ */
/* keyboard LEDs: poll VKD's toggle state, write an output report      */
/* ------------------------------------------------------------------ */
static void __stdcall LedWork(PVOID ctx)
{
    KBD *k = (KBD *)ctx;
    UCHAR rep[72];
    USAGE u[3];
    ULONG n = 0;
    IOSB iosb;
    DWORD off[2];
    DWORD leds = k->ledSent;
    HANDLE h = k->h;

    if (h && !k->closing && !g_Exiting) {
        long st;

        memset(rep, 0, sizeof(rep));
        rep[0] = k->ledId;
        if (leds & 2) u[n++] = HID_USAGE_LED_NUM_LOCK;
        if (leds & 4) u[n++] = HID_USAGE_LED_CAPS_LOCK;
        if (leds & 1) u[n++] = HID_USAGE_LED_SCROLL_LOCK;
        if (n)
            HidP_SetUsages(HidP_Output, HID_USAGE_PAGE_LED, 0, u, &n, (PHIDP_PREPARSED_DATA)k->ppd,
                           (PCHAR)rep, k->outLen);
        off[0] = off[1] = 0;
        iosb.Status = 0;
        st = NtKernWriteFile(h, 0, 0, 0, &iosb, rep, k->outLen, off, 0);
        if (st < 0)
            LOG(h, "KBDHID: LED write failed %x (leds %x)", (DWORD)st, leds);
    }
    {
        DWORD fl = IrqSave();
        k->ledBusy = 0;
        g_Busy--;
        IrqRestore(fl);
    }
}

static void ArmLed(void)
{
    DWORD fl = IrqSave();
    if (!g_LedTimer && !g_Exiting)
        g_LedTimer = SetTimer(LED_POLL_MS, TREF_LED);
    IrqRestore(fl);
}

static void LedTick(void)
{
    KBD *todo[4];
    DWORD nt = 0, i, cur, fl;
    KBD *k;

    fl = IrqSave();
    g_LedTimer = 0;
    IrqRestore(fl);
    if (g_Exiting)
        return;
    cur = GetLeds();
    fl = IrqSave();
    for (k = g_List; k && nt < 4; k = k->Next)
        if (k->kind == KIND_KEYBOARD && k->outLen && k->h && !k->closing && !k->ledBusy &&
            k->ledSent != cur) {
            k->ledSent = cur;
            k->ledBusy = 1;
            g_Busy++;
            todo[nt++] = k;
        }
    IrqRestore(fl);
    for (i = 0; i < nt; i++) {
        memset(&todo[i]->wiLed, 0, sizeof(todo[i]->wiLed));
        todo[i]->wiLed.Routine = (PVOID)LedWork;
        todo[i]->wiLed.Param = todo[i];
        NtKernQueueWorkItem(&todo[i]->wiLed, 0);
    }
    if (g_List)
        ArmLed();
}

/* ------------------------------------------------------------------ */
/* finding keyboards                                                   */
/* ------------------------------------------------------------------ */
static KBD *FindKbd(DWORD serial)
{
    KBD *k;
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
    KBD *k;

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
        !((caps.UsagePage == HID_USAGE_PAGE_GENERIC && caps.Usage == HID_USAGE_GENERIC_KEYBOARD) ||
          (caps.UsagePage == HID_USAGE_PAGE_CONSUMER && caps.Usage == 1)) ||
        caps.InputReportByteLength == 0 || caps.InputReportByteLength > 72) {
        KbdFree(ppd);
        goto reject;
    }

    k = (KBD *)KbdAlloc(sizeof(KBD));
    if (!k) {
        KbdFree(ppd);
        goto reject;
    }
    k->Serial = n;
    k->h = h;
    k->ppd = ppd;
    k->inLen = caps.InputReportByteLength;
    k->kind = (caps.UsagePage == HID_USAGE_PAGE_CONSUMER) ? KIND_CONSUMER : KIND_KEYBOARD;
    k->ledSent = 0xFF;                          /* never written: the first poll sends it */
    if (k->kind == KIND_KEYBOARD && caps.OutputReportByteLength && caps.OutputReportByteLength <= 72) {
        HIDP_BUTTON_CAPS bc[1];
        USHORT nb = 1;

        k->outLen = caps.OutputReportByteLength;
        if (HidP_GetSpecificButtonCaps(HidP_Output, HID_USAGE_PAGE_LED, 0, 0, bc, &nb,
                                       (PHIDP_PREPARSED_DATA)ppd) == HIDP_STATUS_SUCCESS && nb)
            k->ledId = bc[0].ReportID;
    }
    {
        DWORD fl = IrqSave();
        k->Next = g_List;
        g_List = k;
        IrqRestore(fl);
    }
    g_LogHandle = h;
    LOG(h, "KBDHID: %s \\\\.\\HID%d opened, input report %d bytes, output %d",
        k->kind == KIND_CONSUMER ? "consumer keys" : "keyboard", n, k->inLen, k->outLen);
    IssueRead(k);
    if (k->outLen)
        ArmLed();
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
        if (!FindKbd(n)) {
            TryOpen(n);
            if (FindKbd(n))
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
void __stdcall KbdTimer(DWORD ref)
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
    } else if (ref == TREF_LED) {
        LedTick();
    } else if (ref == TREF_REPEAT) {
        USHORT u;
        DWORD fl = IrqSave();
        u = g_RepUsage;
        g_RepTimer = 0;
        IrqRestore(fl);
        if (u && !g_Exiting) {
            KeyMake(u);
            fl = IrqSave();
            if (g_RepUsage == u)
                g_RepTimer = SetTimer(REPEAT_RATE_MS, TREF_REPEAT);
            IrqRestore(fl);
        }
    }
}

/* every control message the VMM sends to this VxD (kbdhid.asm calls this first) */
/* (the shutdown notice to HIDCLASS is HIDEXT.VXD's job) */
void __stdcall KbdCtl(DWORD msg)
{
    TRC("K.ctl", msg);
}

long __stdcall KbdInit(void)
{
    g_Exiting = 0;
    g_ScanRetries = 6;
    ArmScan(300);
    return 0;
}

long __stdcall KbdNewDevnode(void)
{
    g_Exiting = 0;                              /* an earlier EXIT may have been refused */
    g_ScanRetries = 6;
    ArmScan(300);
    if (g_List)
        ArmLed();
    return 0;
}

/* returns 1 while there is still work outstanding: do not unload yet */
long __stdcall KbdExit(void)
{
    KBD *k;

    TRC("K.exit begin", g_Busy);
    g_Exiting = 1;
    RepeatStop();
    if (g_LedTimer) {
        DWORD t = g_LedTimer;
        g_LedTimer = 0;
        CancelTimer(t);
    }
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
            TRC("K.exit close", h);
            NtKernClose(h);
            TRC("K.exit close done", 0);
        }
    }
    TRC("K.exit end", g_Busy);
    return (g_Busy || g_List) ? 1 : 0;
}

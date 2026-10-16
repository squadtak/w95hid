/*
 * hidtrace.h - breadcrumbs that survive a hang in the shutdown path.
 *
 * DebugView is gone by the time Windows shuts the drivers down, so the trace
 * goes straight to the hardware:
 *
 *   TRC("tag", value)   a text line on COM1:  "tag 0000abcd\r\n"
 *                       (115200 baud, 8N1; COM1 = port 3F8h)
 *   TRCBEEP(n)          n short beeps on the PC speaker (WDM drivers only)
 *
 * Set HID_TRACE to 0 to compile it all away.  Used by HIDCLASS.SYS, HIDUSB.SYS
 * (WDM: HAL port functions) and KBDHID.VXD / MOUHID.VXD (define TRC_VXD before
 * including: inline assembler).  Each module has its own copy of the functions.
 */
#ifndef _HIDTRACE_H
#define _HIDTRACE_H

/* 0 = off (normal builds: every VMM message would cost a COM1 line in the VxDs).
 * Set to 1 and rebuild to follow a hang with a terminal on COM1 (115200 8N1). */
#ifndef HID_TRACE
#define HID_TRACE   0
#endif

/* beep milestones (TRCB) in the removal path: define as 1 in the module, or here.
 * 1 = FdoRemove entered, 2 = children dropped, 3 = StopDevice entered,
 * 4 = polling stopped, 5 = deconfigured, 6 = StopDevice done,
 * 7 = minidriver returned, 8 = FDO deleted. */
#ifndef HID_TRACE_BEEP
#define HID_TRACE_BEEP  0
#endif

#if HID_TRACE

#define TRC_COM1    0x3F8

#ifdef TRC_VXD
static void TrcPort(unsigned long off, unsigned char v)
{
    unsigned long port = TRC_COM1 + off;
    __asm {
        mov     edx, port
        mov     al, v
        out     dx, al
    }
}

static unsigned char TrcIn(unsigned long off)
{
    unsigned long port = TRC_COM1 + off;
    unsigned char r;
    __asm {
        mov     edx, port
        in      al, dx
        mov     r, al
    }
    return r;
}
#else
static void TrcPort(unsigned long off, unsigned char v)
{
    WRITE_PORT_UCHAR((PUCHAR)(TRC_COM1 + off), v);
}

static unsigned char TrcIn(unsigned long off)
{
    return READ_PORT_UCHAR((PUCHAR)(TRC_COM1 + off));
}
#endif

static int g_TrcReady;

static void TrcInit(void)
{
    TrcPort(1, 0x00);               /* no UART interrupts */
    TrcPort(3, 0x80);               /* DLAB on */
    TrcPort(0, 0x01);               /* divisor 1 = 115200 baud */
    TrcPort(1, 0x00);
    TrcPort(3, 0x03);               /* 8N1, DLAB off */
    TrcPort(2, 0xC7);               /* FIFO on and cleared */
    TrcPort(4, 0x03);               /* DTR + RTS */
    g_TrcReady = 1;
}

static void TrcCh(char c)
{
    unsigned long n;

    for (n = 0; n < 20000 && !(TrcIn(5) & 0x20); n++)   /* THR empty */
        ;
    TrcPort(0, (unsigned char)c);
}

static void Trc(const char *tag, unsigned long v)
{
    static const char hex[] = "0123456789abcdef";
    int i;

    if (!g_TrcReady)
        TrcInit();
    while (*tag)
        TrcCh(*tag++);
    TrcCh(' ');
    for (i = 28; i >= 0; i -= 4)
        TrcCh(hex[(v >> i) & 15]);
    TrcCh('\r');
    TrcCh('\n');
}

#define TRC(_tag_, _v_)     Trc((_tag_), (unsigned long)(_v_))

#ifndef TRC_VXD
/* n beeps of 880 Hz, 80 ms with 120 ms gaps; then a longer pause */
static void TrcBeep(unsigned long n)
{
    unsigned long i;
    UCHAR gate;

    for (i = 0; i < n; i++) {
        WRITE_PORT_UCHAR((PUCHAR)0x43, 0xB6);           /* PIT channel 2, square wave */
        WRITE_PORT_UCHAR((PUCHAR)0x42, 0x4C);           /* 1193180 / 880 = 0x054C */
        WRITE_PORT_UCHAR((PUCHAR)0x42, 0x05);
        gate = READ_PORT_UCHAR((PUCHAR)0x61);
        WRITE_PORT_UCHAR((PUCHAR)0x61, (UCHAR)(gate | 3));
        KeStallExecutionProcessor(80000);
        WRITE_PORT_UCHAR((PUCHAR)0x61, (UCHAR)(gate & ~3));
        KeStallExecutionProcessor(120000);
    }
    KeStallExecutionProcessor(400000);
}
#define TRCBEEP(_n_)        TrcBeep((unsigned long)(_n_))
#endif

/* TRCB(n): a beep milestone in the removal path; off unless HID_TRACE_BEEP is 1 */
#if defined(HID_TRACE_BEEP) && HID_TRACE_BEEP && !defined(TRC_VXD)
#define TRCB(_n_)           TrcBeep((unsigned long)(_n_))
#else
#define TRCB(_n_)           ((void)0)
#endif

#else   /* !HID_TRACE */
#define TRC(_tag_, _v_)     ((void)0)
#define TRCBEEP(_n_)        ((void)0)
#define TRCB(_n_)           ((void)0)
#endif

#endif

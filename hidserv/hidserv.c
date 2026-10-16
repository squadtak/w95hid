/*
 * HIDSERV.EXE - HID Audio Control Service for Windows 95 OSR2.1
 *
 * The Win98 HIDSERV.EXE of the same name reads the Consumer Control collection of a
 * HID device (the volume keys of a keyboard, a speaker's knob) and turns it into mixer
 * operations.  This is the same job for the Win95 HID port: no service manager and no
 * device interfaces here, so
 *
 *   - it is a plain program (hidden window, message loop), started from the Run key;
 *   - the devices are found through HID.DLL: HidX_OpenDevice(0..63) = \\.\HID0..63;
 *   - a collection with usage page 0x0C usage 0x01 gets a reader thread;
 *   - Volume Up / Volume Down / Mute change the master volume of the mixer
 *     (waveOutSetVolume when the sound card has no mixer).
 *
 * Assigning the other media keys (play, mail, ...) is not done here: it was the job of
 * the keyboard vendor's own software, which reads the same HID collection.
 *
 * All the work happens in the one main thread; the reader threads only post
 * WM_APP_KEY / WM_APP_GONE to the window.
 */
#include <windows.h>
#include <dbt.h>
#include <mmsystem.h>
#include <string.h>
#include <stdarg.h>
#include "hidsdi.h"

/* an extension of this HID.DLL: \\.\HIDn, INVALID_HANDLE_VALUE if there is none */
extern HANDLE __stdcall HidX_OpenDevice(DWORD index);

#define MAX_DEV             64
#define MAX_USAGES          32

#define PAGE_CONSUMER       0x0C
#define USAGE_CONSUMER_CTL  0x01
#define USAGE_MUTE          0xE2
#define USAGE_VOLUME_UP     0xE9
#define USAGE_VOLUME_DOWN   0xEA

#define VOLUME_STEP         (0xFFFF / 20)       /* 5% */
#define REPEAT_DELAY        400                 /* ms before a held key repeats */
#define REPEAT_RATE         100                 /* ms between repeats */
#define SCAN_PERIOD         10000               /* ms: look for new devices anyway */
#define RESCAN_DELAY        1000                /* ms after a device change */

#define WM_APP_KEY          (WM_APP + 1)        /* wParam = usage, lParam = 1 down / 0 up */
#define WM_APP_GONE         (WM_APP + 2)        /* wParam = device index */

#define TIMER_SCAN          1
#define TIMER_RESCAN        2
#define TIMER_REPEAT_START  3
#define TIMER_REPEAT        4

typedef struct {
    int                     state;              /* 0 none, 1 reader running, 2 not a consumer control */
    HANDLE                  h;
    PHIDP_PREPARSED_DATA    ppd;
    USHORT                  inLen;
} DEV;

static DEV      g_Dev[MAX_DEV];
static HWND     g_Wnd;
static USAGE    g_Held;                         /* the volume key being held */
static int      g_Dir;                          /* +1 / -1 for the held key */
static BOOL     g_Muted;                        /* mute by volume 0 (no mute control) */
static DWORD    g_SavedVol;

/* DebugView */
static void Log(const char *fmt, ...)
{
    char buf[200];
    va_list ap;

    va_start(ap, fmt);
    wvsprintf(buf, fmt, ap);
    va_end(ap);
    lstrcat(buf, "\n");
    OutputDebugString(buf);
}

/* ------------------------------------------------------------------ */
/* reading the collection                                              */
/* ------------------------------------------------------------------ */

static BOOL Has(const USAGE *list, ULONG n, USAGE u)
{
    ULONG i;

    for (i = 0; i < n; i++)
        if (list[i] == u)
            return TRUE;
    return FALSE;
}

static DWORD WINAPI Reader(LPVOID param)
{
    DWORD n = (DWORD)param;
    DEV *d = &g_Dev[n];
    BYTE *rep;
    USAGE prev[MAX_USAGES], cur[MAX_USAGES];
    ULONG nprev = 0, ncur, i;
    DWORD got;

    rep = (BYTE *)LocalAlloc(LPTR, d->inLen + 8);
    if (rep) {
        for (;;) {
            if (!ReadFile(d->h, rep, d->inLen, &got, NULL))
                break;                          /* unplugged, or the handle was closed */
            ncur = MAX_USAGES;
            if (HidP_GetUsages(HidP_Input, PAGE_CONSUMER, 0, cur, &ncur, d->ppd,
                               (PCHAR)rep, got) != HIDP_STATUS_SUCCESS)
                ncur = 0;
            for (i = 0; i < nprev; i++)         /* released */
                if (!Has(cur, ncur, prev[i]))
                    PostMessage(g_Wnd, WM_APP_KEY, prev[i], 0);
            for (i = 0; i < ncur; i++)          /* pressed */
                if (!Has(prev, nprev, cur[i]))
                    PostMessage(g_Wnd, WM_APP_KEY, cur[i], 1);
            memcpy(prev, cur, ncur * sizeof(USAGE));
            nprev = ncur;
        }
        LocalFree(rep);
    }
    for (i = 0; i < nprev; i++)                 /* a key held while the device went away */
        PostMessage(g_Wnd, WM_APP_KEY, prev[i], 0);
    PostMessage(g_Wnd, WM_APP_GONE, n, 0);
    return 0;
}

static void Probe(int n)
{
    DEV *d = &g_Dev[n];
    HANDLE h, t;
    PHIDP_PREPARSED_DATA ppd;
    HIDP_CAPS caps;
    DWORD tid;

    h = HidX_OpenDevice(n);
    if (h == INVALID_HANDLE_VALUE) {
        d->state = 0;
        return;
    }
    if (!HidD_GetPreparsedData(h, &ppd)) {
        CloseHandle(h);
        d->state = 2;
        return;
    }
    if (HidP_GetCaps(ppd, &caps) != HIDP_STATUS_SUCCESS ||
        caps.UsagePage != PAGE_CONSUMER || caps.Usage != USAGE_CONSUMER_CTL ||
        !caps.InputReportByteLength) {
        HidD_FreePreparsedData(ppd);
        CloseHandle(h);
        d->state = 2;
        return;
    }
    d->h = h;
    d->ppd = ppd;
    d->inLen = caps.InputReportByteLength;
    d->state = 1;
    Log("[HIDSERV] \\\\.\\HID%d: consumer control, input report %u bytes", n, d->inLen);
    t = CreateThread(NULL, 0, Reader, (LPVOID)n, 0, &tid);
    if (t) {
        CloseHandle(t);
    } else {
        HidD_FreePreparsedData(ppd);
        CloseHandle(h);
        d->state = 0;
    }
}

/* a device that is not ours is remembered (state 2) until the next device change,
 * so that a game pad is not opened and closed again every few seconds */
static void Rescan(BOOL forget)
{
    int n;

    for (n = 0; n < MAX_DEV; n++) {
        if (g_Dev[n].state == 1)
            continue;
        if (g_Dev[n].state == 2 && !forget)
            continue;
        Probe(n);
    }
}

static void Gone(int n)
{
    DEV *d = &g_Dev[n];

    if (d->state != 1)
        return;
    HidD_FreePreparsedData(d->ppd);
    CloseHandle(d->h);
    d->state = 0;
}

/* ------------------------------------------------------------------ */
/* the master volume                                                   */
/* ------------------------------------------------------------------ */

typedef struct {
    HMIXER          mx;
    DWORD           chans;
    BOOL            haveVol, haveMute;
    MIXERCONTROL    vol, mute;
} MIX;

static BOOL GetControl(MIX *m, DWORD line, DWORD type, MIXERCONTROL *out)
{
    MIXERLINECONTROLS mlc;

    memset(&mlc, 0, sizeof(mlc));
    memset(out, 0, sizeof(*out));
    out->cbStruct = sizeof(*out);
    mlc.cbStruct = sizeof(mlc);
    mlc.dwLineID = line;
    mlc.dwControlType = type;
    mlc.cControls = 1;
    mlc.cbmxctrl = sizeof(MIXERCONTROL);
    mlc.pamxctrl = out;
    return mixerGetLineControls((HMIXEROBJ)m->mx, &mlc,
                                MIXER_GETLINECONTROLSF_ONEBYTYPE | MIXER_OBJECTF_HMIXER)
           == MMSYSERR_NOERROR;
}

static BOOL OpenMixer(MIX *m)
{
    MIXERLINE ml;

    memset(m, 0, sizeof(*m));
    if (mixerOpen(&m->mx, 0, 0, 0, MIXER_OBJECTF_MIXER) != MMSYSERR_NOERROR)
        return FALSE;
    memset(&ml, 0, sizeof(ml));
    ml.cbStruct = sizeof(ml);
    ml.dwComponentType = MIXERLINE_COMPONENTTYPE_DST_SPEAKERS;
    if (mixerGetLineInfo((HMIXEROBJ)m->mx, &ml,
                         MIXER_GETLINEINFOF_COMPONENTTYPE | MIXER_OBJECTF_HMIXER)
        != MMSYSERR_NOERROR) {
        mixerClose(m->mx);
        return FALSE;
    }
    m->chans = ml.cChannels ? ml.cChannels : 1;
    m->haveVol = GetControl(m, ml.dwLineID, MIXERCONTROL_CONTROLTYPE_VOLUME, &m->vol);
    m->haveMute = GetControl(m, ml.dwLineID, MIXERCONTROL_CONTROLTYPE_MUTE, &m->mute);
    if (!m->haveVol && !m->haveMute) {
        mixerClose(m->mx);
        return FALSE;
    }
    return TRUE;
}

static DWORD Channels(const MIX *m, const MIXERCONTROL *c)
{
    DWORD n = (c->fdwControl & MIXERCONTROL_CONTROLF_UNIFORM) ? 1 : m->chans;

    return n > 8 ? 8 : n;
}

static long Clamp(long v, const MIXERCONTROL *c)
{
    if (v < (long)c->Bounds.dwMinimum)
        v = (long)c->Bounds.dwMinimum;
    if (v > (long)c->Bounds.dwMaximum)
        v = (long)c->Bounds.dwMaximum;
    return v;
}

/* the volume as a value every channel is moved by; false if there is no way */
static BOOL MixerStep(MIX *m, int dir)
{
    MIXERCONTROLDETAILS d;
    MIXERCONTROLDETAILS_UNSIGNED v[8];
    DWORD ch, i;

    if (!m->haveVol)
        return FALSE;
    ch = Channels(m, &m->vol);
    memset(&d, 0, sizeof(d));
    d.cbStruct = sizeof(d);
    d.dwControlID = m->vol.dwControlID;
    d.cChannels = ch;
    d.cbDetails = sizeof(v[0]);
    d.paDetails = v;
    if (mixerGetControlDetails((HMIXEROBJ)m->mx, &d,
                               MIXER_GETCONTROLDETAILSF_VALUE | MIXER_OBJECTF_HMIXER)
        != MMSYSERR_NOERROR)
        return FALSE;
    for (i = 0; i < ch; i++)
        v[i].dwValue = (DWORD)Clamp((long)v[i].dwValue + dir * VOLUME_STEP, &m->vol);
    return mixerSetControlDetails((HMIXEROBJ)m->mx, &d,
                                  MIXER_SETCONTROLDETAILSF_VALUE | MIXER_OBJECTF_HMIXER)
           == MMSYSERR_NOERROR;
}

static BOOL MixerMute(MIX *m)
{
    MIXERCONTROLDETAILS d;
    MIXERCONTROLDETAILS_BOOLEAN b[8];
    DWORD ch, i;

    if (!m->haveMute)
        return FALSE;
    ch = Channels(m, &m->mute);
    memset(&d, 0, sizeof(d));
    d.cbStruct = sizeof(d);
    d.dwControlID = m->mute.dwControlID;
    d.cChannels = ch;
    d.cbDetails = sizeof(b[0]);
    d.paDetails = b;
    if (mixerGetControlDetails((HMIXEROBJ)m->mx, &d,
                               MIXER_GETCONTROLDETAILSF_VALUE | MIXER_OBJECTF_HMIXER)
        != MMSYSERR_NOERROR)
        return FALSE;
    for (i = 0; i < ch; i++)
        b[i].fValue = !b[0].fValue;
    return mixerSetControlDetails((HMIXEROBJ)m->mx, &d,
                                  MIXER_SETCONTROLDETAILSF_VALUE | MIXER_OBJECTF_HMIXER)
           == MMSYSERR_NOERROR;
}

/* the wave device's own volume: left in the low word, right in the high word */
static void WaveStep(int dir)
{
    DWORD v;
    long l, r;

    if (waveOutGetVolume((HWAVEOUT)0, &v) != MMSYSERR_NOERROR)
        return;
    l = (long)(v & 0xFFFF) + dir * VOLUME_STEP;
    r = (long)(v >> 16) + dir * VOLUME_STEP;
    l = l < 0 ? 0 : (l > 0xFFFF ? 0xFFFF : l);
    r = r < 0 ? 0 : (r > 0xFFFF ? 0xFFFF : r);
    waveOutSetVolume((HWAVEOUT)0, ((DWORD)r << 16) | (DWORD)l);
}

static void WaveMute(void)
{
    DWORD v;

    if (g_Muted) {
        waveOutSetVolume((HWAVEOUT)0, g_SavedVol);
        g_Muted = FALSE;
    } else if (waveOutGetVolume((HWAVEOUT)0, &v) == MMSYSERR_NOERROR) {
        g_SavedVol = v;
        waveOutSetVolume((HWAVEOUT)0, 0);
        g_Muted = TRUE;
    }
}

static void VolumeStep(int dir)
{
    MIX m;
    BOOL ok = FALSE, opened = OpenMixer(&m);

    if (opened) {
        ok = MixerStep(&m, dir);
        mixerClose(m.mx);
    }
    Log("[HIDSERV] volume %+d: mixer %s, %s", dir, opened ? "opened" : "not available",
        ok ? "set" : "falling back to the wave device");
    if (!ok)
        WaveStep(dir);
}

static void VolumeMute(void)
{
    MIX m;
    BOOL ok = FALSE, opened = OpenMixer(&m);

    if (opened) {
        ok = MixerMute(&m);
        mixerClose(m.mx);
    }
    Log("[HIDSERV] mute: mixer %s, %s", opened ? "opened" : "not available",
        ok ? "set" : "falling back to the wave device");
    if (!ok)
        WaveMute();
}

/* ------------------------------------------------------------------ */
/* the window                                                          */
/* ------------------------------------------------------------------ */

static void StopRepeat(void)
{
    KillTimer(g_Wnd, TIMER_REPEAT_START);
    KillTimer(g_Wnd, TIMER_REPEAT);
    g_Held = 0;
}

static void OnKey(USAGE usage, BOOL down)
{
    switch (usage) {
    case USAGE_VOLUME_UP:
    case USAGE_VOLUME_DOWN:
        if (down) {
            StopRepeat();
            g_Held = usage;
            g_Dir = usage == USAGE_VOLUME_UP ? 1 : -1;
            VolumeStep(g_Dir);
            SetTimer(g_Wnd, TIMER_REPEAT_START, REPEAT_DELAY, NULL);
        } else if (g_Held == usage) {
            StopRepeat();
        }
        break;
    case USAGE_MUTE:
        if (down)
            VolumeMute();
        break;
    }
}

static LRESULT CALLBACK WndProc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_APP_KEY:
        OnKey((USAGE)wp, (BOOL)lp);
        return 0;

    case WM_APP_GONE:
        Gone((int)wp);
        SetTimer(w, TIMER_RESCAN, RESCAN_DELAY, NULL);
        return 0;

    case WM_DEVICECHANGE:                       /* a device came or went: look again soon */
        if (wp == DBT_DEVNODES_CHANGED)
            SetTimer(w, TIMER_RESCAN, RESCAN_DELAY, NULL);
        return TRUE;

    case WM_TIMER:
        switch (wp) {
        case TIMER_SCAN:
            Rescan(FALSE);
            break;
        case TIMER_RESCAN:
            KillTimer(w, TIMER_RESCAN);
            Rescan(TRUE);
            break;
        case TIMER_REPEAT_START:
            KillTimer(w, TIMER_REPEAT_START);
            if (g_Held) {
                VolumeStep(g_Dir);
                SetTimer(w, TIMER_REPEAT, REPEAT_RATE, NULL);
            }
            break;
        case TIMER_REPEAT:
            if (g_Held)
                VolumeStep(g_Dir);
            else
                KillTimer(w, TIMER_REPEAT);
            break;
        }
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(w, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    WNDCLASS wc;
    HANDLE mutex;
    MSG msg;

    (void)prev; (void)show;

    /* a test of the volume code without a device: hidserv up | down | mute */
    if (!lstrcmpi(cmd, "up") || !lstrcmpi(cmd, "down") || !lstrcmpi(cmd, "mute")) {
        if (!lstrcmpi(cmd, "mute"))
            VolumeMute();
        else
            VolumeStep(!lstrcmpi(cmd, "up") ? 1 : -1);
        return 0;
    }

    mutex = CreateMutex(NULL, FALSE, "HidAudioControlService");
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS)
        return 0;                               /* already running */

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = "HidAudioClass";
    if (!RegisterClass(&wc))
        return 1;
    g_Wnd = CreateWindow("HidAudioClass", "HID Audio Control Service", WS_POPUP,
                         0, 0, 0, 0, NULL, NULL, inst, NULL);
    if (!g_Wnd)
        return 1;

    Log("[HIDSERV] started");
    Rescan(TRUE);
    SetTimer(g_Wnd, TIMER_SCAN, SCAN_PERIOD, NULL);
    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return 0;
}

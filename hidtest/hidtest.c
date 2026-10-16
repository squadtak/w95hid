/*
 * hidtest.c - Win32 console test of HID.DLL and HIDCLASS's collection devices (\\.\HIDn)
 *
 *   hidtest                    list the collections: ids, usage, strings
 *   hidtest <n>                show collection n (attributes, strings, capabilities),
 *                              then print every input report, decoded by usage
 *                              (buttons, axes) as it arrives; Ctrl+C to quit
 *   hidtest <n> <count>        stop after <count> reports
 *   hidtest <n> feature <id>   read feature report <id> and print it
 *   hidtest <n> flush          drop the reports waiting in HIDCLASS
 *
 * The HID.DLL of this port is used for everything: HidD_* for the device,
 * HidP_* for the reports.  Reports are in the NT form: byte 0 is the report id.
 *
 * Build (MSVC 5.0, after hiddll):   nmake      (BASEDIR = the 98DDK root)
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hidsdi.h"

#define MAXCAPS  64

/* DeviceIoControl is not needed; the HIDX_ functions are extensions of this HID.DLL */
extern HANDLE __stdcall HidX_OpenDevice(DWORD index);

static void PrintStr(const char *label, HANDLE h,
                     BOOLEAN (__stdcall *fn)(HANDLE, PVOID, ULONG))
{
    WCHAR w[128];
    char a[256];

    w[0] = 0;
    if (fn(h, w, sizeof(w))) {
        a[0] = 0;
        WideCharToMultiByte(CP_ACP, 0, w, -1, a, sizeof(a), NULL, NULL);
        printf("  %s: %s\n", label, a);
    } else {
        printf("  %s: (none, error %lu)\n", label, GetLastError());
    }
}

static void Hex(const char *label, const unsigned char *b, unsigned long n)
{
    unsigned long i;

    printf("%s (%lu bytes):", label, n);
    for (i = 0; i < n; i++)
        printf(" %02x", b[i]);
    printf("\n");
}

/* the capabilities of the collection, for the log and for decoding the reports */
typedef struct {
    HIDP_CAPS           caps;
    HIDP_BUTTON_CAPS    bc[MAXCAPS];
    USHORT              nbc;
    HIDP_VALUE_CAPS     vc[MAXCAPS];
    USHORT              nvc;
} DEVINFO;

static int GetInfo(HANDLE h, PHIDP_PREPARSED_DATA *ppd, DEVINFO *d)
{
    memset(d, 0, sizeof(*d));
    if (!HidD_GetPreparsedData(h, ppd)) {
        printf("HidD_GetPreparsedData failed, error %lu\n", GetLastError());
        return 0;
    }
    if (HidP_GetCaps(*ppd, &d->caps) != HIDP_STATUS_SUCCESS) {
        printf("HidP_GetCaps failed\n");
        return 0;
    }
    d->nbc = MAXCAPS;
    if (HidP_GetSpecificButtonCaps(HidP_Input, 0, 0, 0, d->bc, &d->nbc, *ppd) != HIDP_STATUS_SUCCESS)
        d->nbc = 0;
    d->nvc = MAXCAPS;
    if (HidP_GetSpecificValueCaps(HidP_Input, 0, 0, 0, d->vc, &d->nvc, *ppd) != HIDP_STATUS_SUCCESS)
        d->nvc = 0;
    return 1;
}

static void ShowInfo(const DEVINFO *d)
{
    USHORT i;

    printf("  usage page %04x usage %04x; report lengths input %u output %u feature %u (with the id byte)\n",
           d->caps.UsagePage, d->caps.Usage, d->caps.InputReportByteLength,
           d->caps.OutputReportByteLength, d->caps.FeatureReportByteLength);
    for (i = 0; i < d->nbc; i++) {
        const HIDP_BUTTON_CAPS *b = &d->bc[i];
        if (b->IsRange)
            printf("  buttons: page %04x usages %04x..%04x (report id %u)\n", b->UsagePage,
                   b->Range.UsageMin, b->Range.UsageMax, b->ReportID);
        else
            printf("  button: page %04x usage %04x (report id %u)\n", b->UsagePage,
                   b->NotRange.Usage, b->ReportID);
    }
    for (i = 0; i < d->nvc; i++) {
        const HIDP_VALUE_CAPS *v = &d->vc[i];
        if (v->IsRange)
            printf("  values: page %04x usages %04x..%04x, %u bits x %u, logical %ld..%ld (report id %u)\n",
                   v->UsagePage, v->Range.UsageMin, v->Range.UsageMax, v->BitSize, v->ReportCount,
                   v->LogicalMin, v->LogicalMax, v->ReportID);
        else
            printf("  value: page %04x usage %04x, %u bits x %u, logical %ld..%ld (report id %u)\n",
                   v->UsagePage, v->NotRange.Usage, v->BitSize, v->ReportCount,
                   v->LogicalMin, v->LogicalMax, v->ReportID);
    }
}

/* every input report, decoded: the pressed buttons per page, then the values */
static void Decode(PHIDP_PREPARSED_DATA ppd, const DEVINFO *d, unsigned char *rep, DWORD len)
{
    USHORT i, j, k;
    USAGE pages[MAXCAPS];
    int npages = 0;

    for (i = 0; i < d->nbc; i++) {
        int seen = 0;
        for (j = 0; j < npages; j++)
            if (pages[j] == d->bc[i].UsagePage) seen = 1;
        if (!seen && npages < MAXCAPS)
            pages[npages++] = d->bc[i].UsagePage;
    }
    for (j = 0; j < npages; j++) {
        USAGE list[MAXCAPS];
        ULONG n = MAXCAPS;
        ULONG q;
        NTSTATUS st = HidP_GetUsages(HidP_Input, pages[j], 0, list, &n, ppd, (PCHAR)rep, len);
        if (st == HIDP_STATUS_SUCCESS && n) {
            printf("    page %04x pressed:", pages[j]);
            for (q = 0; q < n; q++)
                printf(" %04x", list[q]);
            printf("\n");
        }
    }
    for (i = 0; i < d->nvc; i++) {
        const HIDP_VALUE_CAPS *v = &d->vc[i];
        USAGE lo = v->IsRange ? v->Range.UsageMin : v->NotRange.Usage;
        USAGE hi = v->IsRange ? v->Range.UsageMax : v->NotRange.Usage;

        for (k = lo; k <= hi; k++) {
            if (v->LogicalMin < 0) {                    /* signed (relative axes): the scaled value */
                LONG sv;
                if (HidP_GetScaledUsageValue(HidP_Input, v->UsagePage, v->LinkCollection, k,
                                             &sv, ppd, (PCHAR)rep, len) == HIDP_STATUS_SUCCESS)
                    printf("    page %04x usage %04x = %ld\n", v->UsagePage, k, sv);
            } else {
                ULONG uv;
                if (HidP_GetUsageValue(HidP_Input, v->UsagePage, v->LinkCollection, k,
                                       &uv, ppd, (PCHAR)rep, len) == HIDP_STATUS_SUCCESS)
                    printf("    page %04x usage %04x = %lu\n", v->UsagePage, k, uv);
            }
            if (k == 0xFFFF)
                break;
        }
    }
}

static void List(void)
{
    int n, found = 0;

    for (n = 0; n < 64; n++) {
        HANDLE h = HidX_OpenDevice(n);
        HIDD_ATTRIBUTES a;
        PHIDP_PREPARSED_DATA ppd;
        DEVINFO d;

        if (h == INVALID_HANDLE_VALUE)
            continue;
        found++;
        a.Size = sizeof(a);
        printf("\\\\.\\HID%d\n", n);
        if (HidD_GetAttributes(h, &a))
            printf("  VID %04x PID %04x version %04x\n", a.VendorID, a.ProductID, a.VersionNumber);
        PrintStr("manufacturer", h, HidD_GetManufacturerString);
        PrintStr("product     ", h, HidD_GetProductString);
        if (GetInfo(h, &ppd, &d)) {
            printf("  usage page %04x usage %04x\n", d.caps.UsagePage, d.caps.Usage);
            HidD_FreePreparsedData(ppd);
        }
        CloseHandle(h);
    }
    if (!found)
        printf("no \\\\.\\HIDn found (error %lu)\n", GetLastError());
}

static DWORD WINAPI DrainConsole(LPVOID unused)
{
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    INPUT_RECORD ir;
    DWORD n;

    (void)unused;
    for (;;)
        if (!ReadConsoleInput(in, &ir, 1, &n))
            break;
    return 0;
}

int main(int argc, char **argv)
{
    int n;
    long limit = -1, count = 0;
    HANDLE h;
    HIDD_ATTRIBUTES a;
    PHIDP_PREPARSED_DATA ppd;
    DEVINFO d;
    unsigned char *rep;
    DWORD got;

    if (argc < 2) {
        List();
        return 0;
    }
    n = atoi(argv[1]);
    h = HidX_OpenDevice(n);
    if (h == INVALID_HANDLE_VALUE) {
        printf("cannot open \\\\.\\HID%d, error %lu\n", n, GetLastError());
        return 1;
    }
    printf("opened \\\\.\\HID%d\n", n);
    a.Size = sizeof(a);
    if (HidD_GetAttributes(h, &a))
        printf("  VID %04x PID %04x version %04x\n", a.VendorID, a.ProductID, a.VersionNumber);
    PrintStr("manufacturer", h, HidD_GetManufacturerString);
    PrintStr("product     ", h, HidD_GetProductString);
    PrintStr("serial      ", h, HidD_GetSerialNumberString);
    if (!GetInfo(h, &ppd, &d)) {
        CloseHandle(h);
        return 1;
    }
    ShowInfo(&d);

    if (argc >= 3 && !strcmp(argv[2], "flush")) {
        printf("HidD_FlushQueue: %s\n", HidD_FlushQueue(h) ? "ok" : "failed");
        HidD_FreePreparsedData(ppd);
        CloseHandle(h);
        return 0;
    }
    if (argc >= 4 && !strcmp(argv[2], "feature")) {
        unsigned char *f = (unsigned char *)calloc(1, d.caps.FeatureReportByteLength + 1);

        if (!d.caps.FeatureReportByteLength) {
            printf("this collection has no feature reports\n");
        } else if (f) {
            f[0] = (unsigned char)atoi(argv[3]);
            if (HidD_GetFeature(h, f, d.caps.FeatureReportByteLength))
                Hex("feature report", f, d.caps.FeatureReportByteLength);
            else
                printf("HidD_GetFeature failed, error %lu\n", GetLastError());
            free(f);
        }
        HidD_FreePreparsedData(ppd);
        CloseHandle(h);
        return 0;
    }
    if (argc >= 3)
        limit = atol(argv[2]);

    rep = (unsigned char *)malloc(d.caps.InputReportByteLength + 8);
    if (!rep) {
        CloseHandle(h);
        return 1;
    }
    printf("waiting for reports (press keys / move the mouse / use the pad)...\n");
    /* the console is the foreground window, so the keys also land in its input
     * buffer; if nobody reads them the DOS box beeps for every key and a held key
     * floods the speaker until the machine looks dead */
    {
        DWORD tid;
        HANDLE t = CreateThread(NULL, 0, DrainConsole, NULL, 0, &tid);
        if (t)
            CloseHandle(t);
    }
    for (;;) {
        if (!ReadFile(h, rep, d.caps.InputReportByteLength, &got, NULL)) {
            printf("ReadFile failed, error %lu\n", GetLastError());
            break;
        }
        Hex("report", rep, got);
        if (got == d.caps.InputReportByteLength)
            Decode(ppd, &d, rep, got);
        if (limit >= 0 && ++count >= limit)
            break;
    }
    free(rep);
    HidD_FreePreparsedData(ppd);
    CloseHandle(h);
    return 0;
}

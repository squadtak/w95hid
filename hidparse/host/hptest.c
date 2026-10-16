/*
 * hptest.c - host unit test for hidparse.c
 *
 * Build:  gcc -m32 -I. -I.. -I<98DDK>/src/hid/inc -o hptest hptest.c ../hidparse.c
 * (see test.sh)  The tests use the real report descriptor read from a ThinkPad
 * USB keyboard (E1 log), a standard boot mouse and a small game pad.
 */
#include <stdio.h>
#include "../hpport.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
#define CHECK_ST(e, c) do { NTSTATUS _s = (c); if (_s != (e)) { printf("FAIL line %d: %s -> %08lx (want %08lx)\n", __LINE__, #c, (ULONG)_s, (ULONG)(e)); fails++; } } while (0)

/* ThinkPad USB keyboard, boot keyboard, 65 bytes, from the E1 log */
static const UCHAR kbd[] = {
    0x05,0x01,0x09,0x06,0xa1,0x01,0x05,0x07,0x19,0xe0,0x29,0xe7,0x15,0x00,0x25,0x01,
    0x75,0x01,0x95,0x08,0x81,0x02,0x95,0x01,0x75,0x08,0x81,0x01,0x95,0x03,0x75,0x01,
    0x05,0x08,0x19,0x01,0x29,0x03,0x91,0x02,0x95,0x01,0x75,0x05,0x91,0x01,0x95,0x06,
    0x75,0x08,0x15,0x00,0x26,0xff,0x00,0x05,0x07,0x19,0x00,0x2a,0xff,0x00,0x81,0x00,
    0xc0
};

/* boot mouse with wheel */
static const UCHAR mouse[] = {
    0x05,0x01,0x09,0x02,0xa1,0x01,0x09,0x01,0xa1,0x00,0x05,0x09,0x19,0x01,0x29,0x03,
    0x15,0x00,0x25,0x01,0x95,0x03,0x75,0x01,0x81,0x02,0x95,0x01,0x75,0x05,0x81,0x03,
    0x05,0x01,0x09,0x30,0x09,0x31,0x09,0x38,0x15,0x81,0x25,0x7f,0x75,0x08,0x95,0x03,
    0x81,0x06,0xc0,0xc0
};

/* game pad with report ids: id 1 = X,Y (8 bit, signed) + 4 buttons;
 *                            id 2 = 16-bit throttle */
static const UCHAR pad[] = {
    0x05,0x01,0x09,0x05,0xa1,0x01,
      0x85,0x01,0x09,0x30,0x09,0x31,0x15,0x81,0x25,0x7f,0x75,0x08,0x95,0x02,0x81,0x02,
      0x05,0x09,0x19,0x01,0x29,0x04,0x15,0x00,0x25,0x01,0x75,0x01,0x95,0x04,0x81,0x02,
      0x75,0x04,0x95,0x01,0x81,0x03,
      0x85,0x02,0x05,0x01,0x09,0x36,0x16,0x00,0x00,0x26,0xff,0x03,0x75,0x10,0x95,0x01,0x81,0x02,
    0xc0
};

static PHIDP_PREPARSED_DATA First(HIDP_DEVICE_DESC *d, const UCHAR *desc, ULONG n)
{
    NTSTATUS st = HidP_GetCollectionDescription((PUCHAR)desc, n, 0, d);
    if (!NT_SUCCESS(st)) { printf("parse failed %08lx\n", (ULONG)st); fails++; return NULL; }
    return d->CollectionDesc[0].PreparsedData;
}

static void TestKeyboard(void)
{
    HIDP_DEVICE_DESC d;
    PHIDP_PREPARSED_DATA pd = First(&d, kbd, sizeof(kbd));
    HIDP_CAPS caps;
    HIDP_BUTTON_CAPS bc[8];
    HIDP_VALUE_CAPS vc[8];
    USHORT n;
    UCHAR in[9] = {0}, out[2] = {0};        /* byte 0 = report id (0: the device has none) */
    USAGE list[16];
    ULONG len;

    if (!pd) return;
    printf("keyboard: %lu collection(s), input %u output %u feature %u bytes\n",
           d.CollectionDescLength, d.CollectionDesc[0].InputLength,
           d.CollectionDesc[0].OutputLength, d.CollectionDesc[0].FeatureLength);
    CHECK(d.CollectionDescLength == 1);
    CHECK(d.CollectionDesc[0].UsagePage == 0x01 && d.CollectionDesc[0].Usage == 0x06);
    CHECK(d.CollectionDesc[0].InputLength == 9);
    CHECK(d.CollectionDesc[0].OutputLength == 2);

    CHECK_ST(HIDP_STATUS_SUCCESS, HidP_GetCaps(pd, &caps));
    CHECK(caps.NumberInputButtonCaps == 2);          /* modifiers + key array */
    CHECK(caps.NumberInputValueCaps == 0);
    CHECK(caps.NumberOutputButtonCaps == 1);         /* LEDs */

    n = 8;
    CHECK_ST(HIDP_STATUS_SUCCESS, HidP_GetSpecificButtonCaps(HidP_Input, 0x07, 0, 0, bc, &n, pd));
    CHECK(n == 2);
    CHECK(bc[0].UsagePage == 0x07 && bc[0].IsRange && bc[0].Range.UsageMin == 0xe0 && bc[0].Range.UsageMax == 0xe7);
    CHECK(bc[1].UsagePage == 0x07 && bc[1].IsRange && bc[1].Range.UsageMin == 0x00 && bc[1].Range.UsageMax == 0xff);
    n = 8;
    CHECK_ST(HIDP_STATUS_SUCCESS, HidP_GetSpecificButtonCaps(HidP_Output, 0x08, 0, 0, bc, &n, pd));
    CHECK(n == 1 && bc[0].IsRange && bc[0].Range.UsageMin == 1 && bc[0].Range.UsageMax == 3);
    n = 8;
    CHECK_ST(HIDP_STATUS_USAGE_NOT_FOUND, HidP_GetSpecificValueCaps(HidP_Input, 0, 0, 0, vc, &n, pd));

    CHECK(HidP_MaxUsageListLength(HidP_Input, 0x07, pd) == 8 + 6);

    /* E1 log: 'h' = usage 0x0b pressed, then released */
    in[3] = 0x0b;
    len = 16;
    CHECK_ST(HIDP_STATUS_SUCCESS, HidP_GetUsages(HidP_Input, 0x07, 0, list, &len, pd, (PCHAR)in, 9));
    CHECK(len == 1 && list[0] == 0x0b);
    /* left shift + 'a' + 'b' */
    in[1] = 0x02; in[3] = 0x04; in[4] = 0x05;
    len = 16;
    CHECK_ST(HIDP_STATUS_SUCCESS, HidP_GetUsages(HidP_Input, 0x07, 0, list, &len, pd, (PCHAR)in, 9));
    CHECK(len == 3 && list[0] == 0xe1 && list[1] == 0x04 && list[2] == 0x05);
    /* nothing pressed */
    in[1] = in[3] = in[4] = 0;
    len = 16;
    CHECK_ST(HIDP_STATUS_SUCCESS, HidP_GetUsages(HidP_Input, 0x07, 0, list, &len, pd, (PCHAR)in, 9));
    CHECK(len == 0);
    /* too small a list */
    in[3] = 0x04; in[4] = 0x05;
    len = 1;
    CHECK_ST(HIDP_STATUS_BUFFER_TOO_SMALL, HidP_GetUsages(HidP_Input, 0x07, 0, list, &len, pd, (PCHAR)in, 9));
    CHECK(len == 2);
    /* wrong length / wrong page */
    len = 16;
    CHECK_ST(HIDP_STATUS_INVALID_REPORT_LENGTH, HidP_GetUsages(HidP_Input, 0x07, 0, list, &len, pd, (PCHAR)in, 8));
    CHECK_ST(HIDP_STATUS_USAGE_NOT_FOUND, HidP_GetUsages(HidP_Input, 0x09, 0, list, &len, pd, (PCHAR)in, 9));

    /* LEDs: Num Lock (1) and Scroll Lock (3) */
    list[0] = 1; list[1] = 3; len = 2;
    CHECK_ST(HIDP_STATUS_SUCCESS, HidP_SetUsages(HidP_Output, 0x08, 0, list, &len, pd, (PCHAR)out, 2));
    CHECK(out[1] == 0x05);
    list[0] = 1; len = 1;
    CHECK_ST(HIDP_STATUS_SUCCESS, HidP_UnsetUsages(HidP_Output, 0x08, 0, list, &len, pd, (PCHAR)out, 2));
    CHECK(out[1] == 0x04);

    /* SetUsages on the key array: fills the first empty slot */
    list[0] = 0x04; list[1] = 0x05; len = 2;
    HpZero(in, 9);
    CHECK_ST(HIDP_STATUS_SUCCESS, HidP_SetUsages(HidP_Input, 0x07, 0, list, &len, pd, (PCHAR)in, 9));
    CHECK(in[3] == 0x04 && in[4] == 0x05);

    HidP_FreeCollectionDescription(&d);
}

static void TestMouse(void)
{
    HIDP_DEVICE_DESC d;
    PHIDP_PREPARSED_DATA pd = First(&d, mouse, sizeof(mouse));
    HIDP_CAPS caps;
    HIDP_VALUE_CAPS vc[8];
    HIDP_LINK_COLLECTION_NODE nodes[8];
    USHORT n;
    ULONG len, uv;
    LONG sv;
    UCHAR in[5] = { 0x00, 0x05, 0xfb, 0x10, 0xff };  /* id 0; left+right, x=-5, y=16, wheel=-1 */
    USAGE list[8];

    if (!pd) return;
    CHECK(d.CollectionDescLength == 1);
    CHECK(d.CollectionDesc[0].UsagePage == 0x01 && d.CollectionDesc[0].Usage == 0x02);
    CHECK(d.CollectionDesc[0].InputLength == 5);
    CHECK_ST(HIDP_STATUS_SUCCESS, HidP_GetCaps(pd, &caps));
    printf("mouse: nodes %u, input buttons %u values %u\n", caps.NumberLinkCollectionNodes,
           caps.NumberInputButtonCaps, caps.NumberInputValueCaps);
    CHECK(caps.NumberLinkCollectionNodes == 2);      /* application + physical */
    CHECK(caps.NumberInputButtonCaps == 1);
    CHECK(caps.NumberInputValueCaps == 3);           /* X, Y, wheel */

    len = 8;
    CHECK_ST(HIDP_STATUS_SUCCESS, HidP_GetLinkCollectionNodes(nodes, &len, pd));
    CHECK(len == 2 && nodes[1].Parent == 0 && nodes[0].NumberOfChildren == 1 && nodes[1].CollectionType == 0);

    n = 8;
    CHECK_ST(HIDP_STATUS_SUCCESS, HidP_GetSpecificValueCaps(HidP_Input, 0x01, 0, 0, vc, &n, pd));
    CHECK(n == 3 && vc[0].NotRange.Usage == 0x30 && vc[1].NotRange.Usage == 0x31 && vc[2].NotRange.Usage == 0x38);
    CHECK(vc[0].BitSize == 8 && vc[0].LogicalMin == -127 && vc[0].LogicalMax == 127 && !vc[0].IsAbsolute);

    len = 8;
    CHECK_ST(HIDP_STATUS_SUCCESS, HidP_GetUsages(HidP_Input, 0x09, 0, list, &len, pd, (PCHAR)in, 5));
    CHECK(len == 2 && list[0] == 1 && list[1] == 3);
    CHECK_ST(HIDP_STATUS_SUCCESS, HidP_GetScaledUsageValue(HidP_Input, 0x01, 0, 0x30, &sv, pd, (PCHAR)in, 5));
    CHECK(sv == -5);
    CHECK_ST(HIDP_STATUS_SUCCESS, HidP_GetScaledUsageValue(HidP_Input, 0x01, 0, 0x31, &sv, pd, (PCHAR)in, 5));
    CHECK(sv == 16);
    CHECK_ST(HIDP_STATUS_SUCCESS, HidP_GetScaledUsageValue(HidP_Input, 0x01, 0, 0x38, &sv, pd, (PCHAR)in, 5));
    CHECK(sv == -1);
    CHECK_ST(HIDP_STATUS_SUCCESS, HidP_GetUsageValue(HidP_Input, 0x01, 0, 0x30, &uv, pd, (PCHAR)in, 5));
    CHECK(uv == 0xfb);
    CHECK_ST(HIDP_STATUS_USAGE_NOT_FOUND, HidP_GetUsageValue(HidP_Input, 0x01, 0, 0x35, &uv, pd, (PCHAR)in, 5));

    HidP_FreeCollectionDescription(&d);
}

static void TestPad(void)
{
    HIDP_DEVICE_DESC d;
    PHIDP_PREPARSED_DATA pd = First(&d, pad, sizeof(pad));
    HIDP_CAPS caps;
    UCHAR rep1[4] = { 0x01, 0x10, 0xf0, 0x0a };      /* buttons 2 and 4 */
    ULONG len, uv;
    USAGE list[4];
    HIDP_DATA data[16];

    if (!pd) return;
    CHECK(d.CollectionDescLength == 1);
    CHECK(d.ReportIDsLength == 2);
    CHECK(d.CollectionDesc[0].InputLength == 4);     /* id + X + Y + buttons byte; id 2 is 3 */
    CHECK_ST(HIDP_STATUS_SUCCESS, HidP_GetCaps(pd, &caps));
    CHECK(caps.InputReportByteLength == 4);
    printf("pad: input %u bytes, button caps %u, value caps %u, ids %lu\n",
           caps.InputReportByteLength, caps.NumberInputButtonCaps, caps.NumberInputValueCaps,
           d.ReportIDsLength);

    len = 4;
    CHECK_ST(HIDP_STATUS_SUCCESS, HidP_GetUsages(HidP_Input, 0x09, 0, list, &len, pd, (PCHAR)rep1, 4));
    CHECK(len == 2 && list[0] == 2 && list[1] == 4);
    {
        UCHAR rep2full[4] = { 0x02, 0x34, 0x02, 0x00 };
        CHECK_ST(HIDP_STATUS_SUCCESS, HidP_GetUsageValue(HidP_Input, 0x01, 0, 0x36, &uv, pd, (PCHAR)rep2full, 4));
        CHECK(uv == 0x0234);
    }
    len = 16;
    CHECK_ST(HIDP_STATUS_SUCCESS, HidP_GetData(HidP_Input, data, &len, pd, (PCHAR)rep1, 4));
    printf("pad GetData(report 1): %lu entries\n", len);
    CHECK(len == 4);                                 /* X, Y, button 2, button 4 */
    {
        UCHAR init[4];
        CHECK_ST(HIDP_STATUS_SUCCESS, HidP_InitializeReportForID(HidP_Input, 2, pd, (PCHAR)init, 4));
        CHECK(init[0] == 2);
        CHECK_ST(HIDP_STATUS_REPORT_DOES_NOT_EXIST, HidP_InitializeReportForID(HidP_Input, 9, pd, (PCHAR)init, 4));
    }
    {   /* GetData -> SetData into a fresh report gives the report back */
        UCHAR fresh[4];
        ULONG nd = 16;
        USAGE_AND_PAGE up[4];
        ULONG nu = 4;

        CHECK_ST(HIDP_STATUS_SUCCESS, HidP_InitializeReportForID(HidP_Input, 1, pd, (PCHAR)fresh, 4));
        CHECK_ST(HIDP_STATUS_SUCCESS, HidP_SetData(HidP_Input, data, &len, pd, (PCHAR)fresh, 4));
        CHECK(memcmp(fresh, rep1, 4) == 0);
        (void)nd;

        CHECK_ST(HIDP_STATUS_SUCCESS, HidP_GetUsagesEx(HidP_Input, 0, up, &nu, pd, (PCHAR)rep1, 4));
        CHECK(nu == 2 && up[0].UsagePage == 0x09 && up[0].Usage == 2 && up[1].Usage == 4);
    }
    HidP_FreeCollectionDescription(&d);
}

static void TestScaled(void)
{
    /* one 8-bit value, logical 0..255, physical -100..100, absolute */
    static const UCHAR desc[] = {
        0x05,0x01,0x09,0x04,0xa1,0x01,0x09,0x30,0x15,0x00,0x26,0xff,0x00,
        0x35,0x9c,0x45,0x64,0x75,0x08,0x95,0x01,0x81,0x02,0xc0
    };
    HIDP_DEVICE_DESC d;
    PHIDP_PREPARSED_DATA pd = First(&d, desc, sizeof(desc));
    UCHAR r[2] = { 0, 0 };          /* id 0, then the value */
    LONG sv;

    if (!pd) return;
    r[1] = 0;   CHECK_ST(HIDP_STATUS_SUCCESS, HidP_GetScaledUsageValue(HidP_Input, 1, 0, 0x30, &sv, pd, (PCHAR)r, 2)); CHECK(sv == -100);
    r[1] = 255; CHECK_ST(HIDP_STATUS_SUCCESS, HidP_GetScaledUsageValue(HidP_Input, 1, 0, 0x30, &sv, pd, (PCHAR)r, 2)); CHECK(sv == 100);
    r[1] = 128; CHECK_ST(HIDP_STATUS_SUCCESS, HidP_GetScaledUsageValue(HidP_Input, 1, 0, 0x30, &sv, pd, (PCHAR)r, 2)); CHECK(sv >= 0 && sv <= 1);

    /* the other way: physical -> logical */
    r[0] = 0; r[1] = 0;
    CHECK_ST(HIDP_STATUS_SUCCESS, HidP_SetScaledUsageValue(HidP_Input, 1, 0, 0x30, 100, pd, (PCHAR)r, 2));
    CHECK(r[1] == 255);
    CHECK_ST(HIDP_STATUS_SUCCESS, HidP_SetScaledUsageValue(HidP_Input, 1, 0, 0x30, -100, pd, (PCHAR)r, 2));
    CHECK(r[1] == 0);
    CHECK_ST(HIDP_STATUS_SUCCESS, HidP_SetScaledUsageValue(HidP_Input, 1, 0, 0x30, 0, pd, (PCHAR)r, 2));
    CHECK(r[1] >= 127 && r[1] <= 128);
    CHECK_ST(HIDP_STATUS_VALUE_OUT_OF_RANGE, HidP_SetScaledUsageValue(HidP_Input, 1, 0, 0x30, 101, pd, (PCHAR)r, 2));
    HidP_FreeCollectionDescription(&d);
}

static void TestDiff(void)
{
    USAGE prev[5] = { 1, 2, 3, 0, 0 }, cur[5] = { 2, 3, 4, 0, 0 }, brk[5], make[5];

    CHECK_ST(HIDP_STATUS_SUCCESS, HidP_UsageListDifference(prev, cur, brk, make, 5));
    CHECK(brk[0] == 1 && brk[1] == 0 && make[0] == 4 && make[1] == 0);
}

static void TestBad(void)
{
    HIDP_DEVICE_DESC d;
    static const UCHAR trunc[] = { 0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0x05 };    /* cut short */
    static const UCHAR noend[] = { 0x05, 0x01, 0x09, 0x06, 0xa1, 0x01 };           /* unclosed */
    CHECK(!NT_SUCCESS(HidP_GetCollectionDescription((PUCHAR)trunc, sizeof(trunc), 0, &d)));
    CHECK(!NT_SUCCESS(HidP_GetCollectionDescription((PUCHAR)noend, sizeof(noend), 0, &d)));
}

int main(void)
{
    TestKeyboard();
    TestMouse();
    TestPad();
    TestScaled();
    TestDiff();
    TestBad();
    printf(fails ? "FAILED: %d\n" : "ALL OK\n", fails);
    return fails != 0;
}

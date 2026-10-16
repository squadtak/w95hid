/*
 * hpport.h - environment adapter for the HID report-descriptor parser.
 *
 * The parser (hidparse.c) is plain C with no OS dependencies beyond the
 * types of the DDK HID headers and an allocator.  It is built three ways:
 *
 *   HP_KERNEL  HIDPARSE.SYS (WDM driver)         - includes <wdm.h>
 *   HP_VXD     embedded in KBDHID.VXD/MOUHID.VXD - no CRT, own allocator
 *   (neither)  host unit test (hptest.c)         - tiny shim types below
 *
 * The public structures come from the 98DDK headers hidpi.h / hidpddi.h
 * (src\hid\inc) so the layout is the one every HID client expects.
 */
#ifndef _HPPORT_H
#define _HPPORT_H

#if defined(HP_KERNEL)

#include <wdm.h>
#define HP_PRAGMA_PACK_OK
#define HpAlloc(_n_, _pool_)    ExAllocatePoolWithTag((POOL_TYPE)(_pool_), (_n_), 'PdiH')
#define HpFree(_p_)             ExFreePool(_p_)
#define HpCopy(_d_, _s_, _n_)   RtlCopyMemory((_d_), (_s_), (_n_))
#define HpZero(_d_, _n_)        RtlZeroMemory((_d_), (_n_))

#elif defined(HP_VXD)

#include "hpvxd.h"          /* typedefs + HpAlloc/HpFree/HpCopy/HpZero */

#else /* host test */

#include <stdlib.h>
#include <string.h>
typedef long            NTSTATUS;
typedef unsigned char   UCHAR, *PUCHAR;
typedef unsigned short  USHORT, *PUSHORT;
typedef unsigned long   ULONG, *PULONG;
typedef long            LONG, *PLONG;
typedef char            CHAR, *PCHAR;
typedef unsigned char   BOOLEAN;
typedef void            VOID, *PVOID;
typedef int             POOL_TYPE;
#define IN
#define OUT
#define TRUE  1
#define FALSE 0
#define NT_SUCCESS(_s_) ((NTSTATUS)(_s_) >= 0)
#define STATUS_SUCCESS                  ((NTSTATUS)0x00000000L)
#define STATUS_INVALID_PARAMETER        ((NTSTATUS)0xC000000DL)
#define STATUS_INSUFFICIENT_RESOURCES   ((NTSTATUS)0xC000009AL)
#define STATUS_BUFFER_TOO_SMALL         ((NTSTATUS)0xC0000023L)
#define STATUS_COULD_NOT_INTERPRET      ((NTSTATUS)0xC00000B9L)
#define STATUS_INVALID_PARAMETER_MIX    ((NTSTATUS)0xC0000030L)
#define HpAlloc(_n_, _pool_)    malloc(_n_)
#define HpFree(_p_)             free(_p_)
#define HpCopy(_d_, _s_, _n_)   memcpy((_d_), (_s_), (_n_))
#define HpZero(_d_, _n_)        memset((_d_), 0, (_n_))

#endif

#if !defined(HP_KERNEL)
#include "hidusage.h"
#include "hidpi.h"
#else
#include <hidusage.h>
#include <hidpi.h>
#endif
#include "hidpddi.h"

/* exported by HIDPARSE.SYS but not declared by the 98DDK hidpi.h */
NTSTATUS __stdcall HidP_InitializeReportForID(
    IN HIDP_REPORT_TYPE ReportType, IN UCHAR ReportID,
    IN PHIDP_PREPARSED_DATA PreparsedData, IN OUT PCHAR Report, IN ULONG ReportLength);

#endif

/*
 * hpvxd.h - hidparse.c built into a VxD (HP_VXD): no CRT, no OS headers.
 *
 * Types are the ones the DDK HID headers (hidusage.h / hidpi.h / hidpddi.h)
 * expect, the allocator is the VMM heap (through KBDHID's wrappers), and the
 * code and data go into the locked segments of the VxD.
 */
#ifndef _HPVXD_H
#define _HPVXD_H

#pragma code_seg("_LTEXT", "LCODE")
#pragma data_seg("_LDATA", "LCODE")

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
#define NULL  0
#define NT_SUCCESS(_s_) ((NTSTATUS)(_s_) >= 0)
#define STATUS_SUCCESS                  ((NTSTATUS)0x00000000L)
#define STATUS_INVALID_PARAMETER        ((NTSTATUS)0xC000000DL)
#define STATUS_INSUFFICIENT_RESOURCES   ((NTSTATUS)0xC000009AL)
#define STATUS_BUFFER_TOO_SMALL         ((NTSTATUS)0xC0000023L)
#define STATUS_COULD_NOT_INTERPRET      ((NTSTATUS)0xC00000B9L)
#define STATUS_INVALID_PARAMETER_MIX    ((NTSTATUS)0xC0000030L)

/* provided by kbdhid.c */
extern void *KbdAlloc(unsigned long n);
extern void  KbdFree(void *p);
extern void  KbdCopy(void *dst, const void *src, unsigned long n);
extern void  KbdZero(void *dst, unsigned long n);

#define HpAlloc(_n_, _pool_)    KbdAlloc(_n_)
#define HpFree(_p_)             KbdFree(_p_)
#define HpCopy(_d_, _s_, _n_)   KbdCopy((_d_), (_s_), (_n_))
#define HpZero(_d_, _n_)        KbdZero((_d_), (_n_))

#endif

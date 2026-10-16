;******************************************************************************
; HIDEXT.ASM - HIDEXT.VXD for Windows 95 OSR2.1: helper for the HID drivers
;
; A small static VxD that gives the WDM drivers loaded by NTKERN what they can
; not see themselves.  First service: the shutdown notice.  The WDM side of
; Windows 95 OSR2.1 sends no IRP_MJ_SHUTDOWN before it removes the devices, and
; by the time a device REMOVE arrives the USB host controller no longer completes
; anything (the hub driver's REMOVE never returns, the machine hangs on "Please
; wait while your computer shuts down").  VMM's KERNEL32_SHUTDOWN comes earlier.
;
; At KERNEL32_SHUTDOWN this VxD opens \DosDevices\HIDSHUT0 .. HIDSHUT7 through
; NTKERN's file services and sends IOCTL_HIDEXT_SHUTDOWN (0x00222408) to each
; one that exists.  A WDM driver that wants the notice creates such a control
; device (HIDCLASS.SYS owns HIDSHUT0) and handles that IOCTL.
;
; This file: the DDB, the control procedure and the NTKERN service wrappers.
; Build: see makefile (-DWIN40COMPAT: the DDB must say SDK version 0400h).
;******************************************************************************

        .386p
        .xlist
        include vmm.inc
        include debug.inc
        .list
        include ntkern.inc

DECLARE_VIRTUAL_DEVICE HIDEXT, 1, 0, HIDEXT_Control, UNDEFINED_DEVICE_ID, \
                       UNDEFINED_INIT_ORDER

        EXTRN   _HidExtCtl@4:NEAR

VxD_LOCKED_CODE_SEG

;------------------------------------------------------------------------------
; control messages: all go to the C code, which picks the ones it wants
;------------------------------------------------------------------------------
BeginProc HIDEXT_Control
        cmp     eax, KERNEL32_INITIALIZED
        jne     short hc_msg
        ; DDB_Reference_Data = 1 from now on: the dynamic VxDs (MOUHID) read it
        ; through Get_DDB to know that Windows has come up, even when they are
        ; loaded (hot plug) long after this message went by
        mov     HIDEXT_DDB.DDB_Reference_Data, 1
hc_msg:
        pushad
        push    eax                     ; EAX = message number
        call    _HidExtCtl@4
        popad
        clc
        ret
EndProc HIDEXT_Control

;------------------------------------------------------------------------------
; NTKERN file services (stdcall, NT semantics)
;------------------------------------------------------------------------------
PUBLIC _NtKernCreateFile@44
_NtKernCreateFile@44 PROC
        VxDJmp  _NtKernCreateFile
_NtKernCreateFile@44 ENDP

PUBLIC _NtKernClose@4
_NtKernClose@4 PROC
        VxDJmp  _NtKernClose
_NtKernClose@4 ENDP

PUBLIC _NtKernDeviceIoControl@40
_NtKernDeviceIoControl@40 PROC
        VxDJmp  _NtKernDeviceIoControl
_NtKernDeviceIoControl@40 ENDP

VxD_LOCKED_CODE_ENDS

VxD_REAL_INIT_SEG

BeginProc HIDEXT_Real_Init
        xor     bx, bx
        xor     si, si
        xor     edx, edx
        mov     ax, Device_Load_Ok
        ret
EndProc HIDEXT_Real_Init

VxD_REAL_INIT_ENDS

        END HIDEXT_Real_Init

;******************************************************************************
; MOUHID.ASM - MOUHID.VXD for Windows 95 OSR2.1: the VxD front end
;
; HID mouse client.  Same shape as the Win98 MOUHID.VXD: NTKERN loads it
; when a HID mouse child node (HID_DEVICE_SYSTEM_KEYBOARD) arrives, it opens
; the HIDCLASS collections (\\.\HIDn) through NTKERN's file services, reads the
; input reports and feeds the scan codes into VKD.
;
; This file: the DDB, the control procedure and thin wrappers that let the C
; code (kbdhid.c) call NTKERN / VMM / VKD services.  Build: see makefile
; (-DWIN40COMPAT: the DDB must say SDK version 0400h for the OSR2.1 VMM).
;******************************************************************************

        .386p
        .xlist
        include vmm.inc
        include debug.inc
        .list
        include vmd.inc
        include ntkern.inc

; CONFIGMG.INC: return value of PNP_NEW_DEVNODE for "not handled here"
IFNDEF CR_DEFAULT
CR_DEFAULT      EQU     00000001H
ENDIF

DECLARE_VIRTUAL_DEVICE MOUHID, 1, 0, MOUHID_Control, UNDEFINED_DEVICE_ID, \
                       UNDEFINED_INIT_ORDER

        EXTRN   _MouInit@0:NEAR
        EXTRN   _MouExit@0:NEAR
        EXTRN   _MouNewDevnode@0:NEAR
        EXTRN   _MouTimer@4:NEAR
        EXTRN   _MouCtl@4:NEAR

VxD_LOCKED_CODE_SEG

;------------------------------------------------------------------------------
; control messages
;------------------------------------------------------------------------------
BeginProc MOUHID_Control
        pushad                          ; trace every message (EAX = message number)
        push    eax
        call    _MouCtl@4
        popad
        cmp     eax, SYS_DYNAMIC_DEVICE_INIT
        je      short c_init
        cmp     eax, SYS_DYNAMIC_DEVICE_EXIT
        je      short c_exit
        cmp     eax, PNP_NEW_DEVNODE
        je      short c_newdev
        clc
        ret

c_init:
        pushad
        call    _MouInit@0
        popad
        clc
        ret

c_exit:                                 ; carry set = still busy, do not unload
        pushad
        call    _MouExit@0
        or      eax, eax
        popad                           ; popad leaves the flags alone
        jnz     short c_busy
        clc
        ret
c_busy:
        stc
        ret

c_newdev:
        pushad
        call    _MouNewDevnode@0
        popad
        mov     eax, CR_DEFAULT
        clc
        ret
EndProc MOUHID_Control

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

PUBLIC _NtKernReadFile@36
_NtKernReadFile@36 PROC
        VxDJmp  _NtKernReadFile
_NtKernReadFile@36 ENDP

PUBLIC _NtKernDeviceIoControl@40
_NtKernDeviceIoControl@40 PROC
        VxDJmp  _NtKernDeviceIoControl
_NtKernDeviceIoControl@40 ENDP

PUBLIC _NtKernQueueWorkItem@8
_NtKernQueueWorkItem@8 PROC
        VxDJmp  _NtKernQueueWorkItem
_NtKernQueueWorkItem@8 ENDP

;------------------------------------------------------------------------------
; VMM heap (cdecl)
;------------------------------------------------------------------------------
PUBLIC _VmmHeapAlloc
_VmmHeapAlloc PROC
        VMMJmp  _HeapAllocate
_VmmHeapAlloc ENDP

PUBLIC _VmmHeapFree
_VmmHeapFree PROC
        VMMJmp  _HeapFree
_VmmHeapFree ENDP

;------------------------------------------------------------------------------
; timers.  SetTimer(ms, ref) -> handle ; the callback ends up in MouTimer(ref)
;------------------------------------------------------------------------------
TimerGlue PROC
        pushad
        push    edx                     ; reference data
        call    _MouTimer@4
        popad
        ret
TimerGlue ENDP

PUBLIC _SetTimer@8
_SetTimer@8 PROC
        push    esi
        mov     eax, [esp+8]            ; milliseconds
        mov     edx, [esp+12]           ; reference data
        mov     esi, OFFSET32 TimerGlue
        VMMcall Set_Global_Time_Out
        mov     eax, esi                ; time-out handle
        pop     esi
        ret     8
_SetTimer@8 ENDP

PUBLIC _CancelTimer@4
_CancelTimer@4 PROC
        push    esi
        mov     esi, [esp+8]
        VMMcall Cancel_Time_Out
        pop     esi
        ret     4
_CancelTimer@4 ENDP

;------------------------------------------------------------------------------
; interrupt flag save / restore for short critical sections
;------------------------------------------------------------------------------
PUBLIC _IrqSave@0
_IrqSave@0 PROC
        pushfd
        pop     eax
        cli
        ret
_IrqSave@0 ENDP

PUBLIC _IrqRestore@4
_IrqRestore@4 PROC
        push    dword ptr [esp+4]
        popfd
        ret     4
_IrqRestore@4 ENDP

;------------------------------------------------------------------------------
; PostPointer(buttons, dx, dy, wheel): relative motion + button state (+ wheel) into VMOUSE.
; buttons byte: 0 0 b1 b3 b2 b4 0 0  (bit5 left, bit4 middle, bit3 right, bit2 4th)
;------------------------------------------------------------------------------
;------------------------------------------------------------------------------
; GetSysTime(): milliseconds since Windows started (VMM Get_System_Time)
;------------------------------------------------------------------------------
PUBLIC _GetSysTime@0
_GetSysTime@0 PROC
        VMMcall Get_System_Time
        ret
_GetSysTime@0 ENDP

;------------------------------------------------------------------------------
; HidExtReady(): 1 once the static HIDEXT.VXD has seen KERNEL32_INITIALIZED
; (it keeps that in its DDB's reference data); 0 before, or if HIDEXT is absent
;------------------------------------------------------------------------------
hidext_name     db      'HIDEXT  ', 0

PUBLIC _HidExtReady@0
_HidExtReady@0 PROC
        push    edi
        push    ecx
        mov     eax, UNDEFINED_DEVICE_ID
        mov     edi, OFFSET32 hidext_name
        VMMcall Get_DDB                 ; ECX = DDB or 0
        xor     eax, eax
        jecxz   short her_done
        mov     eax, [ecx.DDB_Reference_Data]
her_done:
        pop     ecx
        pop     edi
        ret
_HidExtReady@0 ENDP

;------------------------------------------------------------------------------
; PostPointer(buttons, dx, dy, wheel)
;
; The wheel is for the wheel-aware VMOUSE of Microsoft IntelliPoint (Win98 SE / IntelliPoint
; 4.0: EAX bit 0 says "ECX is the wheel"; IntelliPoint 2.2 takes ECX without that bit).
; ECX is always set: the stock Win95 VMOUSE ignores it, a wheel-aware one reads it.
; wheel: the raw count of a PS/2 IntelliMouse packet, signed 4 bits (-7..7, up = positive);
; the IntelliPoint mini driver scales it to WHEEL_DELTA (120) units.
;------------------------------------------------------------------------------
PUBLIC _PostPointer@16
_PostPointer@16 PROC
        pushad
        cld
        VMMcall Get_Cur_VM_Handle       ; VMOUSE wants EBX = a VM handle (C leaves garbage there)
        movzx   eax, BYTE PTR [esp+36]  ; 32 (pushad) + 4 (return)
        mov     esi, [esp+40]
        mov     edi, [esp+44]
        mov     ecx, [esp+48]
        jecxz   short pp_post           ; no wheel: ECX = 0, bit 0 stays clear
        or      al, 1                   ; "ECX is the wheel"
pp_post:
        VxDCall VMD_Post_Pointer_Message
        popad
        ret     16
_PostPointer@16 ENDP

VxD_LOCKED_CODE_ENDS

VxD_REAL_INIT_SEG

BeginProc MOUHID_Real_Init
        xor     bx, bx
        xor     si, si
        xor     edx, edx
        mov     ax, Device_Load_Ok
        ret
EndProc MOUHID_Real_Init

VxD_REAL_INIT_ENDS

        END MOUHID_Real_Init

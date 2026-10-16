;******************************************************************************
; KBDHID.ASM - KBDHID.VXD for Windows 95 OSR2.1: the VxD front end
;
; HID keyboard client.  Same shape as the Win98 KBDHID.VXD: NTKERN loads it
; when a HID keyboard child node (HID_DEVICE_SYSTEM_KEYBOARD) arrives, it opens
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
        include vkd.inc
        include ntkern.inc

; CONFIGMG.INC: return value of PNP_NEW_DEVNODE for "not handled here"
IFNDEF CR_DEFAULT
CR_DEFAULT      EQU     00000001H
ENDIF

DECLARE_VIRTUAL_DEVICE KBDHID, 1, 0, KBDHID_Control, UNDEFINED_DEVICE_ID, \
                       UNDEFINED_INIT_ORDER

        EXTRN   _KbdInit@0:NEAR
        EXTRN   _KbdExit@0:NEAR
        EXTRN   _KbdNewDevnode@0:NEAR
        EXTRN   _KbdTimer@4:NEAR
        EXTRN   _KbdCtl@4:NEAR

VxD_LOCKED_CODE_SEG

;------------------------------------------------------------------------------
; control messages
;------------------------------------------------------------------------------
BeginProc KBDHID_Control
        pushad                          ; trace every message (EAX = message number)
        push    eax
        call    _KbdCtl@4
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
        call    _KbdInit@0
        popad
        clc
        ret

c_exit:                                 ; carry set = still busy, do not unload
        pushad
        call    _KbdExit@0
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
        call    _KbdNewDevnode@0
        popad
        mov     eax, CR_DEFAULT
        clc
        ret
EndProc KBDHID_Control

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

PUBLIC _NtKernWriteFile@36
_NtKernWriteFile@36 PROC
        VxDJmp  _NtKernWriteFile
_NtKernWriteFile@36 ENDP

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
; timers.  SetTimer(ms, ref) -> handle ; the callback ends up in KbdTimer(ref)
;------------------------------------------------------------------------------
TimerGlue PROC
        pushad
        push    edx                     ; reference data
        call    _KbdTimer@4
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
; GetLeds(): the keyboard LED bits (1 scroll, 2 num, 4 caps) from the toggle
; state VKD keeps for the VM that owns the keyboard.
;------------------------------------------------------------------------------
PUBLIC _GetLeds@0
_GetLeds@0 PROC
        pushad
        VxDCall VKD_Get_Kbd_Owner               ; EBX = VM handle
        VxDCall VKD_Get_Shift_State             ; AX = shift state
        movzx   eax, ax
        shr     eax, 4
        and     eax, 7
        mov     [esp+28], eax                   ; EAX slot of the pushad frame
        popad
        ret
_GetLeds@0 ENDP

;------------------------------------------------------------------------------
; SendScanByte(code): one set-1 scan code byte into the keyboard path, the way
; the Win98 KBDHID does: let other VxDs filter it, then force it into VKD.
; Returns 1 if it went in.
;------------------------------------------------------------------------------
PUBLIC _SendScanByte@4
_SendScanByte@4 PROC
        push    ebx
        push    esi
        push    ecx
        mov     ecx, [esp+16]           ; code
        and     ecx, 0FFh
        VxDCall VKD_Filter_Keyboard_Input
        jc      short ssb_drop
        push    ecx                     ; the (possibly changed) code, on the stack
        mov     esi, esp
        mov     ecx, 1
        VxDCall VKD_Force_Keys
        pop     ecx
        mov     eax, 1
        jmp     short ssb_out
ssb_drop:
        xor     eax, eax
ssb_out:
        pop     ecx
        pop     esi
        pop     ebx
        ret     4
_SendScanByte@4 ENDP

VxD_LOCKED_CODE_ENDS

VxD_REAL_INIT_SEG

BeginProc KBDHID_Real_Init
        xor     bx, bx
        xor     si, si
        xor     edx, edx
        mov     ax, Device_Load_Ok
        ret
EndProc KBDHID_Real_Init

VxD_REAL_INIT_ENDS

        END KBDHID_Real_Init

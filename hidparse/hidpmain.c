/*
 * hidpmain.c - HIDPARSE.SYS entry point.
 *
 * HIDPARSE.SYS only exports the HidP_* functions; NTKERN loads it when
 * another driver (HIDCLASS.SYS) imports from it.
 */
#include <wdm.h>

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    UNREFERENCED_PARAMETER(DriverObject);
    UNREFERENCED_PARAMETER(RegistryPath);
    return STATUS_SUCCESS;
}

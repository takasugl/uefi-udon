/** @file
  Give EL1 access to the physical timer (CNTHCTL_EL2.EL1PCEN/EL1PCTEN).

  The stock Android bootloader leaves CNTHCTL_EL2 with EL1PCEN=0 because the
  Linux kernel only uses the virtual timer. Apple's XNU disables the physical
  timer as a precaution during fiq_context_init, so without these bits the
  kernel takes an undefined-instruction exception on `msr CNTP_CTL_EL0`.
  Firmware runs at EL2 here; set the bits and leave them for the kernel.
**/

#include <Uefi.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DebugLib.h>

STATIC
UINT64
ReadCnthctl (
  VOID
  )
{
  UINT64  Value;

  __asm__ volatile ("mrs %0, CNTHCTL_EL2" : "=r" (Value));
  return Value;
}

STATIC
VOID
WriteCnthctl (
  IN UINT64  Value
  )
{
  __asm__ volatile ("msr CNTHCTL_EL2, %0" : : "r" (Value));
  __asm__ volatile ("isb");
}

EFI_STATUS
EFIAPI
UdonTimerAccessDxeEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  UINT64  Value;
  UINT64  CurrentEl;

  __asm__ volatile ("mrs %0, CurrentEL" : "=r" (CurrentEl));
  CurrentEl >>= 2;
  Value = ReadCnthctl ();
  DEBUG ((DEBUG_ERROR, "UDONTIMER: EL%lu CNTHCTL_EL2=0x%lx\n", CurrentEl, Value));
  if (CurrentEl == 2) {
    Value |= 0x3;   /* EL1PCTEN | EL1PCEN */
    WriteCnthctl (Value);
    DEBUG ((DEBUG_ERROR, "UDONTIMER: CNTHCTL_EL2=0x%lx\n", ReadCnthctl ()));
  }

  return EFI_SUCCESS;
}

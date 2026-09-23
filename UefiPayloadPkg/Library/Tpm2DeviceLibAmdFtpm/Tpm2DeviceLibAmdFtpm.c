/** @file
  TPM2 device library for the AMD fTPM CRB interface exposed by coreboot.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <IndustryStandard/Acpi.h>
#include <IndustryStandard/Tpm20.h>
#include <IndustryStandard/Tpm2Acpi.h>
#include <Library/Tpm2DeviceLib.h>
#include "Tpm2AMDCRB.h"

EFI_STATUS
EFIAPI
Tpm2SubmitCommand (
  IN UINT32      InputParameterBlockSize,
  IN UINT8       *InputParameterBlock,
  IN OUT UINT32  *OutputParameterBlockSize,
  IN UINT8       *OutputParameterBlock
  )
{
  return Tpm2AMDfTPMSubmitCommand (
           InputParameterBlockSize,
           InputParameterBlock,
           OutputParameterBlockSize,
           OutputParameterBlock
           );
}

EFI_STATUS
EFIAPI
Tpm2RequestUseTpm (
  VOID
  )
{
  return Tpm2AMDfTPMRequestUseTpm ();
}

EFI_STATUS
EFIAPI
Tpm2RegisterTpm2DeviceLib (
  IN TPM2_DEVICE_INTERFACE  *Tpm2Device
  )
{
  return EFI_UNSUPPORTED;
}

/** @file
  TPM2 device library for the AMD fTPM CRB interface.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

EFI_STATUS
EFIAPI
Tpm2AMDfTPMProbe (
  VOID
  );

EFI_STATUS
EFIAPI
Tpm2AMDfTPMSubmitCommand (
  IN UINT32      InputParameterBlockSize,
  IN UINT8       *InputParameterBlock,
  IN OUT UINT32  *OutputParameterBlockSize,
  IN UINT8       *OutputParameterBlock
  );

EFI_STATUS
EFIAPI
Tpm2AMDfTPMRequestUseTpm (
  VOID
  );

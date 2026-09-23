/** @file
  This library is a TPM2 fTPM instance, supporting AMD fTPM CRB interface.

  It can be registered to Tpm2 Device router, to be active TPM2 engine,
  based on platform setting.

Copyright (c) 2024 Red Hat
SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Library/BaseLib.h>
#include <Library/Tpm2DeviceLib.h>

#include <Guid/TpmInstance.h>

#include "Tpm2AMDCRB.h"

TPM2_DEVICE_INTERFACE  mAMDfTpm2InternalTpm2Device = {
  TPM_DEVICE_INTERFACE_TPM20_AMD_FTPM,
  Tpm2AMDfTPMSubmitCommand,
  Tpm2AMDfTPMRequestUseTpm,
};

/**
  Registers AMD fTPM2.0 instance and caches current active TPM interface type.

  @retval EFI_SUCCESS   fTPM2.0 instance is registered, or system does not support registering a fTPM2.0 instance
**/
EFI_STATUS
EFIAPI
Tpm2InstanceLibAMDfTpmConstructor (
  VOID
  )
{
  EFI_STATUS  Status;

  Status = Tpm2RegisterTpm2DeviceLib (&mAMDfTpm2InternalTpm2Device);

  if (Status == EFI_UNSUPPORTED) {
    //
    // Unsupported means platform policy does not need this instance enabled.
    //
    return EFI_SUCCESS;
  }

  Status = Tpm2AMDfTPMProbe();
  if (Status != EFI_SUCCESS) {
    //
    // Probe failed, so fTPM is not present or not functional. Do not register this instance.
    //
    return EFI_SUCCESS;
  }

  if (Status != EFI_SUCCESS) {
    return Status;
  }

  return Status;
}

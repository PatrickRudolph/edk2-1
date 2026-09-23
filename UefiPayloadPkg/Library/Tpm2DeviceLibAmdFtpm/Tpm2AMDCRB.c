/** @file
  TPM2 interface library for the AMD fTPM CRB interface.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <PiDxe.h>
#include <IndustryStandard/Acpi.h>
#include <IndustryStandard/Tpm20.h>
#include <IndustryStandard/Tpm2Acpi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <UniversalPayload/AcpiTable.h>
#include "Tpm2AMDCRB.h"

#define AMD_FTPM_STATUS_ERROR     BIT0
#define AMD_FTPM_START_COMMAND    BIT0
#define AMD_FTPM_POLL_PERIOD_US   30
#define AMD_FTPM_READY_TIMEOUT_US (250 * 1000)
#define AMD_FTPM_CMD_TIMEOUT_US   (3500 * 1000)
#define TPM2_MANUFACTURER_ID_AMD  0x00444D41

#pragma pack(1)

typedef struct {
  TPM2_COMMAND_HEADER    Header;
  TPM_CAP                Capability;
  UINT32                 Property;
  UINT32                 PropertyCount;
} TPM2_GET_CAPABILITY_COMMAND;

typedef struct {
  TPM2_RESPONSE_HEADER    Header;
  TPMI_YES_NO             MoreData;
  TPMS_CAPABILITY_DATA    CapabilityData;
} TPM2_GET_CAPABILITY_RESPONSE;

#pragma pack()

STATIC EFI_TPM2_ACPI_CONTROL_AREA  *mControlArea;

STATIC
EFI_STATUS
GetControlArea (
  OUT UINT32  *CommandSize,
  OUT UINT64  *CommandAddress,
  OUT UINT32  *ResponseSize,
  OUT UINT64  *ResponseAddress
  )
{
  if (mControlArea == NULL) {
    mControlArea = (EFI_TPM2_ACPI_CONTROL_AREA *)(UINTN)PcdGet64 (PcdTpmBaseAddress);
    DEBUG ((DEBUG_INFO, "AMD fTPM: CRB control area at %p\n", mControlArea));
  }

  *CommandSize = mControlArea->CommandSize;
  *CommandAddress = mControlArea->Command;
  *ResponseSize = mControlArea->ResponseSize;
  *ResponseAddress = mControlArea->Response;

  if ((*CommandSize == 0) || (*ResponseSize < sizeof (TPM2_RESPONSE_HEADER)) ||
      (*CommandAddress == 0) || (*ResponseAddress == 0) ||
      (*CommandAddress == MAX_UINT64) || (*ResponseAddress == MAX_UINT64))
  {
    DEBUG ((DEBUG_ERROR, "AMD fTPM: Invalid CRB buffer descriptors\n"));
    return EFI_DEVICE_ERROR;
  }

  return EFI_SUCCESS;
}

/**
  Wait for specific bits in a register to be set or cleared, with a timeout.

  @param Register   The memory-mapped register address to poll.
  @param BitSet     A bitmask of bits that must be set in the register.
  @param BitClear   A bitmask of bits that must be clear in the register.
  @param Timeout    The maximum time to wait, in microseconds.

  @retval EFI_SUCCESS        The specified bits are set/clear as required.
  @retval EFI_TIMEOUT        The timeout expired before the condition was met.
**/
STATIC
EFI_STATUS
WaitRegisterBits (
  IN UINTN   Register,
  IN UINT32  BitSet,
  IN UINT32  BitClear,
  IN UINT32  Timeout
  )
{
  UINT32  Elapsed;
  UINT32  Value;

  for (Elapsed = 0; Elapsed < Timeout; Elapsed += AMD_FTPM_POLL_PERIOD_US) {
    Value = MmioRead32 (Register);
    if (((Value & BitSet) == BitSet) && ((Value & BitClear) == 0)) {
      return EFI_SUCCESS;
    }

    MicroSecondDelay (AMD_FTPM_POLL_PERIOD_US);
  }

  DEBUG ((DEBUG_ERROR, "AMD fTPM: Timeout waiting for register 0x%lx\n", Register));
  return EFI_TIMEOUT;
}

/**
  Get the manufacture ID of the TPM device.

  @param ManufactureId  Pointer to a UINT32 to receive the manufacture ID.

  @retval EFI_SUCCESS        Manufacture ID was successfully retrieved.
  @retval EFI_DEVICE_ERROR   The device is present but not functional.
  @retval EFI_NOT_FOUND      The TPM2 ACPI table or control area was not found.
**/
STATIC
EFI_STATUS
Tpm2AMDfTPMGetManufactureId (
  OUT     UINT32                *ManufactureId
  )
{
  EFI_STATUS                    Status;
  TPM2_GET_CAPABILITY_COMMAND   SendBuffer;
  TPM2_GET_CAPABILITY_RESPONSE  RecvBuffer;
  UINT32                        SendBufferSize;
  UINT32                        RecvBufferSize;

  //
  // Construct command
  //
  SendBuffer.Header.tag         = SwapBytes16 (TPM_ST_NO_SESSIONS);
  SendBuffer.Header.commandCode = SwapBytes32 (TPM_CC_GetCapability);

  SendBuffer.Capability    = SwapBytes32 (TPM_CAP_TPM_PROPERTIES);
  SendBuffer.Property      = SwapBytes32 (TPM_PT_MANUFACTURER);
  SendBuffer.PropertyCount = SwapBytes32 (1);

  SendBufferSize              = (UINT32)sizeof (SendBuffer);
  SendBuffer.Header.paramSize = SwapBytes32 (SendBufferSize);

  //
  // send Tpm command
  //
  RecvBufferSize = sizeof (RecvBuffer);
  Status         = Tpm2AMDfTPMSubmitCommand (SendBufferSize, (UINT8 *)&SendBuffer, &RecvBufferSize, (UINT8 *)&RecvBuffer);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (RecvBufferSize <= sizeof (TPM2_RESPONSE_HEADER) + sizeof (UINT8)) {
    return EFI_DEVICE_ERROR;
  }

  //
  // Fail if command failed
  //
  if (SwapBytes32 (RecvBuffer.Header.responseCode) != TPM_RC_SUCCESS) {
    DEBUG ((DEBUG_ERROR, "Tpm2GetCapability: Response Code error! 0x%08x\r\n", SwapBytes32 (RecvBuffer.Header.responseCode)));
    return EFI_DEVICE_ERROR;
  }

  *ManufactureId = RecvBuffer.CapabilityData.data.tpmProperties.tpmProperty->value;

  return EFI_SUCCESS;
}

/**
  Probe for the presence of an AMD fTPM device and verify that it is functional.

  @retval EFI_SUCCESS        The AMD fTPM device is present and functional.
  @retval EFI_UNSUPPORTED    The platform is not AMD or the device is not an AMD fTPM.
  @retval EFI_DEVICE_ERROR   The device is present but not functional.
**/
EFI_STATUS
EFIAPI
Tpm2AMDfTPMProbe (
  VOID
)
{
  EFI_STATUS  Status;
  UINT32      CommandSize;
  UINT64      CommandAddress;
  UINT32      ResponseCapacity;
  UINT64      ResponseAddress;
  UINT32      ManufactureId;

  //
  // Probe the control area to ensure it is valid and accessible
  //
  Status = GetControlArea (
    &CommandSize,
    &CommandAddress,
    &ResponseCapacity,
    &ResponseAddress
    );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // Check if the manufacture ID matches AMD's ID (0x414D4400)
  //
  Status = Tpm2AMDfTPMGetManufactureId (
    &ManufactureId
    );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (ManufactureId != TPM2_MANUFACTURER_ID_AMD) {
    DEBUG ((DEBUG_INFO, "AMD fTPM: Manufacture ID mismatch 0x%x\n", ManufactureId));
    return EFI_UNSUPPORTED;
  }

  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
Tpm2AMDfTPMSubmitCommand (
  IN UINT32      InputParameterBlockSize,
  IN UINT8       *InputParameterBlock,
  IN OUT UINT32  *OutputParameterBlockSize,
  IN UINT8       *OutputParameterBlock
  )
{
  EFI_STATUS  Status;
  UINT32      CommandSize;
  UINT64      CommandAddress;
  UINT32      ResponseCapacity;
  UINT64      ResponseAddress;
  UINT32      ResponseSize;

  if ((InputParameterBlock == NULL) || (OutputParameterBlockSize == NULL) ||
      (OutputParameterBlock == NULL))
  {
    return EFI_INVALID_PARAMETER;
  }

  Status = GetControlArea (
             &CommandSize,
             &CommandAddress,
             &ResponseCapacity,
             &ResponseAddress
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (InputParameterBlockSize > CommandSize) {
    return EFI_BAD_BUFFER_SIZE;
  }

  Status = WaitRegisterBits (
             (UINTN)&mControlArea->Start,
             0,
             AMD_FTPM_START_COMMAND,
             AMD_FTPM_READY_TIMEOUT_US
             );
  if (EFI_ERROR (Status)) {
    return EFI_DEVICE_ERROR;
  }

  CopyMem ((VOID *)(UINTN)CommandAddress, InputParameterBlock, InputParameterBlockSize);
  ZeroMem ((VOID *)(UINTN)ResponseAddress, ResponseCapacity);
  MmioWrite32 ((UINTN)&mControlArea->ResponseSize, ResponseCapacity);
  MmioWrite32 ((UINTN)&mControlArea->Start, AMD_FTPM_START_COMMAND);

  Status = WaitRegisterBits (
             (UINTN)&mControlArea->Start,
             0,
             AMD_FTPM_START_COMMAND,
             AMD_FTPM_CMD_TIMEOUT_US
             );
  if (EFI_ERROR (Status)) {
    return EFI_DEVICE_ERROR;
  }

  if ((MmioRead32 ((UINTN)&mControlArea->Error) & AMD_FTPM_STATUS_ERROR) != 0) {
    DEBUG ((DEBUG_ERROR, "AMD fTPM: Command failed, status 0x%x\n",
            MmioRead32 ((UINTN)&mControlArea->Error)));
    return EFI_DEVICE_ERROR;
  }

  CopyMem (
    &ResponseSize,
    (UINT8 *)(UINTN)ResponseAddress + OFFSET_OF (TPM2_RESPONSE_HEADER, paramSize),
    sizeof (ResponseSize)
    );
  ResponseSize = SwapBytes32 (ResponseSize);
  if ((ResponseSize < sizeof (TPM2_RESPONSE_HEADER)) ||
      (ResponseSize > ResponseCapacity))
  {
    DEBUG ((DEBUG_ERROR, "AMD fTPM: Invalid response size 0x%x\n", ResponseSize));
    return EFI_DEVICE_ERROR;
  }

  if (*OutputParameterBlockSize < ResponseSize) {
    *OutputParameterBlockSize = ResponseSize;
    return EFI_BUFFER_TOO_SMALL;
  }

  CopyMem (OutputParameterBlock, (VOID *)(UINTN)ResponseAddress, ResponseSize);
  *OutputParameterBlockSize = ResponseSize;
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
Tpm2AMDfTPMRequestUseTpm (
  VOID
  )
{
  EFI_STATUS  Status;
  UINT32      CommandSize;
  UINT64      CommandAddress;
  UINT32      ResponseSize;
  UINT64      ResponseAddress;

  Status = GetControlArea (
             &CommandSize,
             &CommandAddress,
             &ResponseSize,
             &ResponseAddress
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return WaitRegisterBits (
           (UINTN)&mControlArea->Start,
           0,
           AMD_FTPM_START_COMMAND,
           AMD_FTPM_READY_TIMEOUT_US
           );
}

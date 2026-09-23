/** @file


  Copyright (c) 2021, Intel Corporation. All rights reserved.<BR>

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include "UefiPayloadEntry.h"

/**
  Parse the TPM2 ACPI table to determine if a dTPM or fTPM is present and if so,
  where the control area is located.

  @param[in]  Tpm2AcpiTable      Pointer to the TPM2 ACPI table.
  @param[out] AcpiBoardInfo      Pointer to the ACPI board info structure.
*/
STATIC
VOID
ParseTPM2AcpiInfo (
  IN   EFI_TPM2_ACPI_TABLE_V4  *Tpm2AcpiTable,
  OUT  ACPI_BOARD_INFO         *AcpiBoardInfo
  )
{
  UINT32                      CommandSize;
  UINT64                      CommandAddress;
  UINT32                      ResponseSize;
  UINT64                      ResponseAddress;
  EFI_TPM2_ACPI_CONTROL_AREA  *ControlArea;

  if ((Tpm2AcpiTable == NULL) || (AcpiBoardInfo == NULL)) {
    return;
  }

  if (Tpm2AcpiTable->Header.Signature != EFI_ACPI_5_0_TRUSTED_COMPUTING_PLATFORM_2_TABLE_SIGNATURE) {
    return;
  }

  if (Tpm2AcpiTable->Header.Length < sizeof (EFI_TPM2_ACPI_TABLE_V4)) {
    return;
  }

  AcpiBoardInfo->TPM20Present = 1;

  //
  // AMD fTPM is only supported on AMD platforms, so check the signature first
  //
  if (!StandardSignatureIsAuthenticAMD ()) {
    return;
  }

  //
  // Check that the control area address is valid.
  // Usually points to PSP MMIO space.
  //
  if ((Tpm2AcpiTable->AddressOfControlArea == 0) ||
      (Tpm2AcpiTable->AddressOfControlArea == MAX_UINT64) ||
      (Tpm2AcpiTable->AddressOfControlArea == 0xfed40000))
  {
    return;
  }

  //
  // AMD fTPM only supports the ACPI start method, so check that here.
  //
  if (Tpm2AcpiTable->StartMethod != EFI_TPM2_ACPI_TABLE_START_METHOD_ACPI) {
    return;
  }

  ControlArea = (EFI_TPM2_ACPI_CONTROL_AREA *)(UINTN)Tpm2AcpiTable->AddressOfControlArea;

  //
  // Ensure MMIO space is accessible and valid by reading the command and
  // response addresses and sizes. No TPM communication is done here, just probing
  // the MMIO space to ensure it is valid.
  //
  CommandSize     = ControlArea->CommandSize;
  CommandAddress  = ControlArea->Command;
  ResponseSize    = ControlArea->ResponseSize;
  ResponseAddress = ControlArea->Response;

  if ((CommandSize == 0) || (CommandSize == MAX_UINT32) ||
      (ResponseSize == MAX_UINT32) || (ResponseSize < sizeof (TPM2_RESPONSE_HEADER)) ||
      (CommandAddress == 0) || (ResponseAddress == 0) ||
      (CommandAddress == MAX_UINT64) || (ResponseAddress == MAX_UINT64))
  {
    return;
  }

  AcpiBoardInfo->TPM20Present             = 0;
  AcpiBoardInfo->AMDfTPMPresent           = 1;
  AcpiBoardInfo->Tpm2AddressOfControlArea = Tpm2AcpiTable->AddressOfControlArea;
}

/**
  Find the board related info from ACPI table

  @param  AcpiTableBase          ACPI table start address in memory
  @param  AcpiBoardInfo          Pointer to the acpi board info structure

  @retval RETURN_SUCCESS     Successfully find out all the required information.
  @retval RETURN_NOT_FOUND   Failed to find the required info.

**/
RETURN_STATUS
ParseAcpiInfo (
  IN   UINT64           AcpiTableBase,
  OUT  ACPI_BOARD_INFO  *AcpiBoardInfo
  )
{
  EFI_ACPI_3_0_ROOT_SYSTEM_DESCRIPTION_POINTER                                           *Rsdp;
  EFI_ACPI_DESCRIPTION_HEADER                                                            *Rsdt;
  UINT32                                                                                 *Entry32;
  UINTN                                                                                  Entry32Num;
  EFI_ACPI_3_0_FIXED_ACPI_DESCRIPTION_TABLE                                              *Fadt;
  EFI_ACPI_DESCRIPTION_HEADER                                                            *Xsdt;
  UINT64                                                                                 *Entry64;
  UINTN                                                                                  Entry64Num;
  UINTN                                                                                  Idx;
  UINT32                                                                                 *Signature;
  EFI_ACPI_MEMORY_MAPPED_CONFIGURATION_BASE_ADDRESS_TABLE_HEADER                         *MmCfgHdr;
  EFI_ACPI_MEMORY_MAPPED_ENHANCED_CONFIGURATION_SPACE_BASE_ADDRESS_ALLOCATION_STRUCTURE  *MmCfgBase;
  BOOLEAN                                                                                TCPATablePresent;
  EFI_TPM2_ACPI_TABLE_V4                                                                 *Tpm2AcpiTable;

  Rsdp = (EFI_ACPI_3_0_ROOT_SYSTEM_DESCRIPTION_POINTER *)(UINTN)AcpiTableBase;
  DEBUG ((DEBUG_INFO, "Rsdp at 0x%p\n", Rsdp));
  DEBUG ((DEBUG_INFO, "Rsdt at 0x%x, Xsdt at 0x%lx\n", Rsdp->RsdtAddress, Rsdp->XsdtAddress));

  //
  // Search Rsdt First
  //
  Fadt             = NULL;
  MmCfgHdr         = NULL;
  Tpm2AcpiTable    = NULL;
  TCPATablePresent = FALSE;
  Rsdt             = (EFI_ACPI_DESCRIPTION_HEADER *)(UINTN)(Rsdp->RsdtAddress);
  if (Rsdt != NULL) {
    Entry32    = (UINT32 *)(Rsdt + 1);
    Entry32Num = (Rsdt->Length - sizeof (EFI_ACPI_DESCRIPTION_HEADER)) >> 2;
    for (Idx = 0; Idx < Entry32Num; Idx++) {
      Signature = (UINT32 *)(UINTN)Entry32[Idx];
      if (*Signature == EFI_ACPI_3_0_FIXED_ACPI_DESCRIPTION_TABLE_SIGNATURE) {
        Fadt = (EFI_ACPI_3_0_FIXED_ACPI_DESCRIPTION_TABLE *)Signature;
        DEBUG ((DEBUG_INFO, "Found Fadt in Rsdt\n"));
      }

      if (*Signature == EFI_ACPI_5_0_PCI_EXPRESS_MEMORY_MAPPED_CONFIGURATION_SPACE_BASE_ADDRESS_DESCRIPTION_TABLE_SIGNATURE) {
        MmCfgHdr = (EFI_ACPI_MEMORY_MAPPED_CONFIGURATION_BASE_ADDRESS_TABLE_HEADER *)Signature;
        DEBUG ((DEBUG_INFO, "Found MM config address in Rsdt\n"));
      }

      if (*Signature == EFI_ACPI_5_0_TRUSTED_COMPUTING_PLATFORM_2_TABLE_SIGNATURE) {
        Tpm2AcpiTable = (EFI_TPM2_ACPI_TABLE_V4 *)(UINTN)Entry32[Idx];
      }

      if (*Signature == EFI_ACPI_5_0_TRUSTED_COMPUTING_PLATFORM_ALLIANCE_CAPABILITIES_TABLE_SIGNATURE) {
        TCPATablePresent = TRUE;
      }

      if ((Fadt != NULL) && (MmCfgHdr != NULL) && ((Tpm2AcpiTable != NULL) || TCPATablePresent)) {
        goto TpmDectectDone;
      }
    }
  }

  //
  // Search Xsdt Second
  //
  Xsdt = (EFI_ACPI_DESCRIPTION_HEADER *)(UINTN)(Rsdp->XsdtAddress);
  if (Xsdt != NULL) {
    Entry64    = (UINT64 *)(Xsdt + 1);
    Entry64Num = (Xsdt->Length - sizeof (EFI_ACPI_DESCRIPTION_HEADER)) >> 3;
    for (Idx = 0; Idx < Entry64Num; Idx++) {
      Signature = (UINT32 *)(UINTN)ReadUnaligned64 (&Entry64[Idx]);
      if (*Signature == EFI_ACPI_3_0_FIXED_ACPI_DESCRIPTION_TABLE_SIGNATURE) {
        Fadt = (EFI_ACPI_3_0_FIXED_ACPI_DESCRIPTION_TABLE *)Signature;
        DEBUG ((DEBUG_INFO, "Found Fadt in Xsdt\n"));
      }

      if (*Signature == EFI_ACPI_5_0_PCI_EXPRESS_MEMORY_MAPPED_CONFIGURATION_SPACE_BASE_ADDRESS_DESCRIPTION_TABLE_SIGNATURE) {
        MmCfgHdr = (EFI_ACPI_MEMORY_MAPPED_CONFIGURATION_BASE_ADDRESS_TABLE_HEADER *)Signature;
        DEBUG ((DEBUG_INFO, "Found MM config address in Xsdt\n"));
      }

      if (*Signature == EFI_ACPI_5_0_TRUSTED_COMPUTING_PLATFORM_2_TABLE_SIGNATURE) {
        Tpm2AcpiTable = (EFI_TPM2_ACPI_TABLE_V4 *)(UINTN)Entry32[Idx];
      }

      if (*Signature == EFI_ACPI_5_0_TRUSTED_COMPUTING_PLATFORM_ALLIANCE_CAPABILITIES_TABLE_SIGNATURE) {
        TCPATablePresent = TRUE;
      }

      if ((Fadt != NULL) && (MmCfgHdr != NULL) && ((Tpm2AcpiTable != NULL) || TCPATablePresent)) {
        goto TpmDectectDone;
      }
    }
  }

  if (Fadt == NULL) {
    return RETURN_NOT_FOUND;
  }

TpmDectectDone:
  if (Tpm2AcpiTable != NULL) {
    ParseTPM2AcpiInfo (Tpm2AcpiTable, AcpiBoardInfo);
  }

  AcpiBoardInfo->TPM12Present = TCPATablePresent;

  AcpiBoardInfo->PmCtrlRegBase   = Fadt->Pm1aCntBlk;
  AcpiBoardInfo->PmTimerRegBase  = Fadt->PmTmrBlk;
  AcpiBoardInfo->ResetRegAddress = Fadt->ResetReg.Address;
  AcpiBoardInfo->ResetValue      = Fadt->ResetValue;
  AcpiBoardInfo->PmEvtBase       = Fadt->Pm1aEvtBlk;
  AcpiBoardInfo->PmGpeEnBase     = Fadt->Gpe0Blk + Fadt->Gpe0BlkLen / 2;

  if (MmCfgHdr != NULL) {
    MmCfgBase                      = (EFI_ACPI_MEMORY_MAPPED_ENHANCED_CONFIGURATION_SPACE_BASE_ADDRESS_ALLOCATION_STRUCTURE *)((UINT8 *)MmCfgHdr + sizeof (*MmCfgHdr));
    AcpiBoardInfo->PcieBaseAddress = MmCfgBase->BaseAddress;
    AcpiBoardInfo->PcieBaseSize    = (MmCfgBase->EndBusNumber + 1 - MmCfgBase->StartBusNumber) * 4096 * 32 * 8;
  } else {
    AcpiBoardInfo->PcieBaseAddress = 0;
    AcpiBoardInfo->PcieBaseSize    = 0;
  }

  DEBUG ((DEBUG_INFO, "PmCtrl  Reg     0x%lx\n", AcpiBoardInfo->PmCtrlRegBase));
  DEBUG ((DEBUG_INFO, "PmTimer Reg     0x%lx\n", AcpiBoardInfo->PmTimerRegBase));
  DEBUG ((DEBUG_INFO, "Reset   Reg     0x%lx\n", AcpiBoardInfo->ResetRegAddress));
  DEBUG ((DEBUG_INFO, "Reset   Value   0x%x\n", AcpiBoardInfo->ResetValue));
  DEBUG ((DEBUG_INFO, "PmEvt   Reg     0x%lx\n", AcpiBoardInfo->PmEvtBase));
  DEBUG ((DEBUG_INFO, "PmGpeEn Reg     0x%lx\n", AcpiBoardInfo->PmGpeEnBase));
  DEBUG ((DEBUG_INFO, "PcieBaseAddr    0x%lx\n", AcpiBoardInfo->PcieBaseAddress));
  DEBUG ((DEBUG_INFO, "PcieBaseSize    0x%lx\n", AcpiBoardInfo->PcieBaseSize));
  DEBUG ((DEBUG_INFO, "TPM 2.0 present %x\n", AcpiBoardInfo->TPM20Present));
  DEBUG ((DEBUG_INFO, "AMDfTPM present %x\n", AcpiBoardInfo->AMDfTPMPresent));
  DEBUG ((DEBUG_INFO, "TPM 1.2 present %x\n", AcpiBoardInfo->TPM12Present));

  return RETURN_SUCCESS;
}

/**
  Build ACPI board info HOB using infomation from ACPI table

  @param  AcpiTableBase      ACPI table start address in memory

  @retval  A pointer to ACPI board HOB ACPI_BOARD_INFO. Null if build HOB failure.
**/
ACPI_BOARD_INFO *
BuildHobFromAcpi (
  IN   UINT64  AcpiTableBase
  )
{
  EFI_STATUS       Status;
  ACPI_BOARD_INFO  AcpiBoardInfo;
  ACPI_BOARD_INFO  *NewAcpiBoardInfo;

  NewAcpiBoardInfo = NULL;
  Status           = ParseAcpiInfo (AcpiTableBase, &AcpiBoardInfo);
  ASSERT_EFI_ERROR (Status);
  if (!EFI_ERROR (Status)) {
    NewAcpiBoardInfo = BuildGuidHob (&gUefiAcpiBoardInfoGuid, sizeof (ACPI_BOARD_INFO));
    ASSERT (NewAcpiBoardInfo != NULL);
    CopyMem (NewAcpiBoardInfo, &AcpiBoardInfo, sizeof (ACPI_BOARD_INFO));
    DEBUG ((DEBUG_INFO, "Create acpi board info guid hob\n"));
  }

  return NewAcpiBoardInfo;
}

/** @file
  This file defines the hob structure for board related information from acpi table

  Copyright (c) 2014, Intel Corporation. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#pragma once

///
/// Board information GUID
///
extern EFI_GUID  gUefiAcpiBoardInfoGuid;

typedef struct {
  UINT8     Revision;
  UINT8     Reserved0[2];
  UINT8     ResetValue;
  UINT64    PmEvtBase;
  UINT64    PmGpeEnBase;
  UINT64    PmCtrlRegBase;
  UINT64    PmTimerRegBase;
  UINT64    ResetRegAddress;
  UINT64    PcieBaseAddress;
  UINT64    PcieBaseSize;
  UINT64    Tpm2AddressOfControlArea;
  UINT8     TPM20Present;
  UINT8     AMDfTPMPresent;
  UINT8     TPM12Present;
} ACPI_BOARD_INFO;

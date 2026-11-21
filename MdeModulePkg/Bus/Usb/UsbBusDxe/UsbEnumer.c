/** @file

    Usb bus enumeration support.

Copyright (c) 2007 - 2018, Intel Corporation. All rights reserved.<BR>
SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include "UsbBus.h"

STATIC
EFI_STATUS
UsbAsyncEnumConfigureLater (
  IN USB_INTERFACE  *HubIf,
  IN UINT8          Port,
  IN USB_DEVICE     *Child,
  IN UINT8          RetryCount
  );

/**
  Return the endpoint descriptor in this interface.

  @param  UsbIf                 The interface to search in.
  @param  EpAddr                The address of the endpoint to return.

  @return The endpoint descriptor or NULL.

**/
USB_ENDPOINT_DESC *
UsbGetEndpointDesc (
  IN USB_INTERFACE  *UsbIf,
  IN UINT8          EpAddr
  )
{
  USB_ENDPOINT_DESC  *EpDesc;
  UINT8              Index;
  UINT8              NumEndpoints;

  NumEndpoints = UsbIf->IfSetting->Desc.NumEndpoints;

  for (Index = 0; Index < NumEndpoints; Index++) {
    EpDesc = UsbIf->IfSetting->Endpoints[Index];

    if (EpDesc->Desc.EndpointAddress == EpAddr) {
      return EpDesc;
    }
  }

  return NULL;
}

/**
  Free the resource used by USB interface.

  @param  UsbIf                 The USB interface to free.

  @retval EFI_ACCESS_DENIED     The interface is still occupied.
  @retval EFI_SUCCESS           The interface is freed.
**/
EFI_STATUS
UsbFreeInterface (
  IN USB_INTERFACE  *UsbIf
  )
{
  EFI_STATUS  Status;

  UsbCloseHostProtoByChild (UsbIf->Device->Bus, UsbIf->Handle);

  Status = gBS->UninstallMultipleProtocolInterfaces (
                  UsbIf->Handle,
                  &gEfiDevicePathProtocolGuid,
                  UsbIf->DevicePath,
                  &gEfiUsbIoProtocolGuid,
                  &UsbIf->UsbIo,
                  NULL
                  );
  if (!EFI_ERROR (Status)) {
    if (UsbIf->DevicePath != NULL) {
      FreePool (UsbIf->DevicePath);
    }

    FreePool (UsbIf);
  } else {
    UsbOpenHostProtoByChild (UsbIf->Device->Bus, UsbIf->Handle);
  }

  return Status;
}

/**
  Create an interface for the descriptor IfDesc. Each
  device's configuration can have several interfaces.

  @param  Device                The device has the interface descriptor.
  @param  IfDesc                The interface descriptor.

  @return The created USB interface for the descriptor, or NULL.

**/
USB_INTERFACE *
UsbCreateInterface (
  IN USB_DEVICE          *Device,
  IN USB_INTERFACE_DESC  *IfDesc
  )
{
  USB_DEVICE_PATH  UsbNode;
  USB_INTERFACE    *UsbIf;
  USB_INTERFACE    *HubIf;
  EFI_STATUS       Status;

  UsbIf = AllocateZeroPool (sizeof (USB_INTERFACE));

  if (UsbIf == NULL) {
    return NULL;
  }

  UsbIf->Signature = USB_INTERFACE_SIGNATURE;
  UsbIf->Device    = Device;
  UsbIf->IfDesc    = IfDesc;
  ASSERT (IfDesc->ActiveIndex < USB_MAX_INTERFACE_SETTING);
  UsbIf->IfSetting = IfDesc->Settings[IfDesc->ActiveIndex];

  CopyMem (
    &(UsbIf->UsbIo),
    &mUsbIoProtocol,
    sizeof (EFI_USB_IO_PROTOCOL)
    );

  //
  // Install protocols for USBIO and device path
  //
  UsbNode.Header.Type      = MESSAGING_DEVICE_PATH;
  UsbNode.Header.SubType   = MSG_USB_DP;
  UsbNode.ParentPortNumber = Device->ParentPort;
  UsbNode.InterfaceNumber  = UsbIf->IfSetting->Desc.InterfaceNumber;

  SetDevicePathNodeLength (&UsbNode.Header, sizeof (UsbNode));

  HubIf = Device->ParentIf;
  ASSERT (HubIf != NULL);

  UsbIf->DevicePath = AppendDevicePathNode (HubIf->DevicePath, &UsbNode.Header);

  if (UsbIf->DevicePath == NULL) {
    DEBUG ((DEBUG_ERROR, "UsbCreateInterface: failed to create device path\n"));

    Status = EFI_OUT_OF_RESOURCES;
    goto ON_ERROR;
  }

  Status = gBS->InstallMultipleProtocolInterfaces (
                  &UsbIf->Handle,
                  &gEfiDevicePathProtocolGuid,
                  UsbIf->DevicePath,
                  &gEfiUsbIoProtocolGuid,
                  &UsbIf->UsbIo,
                  NULL
                  );

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "UsbCreateInterface: failed to install UsbIo - %r\n", Status));
    goto ON_ERROR;
  }

  //
  // Open USB Host Controller Protocol by Child
  //
  Status = UsbOpenHostProtoByChild (Device->Bus, UsbIf->Handle);

  if (EFI_ERROR (Status)) {
    gBS->UninstallMultipleProtocolInterfaces (
           UsbIf->Handle,
           &gEfiDevicePathProtocolGuid,
           UsbIf->DevicePath,
           &gEfiUsbIoProtocolGuid,
           &UsbIf->UsbIo,
           NULL
           );

    DEBUG ((DEBUG_ERROR, "UsbCreateInterface: failed to open host for child - %r\n", Status));
    goto ON_ERROR;
  }

  return UsbIf;

ON_ERROR:
  if (UsbIf->DevicePath != NULL) {
    FreePool (UsbIf->DevicePath);
  }

  FreePool (UsbIf);
  return NULL;
}

/**
  Free the resource used by this USB device.

  @param  Device                The USB device to free.

**/
VOID
UsbFreeDevice (
  IN USB_DEVICE  *Device
  )
{
  if (Device->DevDesc != NULL) {
    UsbFreeDevDesc (Device->DevDesc);
  }

  gBS->FreePool (Device);
}

/**
  Create a device which is on the parent's ParentPort port.

  @param  ParentIf              The parent HUB interface.
  @param  ParentPort            The port on the HUB this device is connected to.

  @return Created USB device, Or NULL.

**/
USB_DEVICE *
UsbCreateDevice (
  IN USB_INTERFACE  *ParentIf,
  IN UINT8          ParentPort
  )
{
  USB_DEVICE  *Device;

  ASSERT (ParentIf != NULL);

  Device = AllocateZeroPool (sizeof (USB_DEVICE));

  if (Device == NULL) {
    return NULL;
  }

  Device->Bus        = ParentIf->Device->Bus;
  Device->MaxPacket0 = 8;
  Device->ParentAddr = ParentIf->Device->Address;
  Device->ParentIf   = ParentIf;
  Device->ParentPort = ParentPort;
  Device->Tier       = (UINT8)(ParentIf->Device->Tier + 1);
  Device->EnumScript = 0;
  Device->IsSSDev    = FALSE;
  return Device;
}

/**
  Connect the USB interface with its driver. EFI USB bus will
  create a USB interface for each separate interface descriptor.

  @param  UsbIf             The interface to connect driver to.

  @return EFI_SUCCESS       Interface is managed by some driver.
  @return Others            Failed to locate a driver for this interface.

**/
EFI_STATUS
UsbConnectDriver (
  IN USB_INTERFACE  *UsbIf
  )
{
  EFI_STATUS  Status;
  EFI_TPL     OldTpl;

  Status = EFI_SUCCESS;

  //
  // Hub is maintained by the USB bus driver. Otherwise try to
  // connect drivers with this interface
  //
  if (UsbIsHubInterface (UsbIf)) {
    DEBUG ((DEBUG_INFO, "UsbConnectDriver: found a hub device\n"));
    Status = mUsbHubApi.Init (UsbIf);
  } else {
    //
    // This function is called in both UsbIoControlTransfer and
    // the timer callback in hub enumeration. So, at least it is
    // called at TPL_CALLBACK. Some driver sitting on USB has
    // twisted TPL used. It should be no problem for us to connect
    // or disconnect at CALLBACK.
    //

    //
    // Only recursively wanted usb child device
    //
    if (UsbBusIsWantedUsbIO (UsbIf->Device->Bus, UsbIf)) {
      OldTpl = UsbGetCurrentTpl ();
      DEBUG ((DEBUG_INFO, "UsbConnectDriver: TPL before connect is %d, %p\n", (UINT32)OldTpl, UsbIf->Handle));

      gBS->RestoreTPL (TPL_CALLBACK);

      USB_TS ((DEBUG_INFO, "UsbConnectDriver: ConnectController %p\n", UsbIf->Handle));
      Status           = gBS->ConnectController (UsbIf->Handle, NULL, NULL, TRUE);
      USB_TS ((DEBUG_INFO, "UsbConnectDriver: ConnectController %p done - %r\n", UsbIf->Handle, Status));
      UsbIf->IsManaged = (BOOLEAN) !EFI_ERROR (Status);

      DEBUG ((DEBUG_INFO, "UsbConnectDriver: TPL after connect is %d\n", (UINT32)UsbGetCurrentTpl ()));
      ASSERT (UsbGetCurrentTpl () == TPL_CALLBACK);

      gBS->RaiseTPL (OldTpl);
    }
  }

  return Status;
}

/**
  Select an alternate setting for the interface.
  Each interface can have several mutually exclusive
  settings. Only one setting is active. It will
  also reset its endpoints' toggle to zero.

  @param  IfDesc                The interface descriptor to set.
  @param  Alternate             The alternate setting number to locate.

  @retval EFI_NOT_FOUND         There is no setting with this alternate index.
  @retval EFI_SUCCESS           The interface is set to Alternate setting.

**/
EFI_STATUS
UsbSelectSetting (
  IN USB_INTERFACE_DESC  *IfDesc,
  IN UINT8               Alternate
  )
{
  USB_INTERFACE_SETTING  *Setting;
  UINTN                  Index;

  //
  // Locate the active alternate setting
  //
  Setting = NULL;

  for (Index = 0; Index < IfDesc->NumOfSetting; Index++) {
    ASSERT (Index < USB_MAX_INTERFACE_SETTING);
    Setting = IfDesc->Settings[Index];

    if (Setting->Desc.AlternateSetting == Alternate) {
      break;
    }
  }

  if ((Index == IfDesc->NumOfSetting) || (Setting == NULL)) {
    return EFI_NOT_FOUND;
  }

  IfDesc->ActiveIndex = Index;

  ASSERT (Setting != NULL);
  DEBUG ((
    DEBUG_INFO,
    "UsbSelectSetting: setting %d selected for interface %d\n",
    Alternate,
    Setting->Desc.InterfaceNumber
    ));

  //
  // Reset the endpoint toggle to zero
  //
  for (Index = 0; Index < Setting->Desc.NumEndpoints; Index++) {
    Setting->Endpoints[Index]->Toggle = 0;
  }

  return EFI_SUCCESS;
}

/**
  Select a new configuration for the device. Each
  device may support several configurations.

  @param  Device                The device to select configuration.
  @param  ConfigValue           The index of the configuration ( != 0).

  @retval EFI_NOT_FOUND         There is no configuration with the index.
  @retval EFI_OUT_OF_RESOURCES  Failed to allocate resource.
  @retval EFI_SUCCESS           The configuration is selected.

**/
EFI_STATUS
UsbSelectConfig (
  IN USB_DEVICE  *Device,
  IN UINT8       ConfigValue
  )
{
  USB_DEVICE_DESC     *DevDesc;
  USB_CONFIG_DESC     *ConfigDesc;
  USB_INTERFACE_DESC  *IfDesc;
  USB_INTERFACE       *UsbIf;
  EFI_STATUS          Status;
  UINT8               Index;

  //
  // Locate the active config, then set the device's pointer
  //
  DevDesc    = Device->DevDesc;
  ConfigDesc = NULL;

  for (Index = 0; Index < DevDesc->Desc.NumConfigurations; Index++) {
    ConfigDesc = DevDesc->Configs[Index];

    if (ConfigDesc->Desc.ConfigurationValue == ConfigValue) {
      break;
    }
  }

  if ((Index == DevDesc->Desc.NumConfigurations) || (ConfigDesc == NULL)) {
    return EFI_NOT_FOUND;
  }

  Device->ActiveConfig = ConfigDesc;

  DEBUG ((
    DEBUG_INFO,
    "UsbSelectConfig: config %d selected for device %d\n",
    ConfigValue,
    Device->Address
    ));

  //
  // Create interfaces for each USB interface descriptor.
  //
  for (Index = 0; Index < ConfigDesc->Desc.NumInterfaces; Index++) {
    //
    // First select the default interface setting, and reset
    // the endpoint toggles to zero for its endpoints.
    //
    IfDesc = ConfigDesc->Interfaces[Index];
    UsbSelectSetting (IfDesc, IfDesc->Settings[0]->Desc.AlternateSetting);

    //
    // Create a USB_INTERFACE and install USB_IO and other protocols
    //
    UsbIf = UsbCreateInterface (Device, ConfigDesc->Interfaces[Index]);

    if (UsbIf == NULL) {
      Device->NumOfInterface = Index;
      return EFI_OUT_OF_RESOURCES;
    }

    ASSERT (Index < USB_MAX_INTERFACE);
    Device->Interfaces[Index] = UsbIf;

    //
    // Connect the device to drivers, if it failed, ignore
    // the error. Don't let the unsupported interfaces to block
    // the supported interfaces.
    //
    Status = UsbConnectDriver (UsbIf);

    if (EFI_ERROR (Status)) {
      DEBUG ((
        DEBUG_WARN,
        "UsbSelectConfig: failed to connect driver - %r, ignored\n",
        Status
        ));
    }
  }

  Device->NumOfInterface = Index;

  return EFI_SUCCESS;
}

/**
  Disconnect the USB interface with its driver.

  @param  UsbIf                 The interface to disconnect driver from.

**/
EFI_STATUS
UsbDisconnectDriver (
  IN USB_INTERFACE  *UsbIf
  )
{
  EFI_TPL     OldTpl;
  EFI_STATUS  Status;

  //
  // Release the hub if it's a hub controller, otherwise
  // disconnect the driver if it is managed by other drivers.
  //
  Status = EFI_SUCCESS;
  if (UsbIf->IsHub) {
    Status = UsbIf->HubApi->Release (UsbIf);
  } else if (UsbIf->IsManaged) {
    //
    // This function is called in both UsbIoControlTransfer and
    // the timer callback in hub enumeration. So, at least it is
    // called at TPL_CALLBACK. Some driver sitting on USB has
    // twisted TPL used. It should be no problem for us to connect
    // or disconnect at CALLBACK.
    //
    OldTpl = UsbGetCurrentTpl ();
    DEBUG ((DEBUG_INFO, "UsbDisconnectDriver: old TPL is %d, %p\n", (UINT32)OldTpl, UsbIf->Handle));

    gBS->RestoreTPL (TPL_CALLBACK);

    Status = gBS->DisconnectController (UsbIf->Handle, NULL, NULL);
    if (!EFI_ERROR (Status)) {
      UsbIf->IsManaged = FALSE;
    }

    DEBUG ((DEBUG_INFO, "UsbDisconnectDriver: TPL after disconnect is %d, %d\n", (UINT32)UsbGetCurrentTpl (), Status));
    ASSERT (UsbGetCurrentTpl () == TPL_CALLBACK);

    gBS->RaiseTPL (OldTpl);
  }

  return Status;
}

/**
  Remove the current device configuration.

  @param  Device                The USB device to remove configuration from.

**/
EFI_STATUS
UsbRemoveConfig (
  IN USB_DEVICE  *Device
  )
{
  USB_INTERFACE  *UsbIf;
  UINTN          Index;
  EFI_STATUS     Status;
  EFI_STATUS     ReturnStatus;

  //
  // Remove each interface of the device
  //
  ReturnStatus = EFI_SUCCESS;
  for (Index = 0; Index < Device->NumOfInterface; Index++) {
    ASSERT (Index < USB_MAX_INTERFACE);
    UsbIf = Device->Interfaces[Index];

    if (UsbIf == NULL) {
      continue;
    }

    Status = UsbDisconnectDriver (UsbIf);
    if (!EFI_ERROR (Status)) {
      Status = UsbFreeInterface (UsbIf);
      if (EFI_ERROR (Status)) {
        UsbConnectDriver (UsbIf);
      }
    }

    if (!EFI_ERROR (Status)) {
      Device->Interfaces[Index] = NULL;
    } else {
      ReturnStatus = Status;
    }
  }

  Device->ActiveConfig = NULL;
  return ReturnStatus;
}

/**
  Remove the device and all its children from the bus.

  @param  Device                The device to remove.

  @retval EFI_SUCCESS           The device is removed.

**/
EFI_STATUS
UsbRemoveDevice (
  IN USB_DEVICE  *Device
  )
{
  USB_BUS     *Bus;
  USB_DEVICE  *Child;
  EFI_STATUS  Status;
  EFI_STATUS  ReturnStatus;
  UINTN       Index;

  Bus = Device->Bus;

  //
  // Remove all the devices on its downstream ports. Search from devices[1].
  // Devices[0] is the root hub.
  //
  ReturnStatus = EFI_SUCCESS;
  for (Index = 1; Index < Bus->MaxDevices; Index++) {
    Child = Bus->Devices[Index];

    if ((Child == NULL) || (Child->ParentAddr != Device->Address)) {
      continue;
    }

    Status = UsbRemoveDevice (Child);

    if (!EFI_ERROR (Status)) {
      Bus->Devices[Index] = NULL;
    } else {
      Bus->Devices[Index]->DisconnectFail = TRUE;
      ReturnStatus                        = Status;
      DEBUG ((DEBUG_INFO, "UsbRemoveDevice: failed to remove child %p at parent %p\n", Child, Device));
    }
  }

  if (EFI_ERROR (ReturnStatus)) {
    return ReturnStatus;
  }

  Status = UsbRemoveConfig (Device);

  if (!EFI_ERROR (Status)) {
    DEBUG ((DEBUG_INFO, "UsbRemoveDevice: device %d removed\n", Device->Address));

    ASSERT (Device->Address < Bus->MaxDevices);
    Bus->Devices[Device->Address] = NULL;
    UsbFreeDevice (Device);
  } else {
    Bus->Devices[Device->Address]->DisconnectFail = TRUE;
  }

  return Status;
}

/**
  Find the child device on the hub's port.

  @param  HubIf                 The hub interface.
  @param  Port                  The port of the hub this child is connected to.

  @return The device on the hub's port, or NULL if there is none.

**/
USB_DEVICE *
UsbFindChild (
  IN USB_INTERFACE  *HubIf,
  IN UINT8          Port
  )
{
  USB_DEVICE  *Device;
  USB_BUS     *Bus;
  UINTN       Index;

  Bus = HubIf->Device->Bus;

  //
  // Start checking from device 1, device 0 is the root hub
  //
  for (Index = 1; Index < Bus->MaxDevices; Index++) {
    Device = Bus->Devices[Index];

    if ((Device != NULL) && (Device->ParentAddr == HubIf->Device->Address) &&
        (Device->ParentPort == Port))
    {
      return Device;
    }
  }

  return NULL;
}

/**
  Enumerate and configure the new device on the port of this HUB interface.

  @param  HubIf                 The HUB that has the device connected.
  @param  Port                  The port index of the hub (started with zero).
  @param  ResetIsNeeded         The boolean to control whether skip the reset of the port.

  @retval EFI_SUCCESS           The device is enumerated (added or removed).
  @retval EFI_OUT_OF_RESOURCES  Failed to allocate resource for the device.
  @retval Others                Failed to enumerate the device.

**/
EFI_STATUS
UsbEnumerateNewDev (
  IN USB_INTERFACE  *HubIf,
  IN UINT8          Port,
  IN BOOLEAN        ResetIsNeeded
  )
{
  USB_STALL (USB_WAIT_PORT_STABLE_STALL);

  return UsbEnumerateStableDev (HubIf, Port, ResetIsNeeded ? UsbPortResetFull : UsbPortResetSkip, NULL, 0, FALSE);
}

/**
  Enumerate and configure the new device on the port of this HUB interface
  after the port connection has been stable for USB_WAIT_PORT_STABLE_STALL.

  The time between the end of the port reset and SET_ADDRESS is spent
  without returning to the caller, so that no two devices on the same bus
  are listening on the default address at the same time.

  @param  HubIf                 The HUB that has the device connected.
  @param  Port                  The port index of the hub (started with zero).
  @param  ResetMode             How to reset the port.
  @param  ResumeChild           The addressed device to configure, or NULL to
                                start the enumeration.
  @param  ResumeRetryCount      The retry count of ResumeChild.
  @param  AllowYield            Whether the configuration of the device may be
                                continued in a timer callback once it is
                                addressed.

  @retval EFI_SUCCESS           The device is enumerated (added or removed).
  @retval EFI_NOT_READY         The device is addressed and will be configured
                                in a timer callback.
  @retval EFI_OUT_OF_RESOURCES  Failed to allocate resource for the device.
  @retval Others                Failed to enumerate the device.

**/
EFI_STATUS
UsbEnumerateStableDev (
  IN USB_INTERFACE        *HubIf,
  IN UINT8                Port,
  IN USB_PORT_RESET_MODE  ResetMode,
  IN USB_DEVICE           *ResumeChild  OPTIONAL,
  IN UINT8                ResumeRetryCount,
  IN BOOLEAN              AllowYield
  )
{
  USB_BUS              *Bus;
  USB_HUB_API          *HubApi;
  USB_DEVICE           *Child;
  USB_DEVICE           *Parent;
  EFI_USB_PORT_STATUS  PortState;
  UINTN                Address;
  UINT8                Config;
  EFI_STATUS           Status;
  EFI_STATUS           OriginalStatus;
  UINT8                RetryCount;

  Parent  = HubIf->Device;
  Bus     = Parent->Bus;
  HubApi  = HubIf->HubApi;
  Address = Bus->MaxDevices;

  if (ResumeChild != NULL) {
    Child      = ResumeChild;
    RetryCount = ResumeRetryCount;
    Address    = Child->Address;
    goto DeviceConfigure;
  }

  USB_TS ((DEBUG_INFO, "UsbEnumerateNewDev: enter hub %p port %d\n", HubIf, Port));

  //
  // Hub resets the device for at least 10 milliseconds.
  // Host learns device speed. If device is of low/full speed
  // and the hub is a EHCI root hub, ResetPort will release
  // the device to its companion UHCI and return an error.
  //
  if (ResetMode != UsbPortResetSkip) {
    if (ResetMode == UsbPortResetRootFinish) {
      Status = UsbRootHubResetPortFinish (HubIf, Port);
    } else {
      Status = HubApi->ResetPort (HubIf, Port);
    }

    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "UsbEnumerateNewDev: failed to reset port %d - %r\n", Port, Status));

      return Status;
    }

    DEBUG ((DEBUG_INFO, "UsbEnumerateNewDev: hub port %d is reset\n", Port));
  } else {
    DEBUG ((DEBUG_INFO, "UsbEnumerateNewDev: hub port %d reset is skipped\n", Port));
  }

  Child = UsbCreateDevice (HubIf, Port);

  if (Child == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  RetryCount = 3;

DeviceRetry:
  //
  // OK, now identify the device speed. After reset, hub
  // fully knows the actual device speed.
  //
  Status = HubApi->GetPortStatus (HubIf, Port, &PortState);

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "UsbEnumerateNewDev: failed to get speed of port %d\n", Port));
    goto ON_ERROR;
  }

  //
  // RetryCount is used to execute the different enum scripts.
  // Current the diffrence is only for getting Configuration Descriptor.
  // +--------------------+---+----------------------------------------------------+
  // | UsbEnumScriptWin   | 3 | UsbGetOneConfig get the descriptor with 255 bytes  |
  // +--------------------+---+----------------------------------------------------+
  // | UsbEnumScriptLinux | 2 | UsbGetOneConfig get the descriptor with            |
  // |                    |   | ConfigurationDescriptor Length.                    |
  // +--------------------+---+----------------------------------------------------+
  // | UsbEnumScriptRsrv  | 1 | UsbGetOneConfig get the descriptor with 8 bytes    |
  // |                    |   | and then get the total length. (Same as Edk2 flow) |
  // +--------------------+---+----------------------------------------------------+
  // | UsbEnumScriptEdk2  | 0 | UsbGetOneConfig get the descriptor with 8 bytes    |
  // |                    |   | and then get the total length.                     |
  // +--------------------+--------------------------------------------------------+
  //
  Child->EnumScript = RetryCount;

  if (!USB_BIT_IS_SET (PortState.PortStatus, USB_PORT_STAT_CONNECTION)) {
    DEBUG ((DEBUG_ERROR, "UsbEnumerateNewDev: No device present at port %d\n", Port));
    Status     = EFI_NOT_FOUND;
    RetryCount = 0;
    goto ON_ERROR;
  } else if (USB_BIT_IS_SET (PortState.PortStatus, USB_PORT_STAT_SUPER_SPEED)) {
    Child->Speed      = EFI_USB_SPEED_SUPER;
    Child->MaxPacket0 = 512;
  } else if (USB_BIT_IS_SET (PortState.PortStatus, USB_PORT_STAT_HIGH_SPEED)) {
    Child->Speed      = EFI_USB_SPEED_HIGH;
    Child->MaxPacket0 = 64;
  } else if (USB_BIT_IS_SET (PortState.PortStatus, USB_PORT_STAT_LOW_SPEED)) {
    Child->Speed      = EFI_USB_SPEED_LOW;
    Child->MaxPacket0 = 8;
  } else {
    Child->Speed      = EFI_USB_SPEED_FULL;
    Child->MaxPacket0 = 8;
  }

  DEBUG ((DEBUG_INFO, "UsbEnumerateNewDev: device is of %d speed\n", Child->Speed));

  if (((Child->Speed == EFI_USB_SPEED_LOW) || (Child->Speed == EFI_USB_SPEED_FULL)) &&
      (Parent->Speed == EFI_USB_SPEED_HIGH))
  {
    //
    // If the child is a low or full speed device, it is necessary to
    // set the transaction translator. Port TT is 1-based.
    // This is quite simple:
    //  1. if parent is of high speed, then parent is our translator
    //  2. otherwise use parent's translator.
    //
    Child->Translator.TranslatorHubAddress = Parent->Address;
    Child->Translator.TranslatorPortNumber = (UINT8)(Port + 1);
  } else {
    Child->Translator = Parent->Translator;
  }

  DEBUG ((
    DEBUG_INFO,
    "UsbEnumerateNewDev: device uses translator (%d, %d)\n",
    Child->Translator.TranslatorHubAddress,
    Child->Translator.TranslatorPortNumber
    ));

  //
  // After port is reset, hub establishes a signal path between
  // the device and host (DEFAULT state). Device's registers are
  // reset, use default address 0 (host enumerates one device at
  // a time) , and ready to respond to control transfer at EP 0.
  //

  //
  // Host assigns an address to the device. Device completes the
  // status stage with default address, then switches to new address.
  // ADDRESS state. Address zero is reserved for root hub.
  //
  ASSERT (Bus->MaxDevices <= 256);
  if (Child->Address == 0) {
    for (Address = 1; Address < Bus->MaxDevices; Address++) {
      if (Bus->Devices[Address] == NULL) {
        break;
      }
    }

    if (Address >= Bus->MaxDevices) {
      DEBUG ((DEBUG_ERROR, "UsbEnumerateNewDev: address pool is full for port %d\n", Port));

      Status = EFI_ACCESS_DENIED;
      goto ON_ERROR;
    }
  } else {
    Address               = Child->Address;
    Child->Address        = 0;
    Bus->Devices[Address] = NULL;
  }

  Status                = UsbSetAddress (Child, (UINT8)Address);
  Child->Address        = (UINT8)Address;
  Bus->Devices[Address] = Child;

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "UsbEnumerateNewDev: failed to set device address - %r\n", Status));
    goto ON_ERROR;
  }

  USB_STALL (USB_SET_DEVICE_ADDRESS_STALL);

  DEBUG ((DEBUG_INFO, "UsbEnumerateNewDev: device is now ADDRESSED at %d\n", Address));

  //
  // The device no longer listens on the default address. Let other ports
  // make progress before the rest of the device is configured.
  //
  if (AllowYield && !EFI_ERROR (UsbAsyncEnumConfigureLater (HubIf, Port, Child, RetryCount))) {
    return EFI_NOT_READY;
  }

DeviceConfigure:

  //
  // Host sends a Get_Descriptor request to learn the max packet
  // size of default pipe (only part of the device's descriptor).
  //
  Status = UsbGetMaxPacketSize0 (Child);

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "UsbEnumerateNewDev: failed to get max packet for EP 0 - %r\n", Status));
    goto ON_ERROR;
  }

  DEBUG ((DEBUG_INFO, "UsbEnumerateNewDev: max packet size for EP 0 is %d\n", Child->MaxPacket0));

  //
  // Host learns about the device's abilities by requesting device's
  // entire descriptions.
  //
  Status = UsbBuildDescTable (Child);

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "UsbEnumerateNewDev: failed to build descriptor table - %r\n", Status));
    goto ON_ERROR;
  }

  // Below code is ensuring the device can be executed with SS.
  // Some device FW might execute with SS later but it would produce failure if the device is already enumerated with HS.
  if (  (RetryCount > 0)
     && (Bus->Usb2Hc != NULL)
     && (Bus->Usb2Hc->MajorRevision >= 0x3)
     && (Child->IsSSDev)
     && (Child->Speed < EFI_USB_SPEED_SUPER)
     && (Child->DevDesc->Desc.DeviceClass != USB_HUB_CLASS_CODE))
  {
    goto ON_ERROR;
  }

  //
  // Select a default configuration: UEFI must set the configuration
  // before the driver can connect to the device.
  //
  Config = Child->DevDesc->Configs[0]->Desc.ConfigurationValue;
  Status = UsbSetConfig (Child, Config);

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "UsbEnumerateNewDev: failed to set configure %d - %r\n", Config, Status));
    goto ON_ERROR;
  }

  DEBUG ((DEBUG_INFO, "UsbEnumerateNewDev: device %d is now in CONFIGED state\n", Address));

  //
  // Host assigns and loads a device driver.
  //
  Status = UsbSelectConfig (Child, Config);

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "UsbEnumerateNewDev: failed to create interfaces - %r\n", Status));
    goto ON_ERROR;
  }

  //
  // Report Status Code to indicate USB device has been detected by hotplug
  //
  REPORT_STATUS_CODE_WITH_DEVICE_PATH (
    EFI_PROGRESS_CODE,
    (EFI_IO_BUS_USB | EFI_IOB_PC_HOTPLUG),
    Bus->DevicePath
    );
  USB_TS ((DEBUG_INFO, "UsbEnumerateNewDev: done hub %p port %d\n", HubIf, Port));
  return EFI_SUCCESS;

ON_ERROR:
  //
  // Do the error handling with retry counter.
  //
  OriginalStatus = Status;
  Status         = HubApi->GetPortStatus (HubIf, Port, &PortState);
  if (EFI_ERROR (Status) || (!USB_BIT_IS_SET (PortState.PortStatus, USB_PORT_STAT_CONNECTION))) {
    DEBUG ((DEBUG_ERROR, "Device is gone. Don't reset the port\n"));
    Status = EFI_NOT_FOUND;
  }

  if (!EFI_ERROR (Status) && (RetryCount > 0)) {
    RetryCount--;
    DEBUG ((DEBUG_INFO, "Reset the port due to Error\n"));
    if ((Child != NULL) && (Child->DevDesc != NULL)) {
      UsbFreeDevDesc (Child->DevDesc);
      Child->DevDesc = NULL;
    }

    Status = HubApi->ResetPort (HubIf, Port);
    if (!EFI_ERROR (Status)) {
      USB_STALL (USB_WAIT_PORT_STABLE_STALL);
      goto DeviceRetry;
    }
  }

  //
  // If reach here, it means the enumeration process on a given port is interrupted due to error.
  // The s/w resources, including the assigned address(Address) and the allocated usb device data
  // structure(Bus->Devices[Address]), will NOT be freed here. These resources will be freed when
  // the device is unplugged from the port or DriverBindingStop() is invoked.
  //
  // This way is used to co-work with the lower layer EDKII UHCI/EHCI/XHCI host controller driver.
  // It's mainly because to keep UEFI spec unchanged EDKII XHCI driver have to maintain a state machine
  // to keep track of the mapping between actual address and request address. If the request address
  // (Address) is freed here, the Address value will be used by next enumerated device. Then EDKII XHCI
  // host controller driver will have wrong information, which will cause further transaction error.
  //
  // EDKII UHCI/EHCI doesn't get impacted as it's make sense to reserve s/w resource till it gets unplugged.
  //
  return OriginalStatus;
}

//
// All pending asynchronous enumeration steps of all USB buses.
//
STATIC LIST_ENTRY  mUsbAsyncEnumList = INITIALIZE_LIST_HEAD_VARIABLE (mUsbAsyncEnumList);

/**
  Find the pending asynchronous enumeration step of a hub port.

  @param  HubIf                 The hub interface.
  @param  Port                  The port of the hub.

  @return The pending step, or NULL if there is none.

**/
STATIC
USB_ASYNC_ENUM *
UsbAsyncEnumFind (
  IN USB_INTERFACE  *HubIf,
  IN UINT8          Port
  )
{
  LIST_ENTRY      *Entry;
  USB_ASYNC_ENUM  *Async;

  BASE_LIST_FOR_EACH (Entry, &mUsbAsyncEnumList) {
    Async = CR (Entry, USB_ASYNC_ENUM, Link, USB_ASYNC_ENUM_SIGNATURE);
    if ((Async->HubIf == HubIf) && (Async->Port == Port) && (Async->State != UsbAsyncHubPowerOn)) {
      return Async;
    }
  }

  return NULL;
}

/**
  Remove an asynchronous enumeration step from the pending list and free it.
  The timer is cancelled by closing its event.

  @param  Async                 The step to free.

**/
STATIC
VOID
UsbAsyncEnumFree (
  IN USB_ASYNC_ENUM  *Async
  )
{
  EFI_TPL  OldTpl;

  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  RemoveEntryList (&Async->Link);
  gBS->RestoreTPL (OldTpl);

  gBS->CloseEvent (Async->Timer);
  FreePool (Async);
}

/**
  Kick the root hub enumeration of all USB buses except the given one. This is
  used after an EHCI root port has been handed over to the companion controller
  so that the device is picked up there without waiting for the next poll.

  @param  Bus                   The bus that released the port.

**/
STATIC
VOID
UsbBusKickOtherRootHubs (
  IN USB_BUS  *Bus
  )
{
  LIST_ENTRY  *Entry;
  USB_BUS     *Other;

  BASE_LIST_FOR_EACH (Entry, &mUsbBusList) {
    Other = BASE_CR (Entry, USB_BUS, Link);
    if ((Other != Bus) && (Other->Devices[0] != NULL)) {
      gBS->SignalEvent (Other->Devices[0]->Interfaces[0]->HubNotify);
    }
  }
}

/**
  Timer callback that advances an asynchronous enumeration step.

  @param  Event                 The timer event.
  @param  Context               The USB_ASYNC_ENUM.

**/
STATIC
VOID
EFIAPI
UsbAsyncEnumOnTimer (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  USB_ASYNC_ENUM       *Async;
  USB_INTERFACE        *HubIf;
  USB_BUS              *Bus;
  UINT8                Port;
  USB_PORT_RESET_MODE  ResetMode;
  USB_DEVICE           *Child;
  UINT8                RetryCount;
  UINTN                DelayUs;
  EFI_STATUS           Status;

  Async = (USB_ASYNC_ENUM *)Context;
  ASSERT (Async->Signature == USB_ASYNC_ENUM_SIGNATURE);

  HubIf      = Async->HubIf;
  Bus        = Async->Bus;
  Port       = Async->Port;
  Child      = NULL;
  RetryCount = 0;

  switch (Async->State) {
    case UsbAsyncHubPowerOn:
      USB_TS ((DEBUG_INFO, "UsbAsyncEnum: hub %p power good\n", HubIf));
      UsbAsyncEnumFree (Async);
      UsbHubInitFinish (HubIf);
      return;

    case UsbAsyncPortDebounce:
      USB_TS ((DEBUG_INFO, "UsbAsyncEnum: hub %p port %d stable\n", HubIf, Port));
      if (!Async->ResetIsNeeded) {
        ResetMode = UsbPortResetSkip;
        break;
      }

      if (HubIf->HubApi != &mUsbRootHubApi) {
        //
        // A hub terminates the reset on its own and the device then listens
        // on the default address, so the reset of a hub port cannot overlap
        // with the enumeration of other devices on the bus.
        //
        ResetMode = UsbPortResetFull;
        break;
      }

      //
      // A root port stays in reset until software terminates it, so the
      // reset can be driven while other ports are enumerated.
      //
      Status = UsbRootHubResetPortStart (HubIf, Port, &DelayUs);
      if (!EFI_ERROR (Status)) {
        Async->State = UsbAsyncRootPortReset;
        Status       = gBS->SetTimer (Async->Timer, TimerRelative, EFI_TIMER_PERIOD_MICROSECONDS (DelayUs));
        if (!EFI_ERROR (Status)) {
          return;
        }

        USB_STALL (DelayUs);
        ResetMode = UsbPortResetRootFinish;
        break;
      }

      DEBUG ((DEBUG_ERROR, "UsbAsyncEnum: failed to reset port %d - %r\n", Port, Status));
      UsbAsyncEnumFree (Async);
      HubIf->HubApi->ClearPortChange (HubIf, Port);
      return;

    case UsbAsyncRootPortReset:
      ResetMode = UsbPortResetRootFinish;
      break;

    case UsbAsyncDevConfigure:
      ResetMode  = UsbPortResetSkip;
      Child      = Async->Child;
      RetryCount = Async->RetryCount;
      break;

    default:
      ASSERT (FALSE);
      return;
  }

  //
  // Remove the step from the pending list before the device is configured,
  // as connecting drivers to it may release the hub as well.
  //
  UsbAsyncEnumFree (Async);

  Status = UsbEnumerateStableDev (HubIf, Port, ResetMode, Child, RetryCount, (BOOLEAN)(Child == NULL));
  if (Status == EFI_NOT_READY) {
    return;
  }

  HubIf->HubApi->ClearPortChange (HubIf, Port);

  if ((Status == EFI_NOT_FOUND) && (HubIf->HubApi == &mUsbRootHubApi)) {
    UsbBusKickOtherRootHubs (Bus);
  }
}

/**
  Allocate and arm an asynchronous enumeration step.

  @param  HubIf                 The hub interface.
  @param  Port                  The port of the hub.
  @param  State                 The initial state.
  @param  ResetIsNeeded         Whether the port needs to be reset.
  @param  DelayUs               The time to wait before the step is executed.

  @retval EFI_SUCCESS           The step is armed.
  @retval Others                Failed to arm the step.

**/
STATIC
EFI_STATUS
UsbAsyncEnumStart (
  IN USB_INTERFACE         *HubIf,
  IN UINT8                 Port,
  IN USB_ASYNC_ENUM_STATE  State,
  IN BOOLEAN               ResetIsNeeded,
  IN UINTN                 DelayUs
  )
{
  USB_ASYNC_ENUM  *Async;
  EFI_STATUS      Status;
  EFI_TPL         OldTpl;

  Async = AllocateZeroPool (sizeof (USB_ASYNC_ENUM));
  if (Async == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Async->Signature     = USB_ASYNC_ENUM_SIGNATURE;
  Async->Bus           = HubIf->Device->Bus;
  Async->HubIf         = HubIf;
  Async->Port          = Port;
  Async->State         = State;
  Async->ResetIsNeeded = ResetIsNeeded;

  Status = gBS->CreateEvent (
                  EVT_TIMER | EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  UsbAsyncEnumOnTimer,
                  Async,
                  &Async->Timer
                  );
  if (EFI_ERROR (Status)) {
    FreePool (Async);
    return Status;
  }

  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  InsertTailList (&mUsbAsyncEnumList, &Async->Link);
  gBS->RestoreTPL (OldTpl);

  Status = gBS->SetTimer (Async->Timer, TimerRelative, EFI_TIMER_PERIOD_MICROSECONDS (DelayUs));
  if (EFI_ERROR (Status)) {
    UsbAsyncEnumFree (Async);
  }

  return Status;
}

/**
  Continue the enumeration of an addressed device in a timer callback.

  @param  HubIf                 The hub interface.
  @param  Port                  The port of the hub.
  @param  Child                 The addressed device.
  @param  RetryCount            The remaining enumeration retries.

  @retval EFI_SUCCESS           The configuration is scheduled.
  @retval Others                Failed to schedule, caller must continue.

**/
STATIC
EFI_STATUS
UsbAsyncEnumConfigureLater (
  IN USB_INTERFACE  *HubIf,
  IN UINT8          Port,
  IN USB_DEVICE     *Child,
  IN UINT8          RetryCount
  )
{
  USB_ASYNC_ENUM  *Async;
  EFI_STATUS      Status;

  Status = UsbAsyncEnumStart (HubIf, Port, UsbAsyncDevConfigure, FALSE, 0);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // The step was just added to the tail of the list.
  //
  Async             = CR (GetPreviousNode (&mUsbAsyncEnumList, &mUsbAsyncEnumList), USB_ASYNC_ENUM, Link, USB_ASYNC_ENUM_SIGNATURE);
  Async->Child      = Child;
  Async->RetryCount = RetryCount;

  //
  // Queue the callback behind the already expired timers.
  //
  gBS->SetTimer (Async->Timer, TimerCancel, 0);
  return gBS->SignalEvent (Async->Timer);
}

/**
  Arm a timer for a hub that needs to wait for its ports to be powered before
  the hub can be used, see UsbHubInitFinish ().

  @param  HubIf                 The hub interface.
  @param  DelayUs               Power-on to power-good time in microseconds.

  @retval EFI_SUCCESS           The timer is armed.
  @retval Others                Failed to arm the timer, caller must wait.

**/
EFI_STATUS
UsbAsyncHubPowerOnStart (
  IN USB_INTERFACE  *HubIf,
  IN UINTN          DelayUs
  )
{
  return UsbAsyncEnumStart (HubIf, 0, UsbAsyncHubPowerOn, FALSE, DelayUs);
}

/**
  Cancel the pending asynchronous enumeration steps of a bus or of a hub.

  @param  Bus                   Cancel all steps of this bus, or NULL.
  @param  HubIf                 Cancel all steps of this hub, or NULL.

  @retval TRUE                  A pending hub power-on step of HubIf was
                                cancelled, so the hub was never fully
                                initialized.
  @retval FALSE                 No hub power-on step was cancelled.

**/
BOOLEAN
UsbAsyncEnumCancel (
  IN USB_BUS        *Bus    OPTIONAL,
  IN USB_INTERFACE  *HubIf  OPTIONAL
  )
{
  LIST_ENTRY      *Entry;
  LIST_ENTRY      *Next;
  USB_ASYNC_ENUM  *Async;
  BOOLEAN         PowerOnCancelled;
  EFI_TPL         OldTpl;

  PowerOnCancelled = FALSE;

  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  BASE_LIST_FOR_EACH_SAFE (Entry, Next, &mUsbAsyncEnumList) {
    Async = CR (Entry, USB_ASYNC_ENUM, Link, USB_ASYNC_ENUM_SIGNATURE);
    if (((Bus != NULL) && (Async->Bus == Bus)) || ((HubIf != NULL) && (Async->HubIf == HubIf))) {
      DEBUG ((DEBUG_INFO, "UsbAsyncEnumCancel: cancel hub %p port %d state %d\n", Async->HubIf, Async->Port, Async->State));
      if ((Async->State == UsbAsyncHubPowerOn) && (Async->HubIf == HubIf)) {
        PowerOnCancelled = TRUE;
      }

      UsbAsyncEnumFree (Async);
    }
  }

  gBS->RestoreTPL (OldTpl);

  return PowerOnCancelled;
}

/**
  Wait until all pending asynchronous enumeration steps on all USB buses
  are finished. Only waits if the timer callbacks can run at the current TPL.

**/
VOID
UsbBusWaitForAsyncEnum (
  VOID
  )
{
  UINTN  Index;

  if (IsListEmpty (&mUsbAsyncEnumList)) {
    return;
  }

  if (UsbGetCurrentTpl () >= TPL_CALLBACK) {
    DEBUG ((DEBUG_INFO, "UsbBusWaitForAsyncEnum: cannot wait at TPL %d\n", (UINT32)UsbGetCurrentTpl ()));
    return;
  }

  USB_TS ((DEBUG_INFO, "UsbBusWaitForAsyncEnum: waiting for pending enumeration\n"));

  for (Index = 0; Index < USB_ASYNC_ENUM_WAIT_LOOP; Index++) {
    if (IsListEmpty (&mUsbAsyncEnumList)) {
      break;
    }

    gBS->Stall (USB_ASYNC_ENUM_WAIT_STALL);
  }

  USB_TS ((DEBUG_INFO, "UsbBusWaitForAsyncEnum: done%a\n", IsListEmpty (&mUsbAsyncEnumList) ? "" : " (timeout)"));
}

/**
  Process the events on the port.

  @param  HubIf                 The HUB that has the device connected.
  @param  Port                  The port index of the hub (started with zero).

  @retval EFI_SUCCESS           The device is enumerated (added or removed).
  @retval EFI_OUT_OF_RESOURCES  Failed to allocate resource for the device.
  @retval Others                Failed to enumerate the device.

**/
EFI_STATUS
UsbEnumeratePort (
  IN USB_INTERFACE  *HubIf,
  IN UINT8          Port
  )
{
  USB_HUB_API          *HubApi;
  USB_DEVICE           *Child;
  EFI_USB_PORT_STATUS  PortState;
  EFI_STATUS           Status;
  BOOLEAN              ResetIsNeeded;

  Child  = NULL;
  HubApi = HubIf->HubApi;

  //
  // The port is being enumerated asynchronously, its change bits are
  // cleared once that is done.
  //
  if (UsbAsyncEnumFind (HubIf, Port) != NULL) {
    return EFI_SUCCESS;
  }

  // Zero out PortState in case GetPortStatus does not set
  // it and we continue on the EFI_DEVICE_ERROR path
  PortState.PortStatus       = 0;
  PortState.PortChangeStatus = 0;

  //
  // Host learns of the new device by polling the hub for port changes.
  //
  Status = HubApi->GetPortStatus (HubIf, Port, &PortState);

  if (EFI_ERROR (Status) && (Status != EFI_DEVICE_ERROR)) {
    DEBUG ((DEBUG_ERROR, "UsbEnumeratePort: failed to get state of port %d\n", Port));
    return Status;
  }

  //
  // Only handle connection/enable/overcurrent/reset change.
  // Usb super speed hub may report other changes, such as warm reset change. Ignore them.
  //
  if ((PortState.PortChangeStatus & (USB_PORT_STAT_C_CONNECTION | USB_PORT_STAT_C_ENABLE | USB_PORT_STAT_C_OVERCURRENT | USB_PORT_STAT_C_RESET)) == 0) {
    return EFI_SUCCESS;
  }

  DEBUG ((
    DEBUG_INFO,
    "UsbEnumeratePort: port %d state - %02x, change - %02x on %p\n",
    Port,
    PortState.PortStatus,
    PortState.PortChangeStatus,
    HubIf
    ));

  //
  // This driver only process two kinds of events now: over current and
  // connect/disconnect. Other three events are: ENABLE, SUSPEND, RESET.
  // ENABLE/RESET is used to reset port. SUSPEND isn't supported.
  //

  if (USB_BIT_IS_SET (PortState.PortChangeStatus, USB_PORT_STAT_C_OVERCURRENT)) {
    if (USB_BIT_IS_SET (PortState.PortStatus, USB_PORT_STAT_OVERCURRENT)) {
      //
      // Case1:
      //   Both OverCurrent and OverCurrentChange set, means over current occurs,
      //   which probably is caused by short circuit. It has to wait system hardware
      //   to perform recovery.
      //
      DEBUG ((DEBUG_ERROR, "UsbEnumeratePort: Critical Over Current (port %d)\n", Port));
      return EFI_DEVICE_ERROR;
    }

    //
    // Case2:
    //   Only OverCurrentChange set, means system has been recoveried from
    //   over current. As a result, all ports are nearly power-off, so
    //   it's necessary to detach and enumerate all ports again.
    //
    DEBUG ((DEBUG_ERROR, "UsbEnumeratePort: 2.0 device Recovery Over Current (port %d)\n", Port));
  }

  if (USB_BIT_IS_SET (PortState.PortChangeStatus, USB_PORT_STAT_C_ENABLE)) {
    //
    // Case3:
    //   1.1 roothub port reg doesn't reflect over-current state, while its counterpart
    //   on 2.0 roothub does. When over-current has influence on 1.1 device, the port
    //   would be disabled, so it's also necessary to detach and enumerate again.
    //
    DEBUG ((DEBUG_ERROR, "UsbEnumeratePort: 1.1 device Recovery Over Current (port %d)\n", Port));
  }

  if (USB_BIT_IS_SET (PortState.PortChangeStatus, USB_PORT_STAT_C_CONNECTION)) {
    //
    // Case4:
    //   Device connected or disconnected normally.
    //
    DEBUG ((DEBUG_INFO, "UsbEnumeratePort: Device Connect/Disconnect Normally (port %d)\n", Port));
  }

  //
  // Following as the above cases, it's safety to remove and create again.
  //
  Child = UsbFindChild (HubIf, Port);

  if (Child != NULL) {
    DEBUG ((DEBUG_INFO, "UsbEnumeratePort: device at port %d removed from root hub %p\n", Port, HubIf));
    UsbRemoveDevice (Child);
  }

  if (USB_BIT_IS_SET (PortState.PortStatus, USB_PORT_STAT_CONNECTION)) {
    //
    // Now, new device connected, enumerate and configure the device
    //
    DEBUG ((DEBUG_INFO, "UsbEnumeratePort: new device connected at port %d\n", Port));
    ResetIsNeeded = (BOOLEAN) !(USB_BIT_IS_SET (PortState.PortChangeStatus, USB_PORT_STAT_C_RESET) &&
                                (Status != EFI_DEVICE_ERROR));

    //
    // Wait for the connection to become stable in a timer callback, so that
    // all ports of all hubs and host controllers can wait in parallel.
    //
    Status = UsbAsyncEnumStart (HubIf, Port, UsbAsyncPortDebounce, ResetIsNeeded, USB_WAIT_PORT_STABLE_STALL);
    if (!EFI_ERROR (Status)) {
      return EFI_SUCCESS;
    }

    Status = UsbEnumerateNewDev (HubIf, Port, ResetIsNeeded);
  } else {
    DEBUG ((DEBUG_INFO, "UsbEnumeratePort: device disconnected event on port %d\n", Port));
  }

  HubApi->ClearPortChange (HubIf, Port);
  return Status;
}

/**
  Enumerate all the changed hub ports.

  @param  Event                 The event that is triggered.
  @param  Context               The context to the event.

**/
VOID
EFIAPI
UsbHubEnumeration (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  USB_INTERFACE  *HubIf;
  UINT8          Byte;
  UINT8          Bit;
  UINT8          Index;
  USB_DEVICE     *Child;

  ASSERT (Context != NULL);

  HubIf = (USB_INTERFACE *)Context;

  for (Index = 0; Index < HubIf->NumOfPort; Index++) {
    Child = UsbFindChild (HubIf, Index);
    if ((Child != NULL) && (Child->DisconnectFail == TRUE)) {
      DEBUG ((DEBUG_INFO, "UsbEnumeratePort: The device disconnect fails at port %d from hub %p, try again\n", Index, HubIf));
      UsbRemoveDevice (Child);
    }
  }

  if (HubIf->ChangeMap == NULL) {
    return;
  }

  //
  // HUB starts its port index with 1.
  //
  Byte = 0;
  Bit  = 1;

  for (Index = 0; Index < HubIf->NumOfPort; Index++) {
    if (USB_BIT_IS_SET (HubIf->ChangeMap[Byte], USB_BIT (Bit))) {
      UsbEnumeratePort (HubIf, Index);
    }

    USB_NEXT_BIT (Byte, Bit);
  }

  UsbHubAckHubStatus (HubIf->Device);

  gBS->FreePool (HubIf->ChangeMap);
  HubIf->ChangeMap = NULL;
  return;
}

/**
  Enumerate all the changed hub ports.

  @param  Event                 The event that is triggered.
  @param  Context               The context to the event.

**/
VOID
EFIAPI
UsbRootHubEnumeration (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  USB_INTERFACE  *RootHub;
  UINT8          Index;
  USB_DEVICE     *Child;

  RootHub = (USB_INTERFACE *)Context;

  for (Index = 0; Index < RootHub->NumOfPort; Index++) {
    Child = UsbFindChild (RootHub, Index);
    if ((Child != NULL) && (Child->DisconnectFail == TRUE)) {
      DEBUG ((DEBUG_INFO, "UsbEnumeratePort: The device disconnect fails at port %d from root hub %p, try again\n", Index, RootHub));
      UsbRemoveDevice (Child);
    }

    UsbEnumeratePort (RootHub, Index);
  }
}

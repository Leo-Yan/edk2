/** @file
*
*  Copyright (c) 2015, Linaro Ltd. All rights reserved.
*  Copyright (c) 2015, Hisilicon Ltd. All rights reserved.
*
*  This program and the accompanying materials
*  are licensed and made available under the terms and conditions of the BSD License
*  which accompanies this distribution.  The full text of the license may be found at
*  http://opensource.org/licenses/bsd-license.php
*
*  THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
*  WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.
*
**/

#include <Library/BaseMemoryLib.h>
#include <Library/BdsLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>

#include <Protocol/DevicePathFromText.h>
#include <Protocol/DevicePathToText.h>
#include <Protocol/EmbeddedGpio.h>

#include <Guid/ArmGlobalVariableHob.h>
#include <Guid/EventGroup.h>
#include <Guid/GlobalVariable.h>
#include <Guid/VariableFormat.h>

#include "HiKeyDxeInternal.h"

#define MAX_BOOT_ENTRIES         16
// Jumper on pin5-6 of J15 determines whether boot to fastboot
#define DETECT_J15_FASTBOOT      0    // pin number in GPIO controller

STATIC CONST BOOLEAN mIsEndOfDxeEvent = TRUE;
STATIC UINT16 *mBootOrder = NULL;
STATIC UINT16 mBootCount = 0;
STATIC UINT16 mBootIndex = 0;

STATIC
BOOLEAN
EFIAPI
HiKeyVerifyBootEntry (
  IN CHAR16          *BootVariableName,
  IN CHAR16          *BootDevicePathText,
  IN CHAR16          *BootArgs,
  IN CHAR16          *BootDescription,
  IN UINT16           LoadOptionAttr
  )
{
  EFI_DEVICE_PATH_TO_TEXT_PROTOCOL   *DevicePathToTextProtocol;
  CHAR16                             *DevicePathText;
  UINTN                               EfiLoadOptionSize;
  EFI_LOAD_OPTION                     EfiLoadOption;
  BDS_LOAD_OPTION                    *LoadOption;
  EFI_STATUS                          Status;
  UINTN                               DescriptionLength;

  Status = GetGlobalEnvironmentVariable (BootVariableName, NULL, &EfiLoadOptionSize, (VOID**)&EfiLoadOption);
  if (EFI_ERROR (Status)) {
    return FALSE;
  }
  if (EfiLoadOption == NULL) {
    return FALSE;
  }
  if (EfiLoadOptionSize < sizeof(UINT32) + sizeof(UINT16) + sizeof(CHAR16) + sizeof(EFI_DEVICE_PATH_PROTOCOL)) {
    return FALSE;
  }
  LoadOption = (BDS_LOAD_OPTION*)AllocateZeroPool (sizeof(BDS_LOAD_OPTION));
  if (LoadOption == NULL) {
    return FALSE;
  }

  LoadOption->LoadOption     = EfiLoadOption;
  LoadOption->Attributes         = *(UINT32*)EfiLoadOption;
  LoadOption->FilePathListLength = *(UINT16*)(EfiLoadOption + sizeof(UINT32));
  LoadOption->Description        = (CHAR16*)(EfiLoadOption + sizeof(UINT32) + sizeof(UINT16));
  DescriptionLength              = StrSize (LoadOption->Description);
  LoadOption->FilePathList       = (EFI_DEVICE_PATH_PROTOCOL*)(EfiLoadOption + sizeof(UINT32) + sizeof(UINT16) + DescriptionLength);
  if ((UINTN)((UINTN)LoadOption->FilePathList + LoadOption->FilePathListLength - (UINTN)EfiLoadOption) == EfiLoadOptionSize) {
    LoadOption->OptionalData     = NULL;
    LoadOption->OptionalDataSize = 0;
  } else {
    LoadOption->OptionalData     = (VOID*)((UINTN)(LoadOption->FilePathList) + LoadOption->FilePathListLength);
    LoadOption->OptionalDataSize = EfiLoadOptionSize - ((UINTN)LoadOption->OptionalData - (UINTN)EfiLoadOption);
  }

  if (((BootArgs == NULL) && (LoadOption->OptionalDataSize)) ||
      (BootArgs && (LoadOption->OptionalDataSize == 0))) {
    return FALSE;
  } else if (BootArgs && LoadOption->OptionalDataSize) {
    if (StrCmp (BootArgs, LoadOption->OptionalData) != 0)
      return FALSE;
  }
  if ((LoadOption->Description == NULL) || (BootDescription == NULL)) {
    return FALSE;
  }
  if (StrCmp (BootDescription, LoadOption->Description) != 0) {
    return FALSE;
  }
  if ((LoadOption->Attributes & LOAD_OPTION_CATEGORY) != (LoadOptionAttr & LOAD_OPTION_CATEGORY)) {
    return FALSE;
  }

  Status = gBS->LocateProtocol (&gEfiDevicePathToTextProtocolGuid, NULL, (VOID **)&DevicePathToTextProtocol);
  ASSERT_EFI_ERROR(Status);
  DevicePathText = DevicePathToTextProtocol->ConvertDevicePathToText(LoadOption->FilePathList, TRUE, TRUE);
  if (StrCmp (DevicePathText, BootDevicePathText) != 0) {
    return FALSE;
  }

  FreePool (LoadOption);
  return TRUE;
}

STATIC
EFI_STATUS
EFIAPI
HiKeyCreateBootEntry (
  IN CHAR16          *DevicePathText,
  IN CHAR16          *BootArgs,
  IN CHAR16          *BootDescription,
  IN UINT16           LoadOption
  )
{
  BDS_LOAD_OPTION                    *BdsLoadOption;
  EFI_STATUS                          Status;
  UINTN                               DescriptionSize;
  UINTN                               BootOrderSize;
  CHAR16                              BootVariableName[9];
  UINT8                              *EfiLoadOptionPtr;
  EFI_DEVICE_PATH_PROTOCOL           *DevicePathNode;
  UINTN                               NodeLength;
  EFI_DEVICE_PATH_FROM_TEXT_PROTOCOL *DevicePathFromTextProtocol;

  if ((DevicePathText == NULL) || (BootDescription == NULL)) {
    DEBUG ((EFI_D_ERROR, "%a: Invalid Parameters\n", __func__));
    return EFI_INVALID_PARAMETER;
  }

  UnicodeSPrint (BootVariableName, 9 * sizeof(CHAR16), L"Boot%04X", mBootCount);
  if (HiKeyVerifyBootEntry (BootVariableName, DevicePathText, BootArgs, BootDescription, LoadOption) == TRUE) {
    // The boot entry is already created.
    Status = EFI_SUCCESS;
    goto done;
  }

  BdsLoadOption = (BDS_LOAD_OPTION*)AllocateZeroPool (sizeof(BDS_LOAD_OPTION));
  ASSERT (BdsLoadOption != NULL);

  Status = gBS->LocateProtocol (
                  &gEfiDevicePathFromTextProtocolGuid,
                  NULL,
                  (VOID**)&DevicePathFromTextProtocol
                  );
  ASSERT_EFI_ERROR(Status);

  BdsLoadOption->FilePathList = DevicePathFromTextProtocol->ConvertTextToDevicePath (DevicePathText);
  ASSERT (BdsLoadOption->FilePathList != NULL);
  BdsLoadOption->FilePathListLength = GetDevicePathSize (BdsLoadOption->FilePathList);
  BdsLoadOption->Attributes = LOAD_OPTION_ACTIVE | (LoadOption & LOAD_OPTION_CATEGORY);

  if (BootArgs) {
    BdsLoadOption->OptionalDataSize = StrSize (BootArgs);
    BdsLoadOption->OptionalData = (CHAR16*)AllocateZeroPool (BdsLoadOption->OptionalDataSize);
    ASSERT (BdsLoadOption->OptionalData != NULL);
    StrCpy (BdsLoadOption->OptionalData, BootArgs);
  }

  BdsLoadOption->LoadOptionIndex = mBootCount;
  DescriptionSize = StrSize (BootDescription);
  BdsLoadOption->Description = (VOID*)AllocateZeroPool (DescriptionSize);
  StrCpy (BdsLoadOption->Description, BootDescription);

  BdsLoadOption->LoadOptionSize = sizeof(UINT32) + sizeof(UINT16) + DescriptionSize + BdsLoadOption->FilePathListLength + BdsLoadOption->OptionalDataSize;
  BdsLoadOption->LoadOption = (EFI_LOAD_OPTION)AllocateZeroPool (BdsLoadOption->LoadOptionSize);
  ASSERT (BdsLoadOption->LoadOption != NULL);

  EfiLoadOptionPtr = BdsLoadOption->LoadOption;

  //
  // Populate the EFI Load Option and BDS Boot Option structures
  //

  // Attributes fields
  *(UINT32*)EfiLoadOptionPtr = BdsLoadOption->Attributes;
  EfiLoadOptionPtr += sizeof(UINT32);

  // FilePath List fields
  *(UINT16*)EfiLoadOptionPtr = BdsLoadOption->FilePathListLength;
  EfiLoadOptionPtr += sizeof(UINT16);

  // Boot description fields
  CopyMem (EfiLoadOptionPtr, BdsLoadOption->Description, DescriptionSize);
  EfiLoadOptionPtr += DescriptionSize;

  // File path fields
  DevicePathNode = BdsLoadOption->FilePathList;
  while (!IsDevicePathEndType (DevicePathNode)) {
    NodeLength = DevicePathNodeLength(DevicePathNode);
    CopyMem (EfiLoadOptionPtr, DevicePathNode, NodeLength);
    EfiLoadOptionPtr += NodeLength;
    DevicePathNode = NextDevicePathNode (DevicePathNode);
  }

  // Set the End Device Path Type
  SetDevicePathEndNode (EfiLoadOptionPtr);
  EfiLoadOptionPtr += sizeof(EFI_DEVICE_PATH);

  // Fill the Optional Data
  if (BdsLoadOption->OptionalDataSize > 0) {
    CopyMem (EfiLoadOptionPtr, BdsLoadOption->OptionalData, BdsLoadOption->OptionalDataSize);
  }

  Status = gRT->SetVariable (
                  BootVariableName,
                  &gEfiGlobalVariableGuid,
                  EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS,
                  BdsLoadOption->LoadOptionSize,
                  BdsLoadOption->LoadOption
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "%a: failed to set BootVariable\n", __func__));
    return Status;
  }

done:
  BootOrderSize = mBootCount * sizeof (UINT16);
  mBootOrder = ReallocatePool (BootOrderSize, BootOrderSize + sizeof (UINT16), mBootOrder);
  mBootOrder[mBootCount] = mBootCount;
  mBootCount++;
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
HiKeyCreateBootOrder (
  IN    VOID
  )
{
  UINT16             *BootOrder;
  UINTN               BootOrderSize;
  UINTN               Index;
  EFI_STATUS          Status;

  Status = GetGlobalEnvironmentVariable (L"BootOrder", NULL, &BootOrderSize, (VOID**)&BootOrder);
  if (EFI_ERROR(Status) == 0) {
    if (BootOrderSize == mBootCount) {
      for (Index = 0; Index < mBootCount; Index++) {
        if (BootOrder[Index] != mBootOrder[Index]) {
          break;
        }
      }
      if (Index == mBootCount) {
        // Found BootOrder variable with expected value.
        return EFI_SUCCESS;
      }
    }
  }

  Status = gRT->SetVariable (
                  (CHAR16*)L"BootOrder",
                  &gEfiGlobalVariableGuid,
                  EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS,
                  mBootCount * sizeof(UINT16),
                  mBootOrder
                  );
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
HiKeyCreateBootNext (
  IN     VOID
  )
{
  EFI_STATUS          Status;
  UINT16             *BootNext;
  UINTN               BootNextSize;

  BootNextSize = sizeof(UINT16);
  Status = GetGlobalEnvironmentVariable (L"BootNext", NULL, &BootNextSize, (VOID**)&BootNext);
  if (EFI_ERROR(Status) == 0) {
    if (BootNextSize == sizeof (UINT16)) {
      if (*BootNext == mBootOrder[mBootIndex]) {
        // Found the BootNext variable with expected value.
        return EFI_SUCCESS;
      }
    }
  }
  BootNext = &mBootOrder[mBootIndex];
  Status = gRT->SetVariable (
                  (CHAR16*)L"BootNext",
                  &gEfiGlobalVariableGuid,
                  EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS,
                  sizeof (UINT16),
                  BootNext
                  );
  return Status;
}

STATIC
VOID
EFIAPI
HiKeyDetectJumper (
  IN     VOID
  )
{
  EMBEDDED_GPIO         *Gpio;
  EFI_STATUS             Status;
  UINTN                  Value;

  Status = gBS->LocateProtocol (&gEmbeddedGpioProtocolGuid, NULL, (VOID **)&Gpio);
  ASSERT_EFI_ERROR (Status);

  Status = Gpio->Set (Gpio, DETECT_J15_FASTBOOT, GPIO_MODE_INPUT);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "%a: failed to set jumper as gpio input\n", __func__));
    return;
  }
  Status = Gpio->Get (Gpio, DETECT_J15_FASTBOOT, &Value);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "%a: failed to get value from jumper\n", __func__));
    return;
  }
  if (Value == 1) {
    // Jump not connected on pin5-6 of J15
    mBootIndex = 1;
  } else {
    mBootIndex = 0;
  }
}

STATIC
VOID
EFIAPI
HiKeyCreateFdtVariable (
  IN CHAR16          *FdtPathText
  )
{
  UINTN                     FdtDevicePathSize;
  EFI_DEVICE_PATH_PROTOCOL *FdtDevicePath;
  EFI_STATUS                Status;
  EFI_DEVICE_PATH_FROM_TEXT_PROTOCOL *DevicePathFromTextProtocol;

  Status = gBS->LocateProtocol (
                  &gEfiDevicePathFromTextProtocolGuid,
                  NULL,
                  (VOID**)&DevicePathFromTextProtocol
                  );
  ASSERT_EFI_ERROR(Status);

  FdtDevicePath = DevicePathFromTextProtocol->ConvertTextToDevicePath (FdtPathText);
  ASSERT (FdtDevicePath != NULL);

  FdtDevicePathSize = GetDevicePathSize (FdtDevicePath);
  Status = gRT->SetVariable (
                  (CHAR16*)L"Fdt",
                  &gArmGlobalVariableGuid,
                  EFI_VARIABLE_RUNTIME_ACCESS | EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS,
                  FdtDevicePathSize,
                  FdtDevicePath
                  );
  ASSERT_EFI_ERROR(Status);
}

STATIC
VOID
EFIAPI
HiKeyOnEndOfDxe (
  EFI_EVENT                               Event,
  VOID                                    *Context
  )
{
  EFI_STATUS          Status;
  UINTN               VariableSize;
  UINT16              BootIndex, AutoBoot;

  VariableSize = sizeof (UINT16);
  Status = gRT->GetVariable (
                  (CHAR16 *)L"HiKeyAutoBoot",
                  &gArmGlobalVariableGuid,
                  NULL,
                  &VariableSize,
                  (VOID*)&AutoBoot
                  );
  if (Status == EFI_NOT_FOUND) {
    AutoBoot = 1;
    Status = gRT->SetVariable (
                    (CHAR16*)L"HiKeyAutoBoot",
                    &gArmGlobalVariableGuid,
                    EFI_VARIABLE_NON_VOLATILE       |
                    EFI_VARIABLE_BOOTSERVICE_ACCESS |
                    EFI_VARIABLE_RUNTIME_ACCESS,
                    sizeof (UINT16),
                    &AutoBoot
                    );
    ASSERT_EFI_ERROR (Status);
  } else if (EFI_ERROR (Status) == 0) {
    if (AutoBoot == 0) {
      // Select boot entry by manual.
      // Delete the BootNext environment variable
      gRT->SetVariable (L"BootNext",
             &gEfiGlobalVariableGuid,
             EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS,
             0,
             NULL);
      return;
    }
  }

  mBootCount = 0;
  mBootOrder = NULL;

  Status = HiKeyCreateBootEntry (
             L"VenHw(B549F005-4BD4-4020-A0CB-06F42BDA68C3)/HD(6,GPT,5C0F213C-17E1-4149-88C8-8B50FB4EC70E,0x7000,0x20000)/fastboot.efi",
             NULL,
             L"fastboot",
             LOAD_OPTION_CATEGORY_APP
             );
  ASSERT_EFI_ERROR (Status);

  Status = HiKeyCreateBootEntry (
             L"VenHw(B549F005-4BD4-4020-A0CB-06F42BDA68C3)/HD(6,GPT,5C0F213C-17E1-4149-88C8-8B50FB4EC70E,0x7000,0x20000)/Image",
             L"console=ttyAMA0,115200 earlycon=pl011,0xf8015000 root=/dev/disk/by-partlabel/system rw rootwait initrd=initrd.img efi=noruntime",
             L"Debian on eMMC",
             LOAD_OPTION_CATEGORY_BOOT
             );
  ASSERT_EFI_ERROR (Status);

  Status = HiKeyCreateBootEntry (
             L"VenHw(594BFE73-5E18-4F12-8119-19DB8C5FC849)/HD(1,MBR,0x00000000,0x3F,0x21FC0)/Image",
             L"console=ttyAMA0,115200 earlycon=pl011,0xf8015000 root=/dev/mmcblk1p2 rw rootwait initrd=initrd.img-3.18.0-linaro-hikey efi=noruntime",
             L"Debian on SD",
             LOAD_OPTION_CATEGORY_BOOT
             );
  ASSERT_EFI_ERROR (Status);

  if ((mBootCount == 0) || (mBootCount >= MAX_BOOT_ENTRIES)) {
    DEBUG ((EFI_D_ERROR, "%a: can't create boot entries\n", __func__));
    return;
  }

  Status = HiKeyCreateBootOrder ();
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "%a: failed to set BootOrder variable\n", __func__));
    return;
  }

  HiKeyDetectJumper ();

  // The priority on jumper is higher than HiKeyBoot.
  if (mBootIndex > 0) {
    VariableSize = sizeof (UINT16);
    Status = gRT->GetVariable (
                    (CHAR16*)L"HiKeyBootNext",
                    &gArmGlobalVariableGuid,
                    NULL,
                    &VariableSize,
                    (VOID*)&BootIndex
                    );
    if ((EFI_ERROR (Status) == 0) && (BootIndex > 0)) {
      mBootIndex = BootIndex;
    }
  }

  // Fdt variable should be aligned with Image path.
  // In another word, Fdt and Image file should be located in the same path.
  switch (mBootIndex) {
  case 1:
    HiKeyCreateFdtVariable (L"VenHw(B549F005-4BD4-4020-A0CB-06F42BDA68C3)/HD(6,GPT,5C0F213C-17E1-4149-88C8-8B50FB4EC70E,0x7000,0x20000)/hi6220-hikey.dtb");
    break;
  case 2:
    HiKeyCreateFdtVariable (L"VenHw(594BFE73-5E18-4F12-8119-19DB8C5FC849)/HD(1,MBR,0x00000000,0x3F,0x21FC0)/hi6220-hikey.dtb");
    break;
  }

  Status = HiKeyCreateBootNext ();
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "%a: failed to set BootNext variable\n", __func__));
    return;
  }
}

EFI_STATUS
HiKeyBootMenuInstall (
  IN VOID
  )
{
  EFI_STATUS          Status;
  EFI_EVENT           EndOfDxeEvent;

  Status = gBS->CreateEventEx (
                  EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  HiKeyOnEndOfDxe,
                  &mIsEndOfDxeEvent,
                  &gEfiEndOfDxeEventGroupGuid,
                  &EndOfDxeEvent
                  );
  ASSERT_EFI_ERROR (Status);
  return Status;
}


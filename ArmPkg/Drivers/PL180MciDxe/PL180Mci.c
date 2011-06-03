/** @file
  This file implement the MMC Host Protocol for the ARM PrimeCell PL180.

  Copyright (c) 2011, ARM Limited. All rights reserved.
  
  This program and the accompanying materials                          
  are licensed and made available under the terms and conditions of the BSD License         
  which accompanies this distribution.  The full text of the license may be found at        
  http://opensource.org/licenses/bsd-license.php                                            

  THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,                     
  WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.             

**/

#include "PL180Mci.h"

#include <Library/DevicePathLib.h>
#include <Library/BaseMemoryLib.h>

EFI_MMC_HOST_PROTOCOL     *gpMmcHost;

// Untested ...
//#define USE_STREAM

#define MMCI0_BLOCKLEN 512
#define MMCI0_POW2_BLOCKLEN     9
#define MMCI0_TIMEOUT           1000

BOOLEAN
MciIsPowerOn (
  VOID
  )
{
  return ((MmioRead32(MCI_POWER_CONTROL_REG) & 0x3) == MCI_POWER_ON);
}

EFI_STATUS
MciInitialize (
  VOID
  )
{
  MCI_TRACE("MciInitialize()");
  return EFI_SUCCESS;
}

BOOLEAN
MciIsCardPresent (
  VOID
  )
{
  return (MmioRead32(FixedPcdGet32(PcdPL180SysMciRegAddress)) & 1);
}

BOOLEAN
MciIsReadOnly (
  VOID
  )
{
  return (MmioRead32(FixedPcdGet32(PcdPL180SysMciRegAddress)) & 2);
}

// Convert block size to 2^n
STATIC
UINT32
GetPow2BlockLen (
  IN UINT32 BlockLen
  )
{
  UINTN Loop;
  UINTN Pow2BlockLen;

  Loop    = 0x8000;
  Pow2BlockLen = 15;
  do {
    Loop = (Loop >> 1) & 0xFFFF;
    Pow2BlockLen--;
  } while (Pow2BlockLen && (!(Loop & BlockLen)));

  return Pow2BlockLen;
}

VOID
MciPrepareDataPath (
  IN UINTN TransferDirection
  )
{
  // Set Data Length & Data Timer
  MmioWrite32(MCI_DATA_TIMER_REG,0xFFFFFFF);
  MmioWrite32(MCI_DATA_LENGTH_REG,MMCI0_BLOCKLEN);

#ifndef USE_STREAM
  //Note: we are using a hardcoded BlockLen (=512). If we decide to use a variable size, we could
  // compute the pow2 of BlockLen with the above function GetPow2BlockLen()
  MmioWrite32(MCI_DATA_CTL_REG, MCI_DATACTL_ENABLE |  MCI_DATACTL_DMA_ENABLE | TransferDirection | (MMCI0_POW2_BLOCKLEN << 4));
#else
  MmioWrite32(MCI_DATA_CTL_REG, MCI_DATACTL_ENABLE | MCI_DATACTL_DMA_ENABLE | TransferDirection | MCI_DATACTL_STREAM_TRANS);
#endif
}

EFI_STATUS
MciSendCommand (
  IN MMC_CMD MmcCmd,
  IN UINT32 Argument
  )
{
  UINT32 Status;
  UINT32 Cmd;
  UINTN  RetVal;
  UINTN  CmdCtrlReg;

  RetVal = EFI_SUCCESS;

  if ((MmcCmd == MMC_CMD17) || (MmcCmd == MMC_CMD11)) {
    MciPrepareDataPath(MCI_DATACTL_CARD_TO_CONT);
  } else if ((MmcCmd == MMC_CMD24) || (MmcCmd == MMC_CMD20)) {
    MciPrepareDataPath(MCI_DATACTL_CONT_TO_CARD);
  }

  // Create Command for PL180
  Cmd = (MMC_GET_INDX(MmcCmd) & INDX_MASK)  | MCI_CPSM_ENABLED;
  if (MmcCmd & MMC_CMD_WAIT_RESPONSE) {
    Cmd |= MCI_CPSM_WAIT_RESPONSE;
  }

  if (MmcCmd & MMC_CMD_LONG_RESPONSE) {
    Cmd |= MCI_CPSM_LONG_RESPONSE;
  }

  // Clear Status register static flags
  MmioWrite32(MCI_CLEAR_STATUS_REG,0x7FF);

  //Write to command argument register
  MmioWrite32(MCI_ARGUMENT_REG,Argument);

  //Write to command register
  MmioWrite32(MCI_COMMAND_REG,Cmd);

  if (Cmd & MCI_CPSM_WAIT_RESPONSE) {
    Status = MmioRead32(MCI_STATUS_REG);
    while (!(Status & (MCI_STATUS_CMD_RESPEND | MCI_STATUS_CMD_CMDCRCFAIL | MCI_STATUS_CMD_CMDTIMEOUT | MCI_STATUS_CMD_START_BIT_ERROR))) {
      Status = MmioRead32(MCI_STATUS_REG);
    }

    if ((Status & MCI_STATUS_CMD_START_BIT_ERROR)) {
      DEBUG ((EFI_D_ERROR, "MciSendCommand(CmdIndex:%d) Start bit Error! Response:0x%X Status:0x%x\n",(Cmd & 0x3F),MmioRead32(MCI_RESPONSE0_REG),Status));
      RetVal = EFI_NO_RESPONSE;
      goto Exit;
    } else if ((Status & MCI_STATUS_CMD_CMDTIMEOUT)) {
      //DEBUG ((EFI_D_ERROR, "MciSendCommand(CmdIndex:%d) TIMEOUT! Response:0x%X Status:0x%x\n",(Cmd & 0x3F),MmioRead32(MCI_RESPONSE0_REG),Status));
      RetVal = EFI_TIMEOUT;
      goto Exit;
    } else if ((!(MmcCmd & MMC_CMD_NO_CRC_RESPONSE)) && (Status & MCI_STATUS_CMD_CMDCRCFAIL)) {
      // The CMD1 and response type R3 do not contain CRC. We should ignore the CRC failed Status.
      RetVal = EFI_CRC_ERROR;
      goto Exit;
    } else {
      RetVal =  EFI_SUCCESS;
      goto Exit;
    }
  } else {
    Status = MmioRead32(MCI_STATUS_REG);
    while (!(Status & (MCI_STATUS_CMD_SENT | MCI_STATUS_CMD_CMDCRCFAIL | MCI_STATUS_CMD_CMDTIMEOUT| MCI_STATUS_CMD_START_BIT_ERROR))) {
      Status = MmioRead32(MCI_STATUS_REG);
    }

    if ((Status & MCI_STATUS_CMD_START_BIT_ERROR)) {
      DEBUG ((EFI_D_ERROR, "MciSendCommand(CmdIndex:%d) Start bit Error! Response:0x%X Status:0x%x\n",(Cmd & 0x3F),MmioRead32(MCI_RESPONSE0_REG),Status));
      RetVal = EFI_NO_RESPONSE;
      goto Exit;
    } else if ((Status & MCI_STATUS_CMD_CMDTIMEOUT)) {
        //DEBUG ((EFI_D_ERROR, "MciSendCommand(CmdIndex:%d) TIMEOUT! Response:0x%X Status:0x%x\n",(Cmd & 0x3F),MmioRead32(MCI_RESPONSE0_REG),Status));
      RetVal = EFI_TIMEOUT;
      goto Exit;
    } else
    if ((!(MmcCmd & MMC_CMD_NO_CRC_RESPONSE)) && (Status & MCI_STATUS_CMD_CMDCRCFAIL)) {
        // The CMD1 does not contain CRC. We should ignore the CRC failed Status.
      RetVal = EFI_CRC_ERROR;
      goto Exit;
    } else {
      RetVal = EFI_SUCCESS;
      goto Exit;
    }
  }

Exit:
	//Disable Command Path
	CmdCtrlReg = MmioRead32(MCI_COMMAND_REG);
	MmioWrite32(MCI_COMMAND_REG, (CmdCtrlReg & ~MCI_CPSM_ENABLED));
	return RetVal;
}

EFI_STATUS
MciReceiveResponse (
  IN MMC_RESPONSE_TYPE Type,
  IN UINT32* Buffer
  )
{
  if (Buffer == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if ((Type == MMC_RESPONSE_TYPE_R1) || (Type == MMC_RESPONSE_TYPE_R1b) ||
      (Type == MMC_RESPONSE_TYPE_R3) || (Type == MMC_RESPONSE_TYPE_R6) ||
      (Type == MMC_RESPONSE_TYPE_R7))
  {
    Buffer[0] = MmioRead32(MCI_RESPONSE0_REG);
    Buffer[1] = MmioRead32(MCI_RESPONSE1_REG);
  } else if (Type == MMC_RESPONSE_TYPE_R2) {
    Buffer[0] = MmioRead32(MCI_RESPONSE0_REG);
    Buffer[1] = MmioRead32(MCI_RESPONSE1_REG);
    Buffer[2] = MmioRead32(MCI_RESPONSE2_REG);
    Buffer[3] = MmioRead32(MCI_RESPONSE3_REG);
  }

  return EFI_SUCCESS;
}

EFI_STATUS
MciReadBlockData (
  IN EFI_LBA Lba,
  IN UINTN Length,
  IN UINT32* Buffer
  )
{
  UINTN Loop;
  UINTN Finish;
  UINTN Status;
  EFI_STATUS RetVal;
  UINTN  DataCtrlReg;

  RetVal = EFI_SUCCESS;

  // Read data from the RX FIFO
  Loop   = 0;
  Finish = MMCI0_BLOCKLEN / 4;
  do {
    // Read the Status flags
    Status = MmioRead32(MCI_STATUS_REG);

    // Do eight reads if possible else a single read
    if (Status & MCI_STATUS_CMD_RXFIFOHALFFULL) {
      Buffer[Loop] = MmioRead32(MCI_FIFO_REG);
      Loop++;
      Buffer[Loop] = MmioRead32(MCI_FIFO_REG);
      Loop++;
      Buffer[Loop] = MmioRead32(MCI_FIFO_REG);
      Loop++;
      Buffer[Loop] = MmioRead32(MCI_FIFO_REG);
      Loop++;
      Buffer[Loop] = MmioRead32(MCI_FIFO_REG);
      Loop++;
      Buffer[Loop] = MmioRead32(MCI_FIFO_REG);
      Loop++;
      Buffer[Loop] = MmioRead32(MCI_FIFO_REG);
      Loop++;
      Buffer[Loop] = MmioRead32(MCI_FIFO_REG);
      Loop++;
    } else if (Status & MCI_STATUS_CMD_RXDATAAVAILBL) {
      Buffer[Loop] = MmioRead32(MCI_FIFO_REG);
      Loop++;
    } else {
      //Check for error conditions and timeouts
      if(Status & MCI_STATUS_CMD_DATATIMEOUT) {
        DEBUG ((EFI_D_ERROR, "MciReadBlockData(): TIMEOUT! Response:0x%X Status:0x%x\n",MmioRead32(MCI_RESPONSE0_REG),Status));
        RetVal = EFI_TIMEOUT;
        break;
      } else if(Status & MCI_STATUS_CMD_DATACRCFAIL) {
        DEBUG ((EFI_D_ERROR, "MciReadBlockData(): CRC Error! Response:0x%X Status:0x%x\n",MmioRead32(MCI_RESPONSE0_REG),Status));
        RetVal = EFI_CRC_ERROR;
        break;
      } else if(Status & MCI_STATUS_CMD_START_BIT_ERROR) {
        DEBUG ((EFI_D_ERROR, "MciReadBlockData(): Start-bit Error! Response:0x%X Status:0x%x\n",MmioRead32(MCI_RESPONSE0_REG),Status));
        RetVal = EFI_NO_RESPONSE;
        break;
      }
    }
    //clear RX over run flag
    if(Status & MCI_STATUS_CMD_RXOVERRUN) {
      MmioWrite32(MCI_CLEAR_STATUS_REG, MCI_STATUS_CMD_RXOVERRUN);
    }
  } while ((Loop < Finish));

	//Clear Status flags
	MmioWrite32(MCI_CLEAR_STATUS_REG, 0x7FF);

	//Disable Data path
	DataCtrlReg = MmioRead32(MCI_DATA_CTL_REG);
	MmioWrite32(MCI_DATA_CTL_REG, (DataCtrlReg & 0xFE));

	return RetVal;
}

EFI_STATUS
MciWriteBlockData (
  IN EFI_LBA Lba,
  IN UINTN Length,
  IN UINT32* Buffer
  )
{
  UINTN Loop;
  UINTN Finish;
  UINTN Timer;
  UINTN Status;
  EFI_STATUS RetVal;
  UINTN  DataCtrlReg;

  RetVal = EFI_SUCCESS;

  // Write the data to the TX FIFO
  Loop   = 0;
  Finish = MMCI0_BLOCKLEN / 4;
  Timer  = MMCI0_TIMEOUT * 100;
  do {
    // Read the Status flags
    Status = MmioRead32(MCI_STATUS_REG);

    // Do eight writes if possible else a single write
    if (Status & MCI_STATUS_CMD_TXFIFOHALFEMPTY) {
      MmioWrite32(MCI_FIFO_REG, Buffer[Loop]);
      Loop++;
      MmioWrite32(MCI_FIFO_REG, Buffer[Loop]);
      Loop++;
      MmioWrite32(MCI_FIFO_REG, Buffer[Loop]);
      Loop++;
      MmioWrite32(MCI_FIFO_REG, Buffer[Loop]);
      Loop++;
      MmioWrite32(MCI_FIFO_REG, Buffer[Loop]);
      Loop++;
      MmioWrite32(MCI_FIFO_REG, Buffer[Loop]);
      Loop++;
      MmioWrite32(MCI_FIFO_REG, Buffer[Loop]);
      Loop++;
      MmioWrite32(MCI_FIFO_REG, Buffer[Loop]);
      Loop++;
    } else if ((Status & MCI_STATUS_CMD_TXFIFOEMPTY)) {
        MmioWrite32(MCI_FIFO_REG, Buffer[Loop]);
        Loop++;
    } else {
      //Check for error conditions and timeouts
      if(Status & MCI_STATUS_CMD_DATATIMEOUT) {
        DEBUG ((EFI_D_ERROR, "MciWriteBlockData(): TIMEOUT! Response:0x%X Status:0x%x\n",MmioRead32(MCI_RESPONSE0_REG),Status));
        RetVal = EFI_TIMEOUT;
        goto Exit;
      } else if(Status & MCI_STATUS_CMD_DATACRCFAIL) {
        DEBUG ((EFI_D_ERROR, "MciWriteBlockData(): CRC Error! Response:0x%X Status:0x%x\n",MmioRead32(MCI_RESPONSE0_REG),Status));
        RetVal = EFI_CRC_ERROR;
        goto Exit;
      } else if(Status & MCI_STATUS_CMD_TX_UNDERRUN) {
        DEBUG ((EFI_D_ERROR, "MciWriteBlockData(): TX buffer Underrun! Response:0x%X Status:0x%x, Number of bytes written 0x%x\n",MmioRead32(MCI_RESPONSE0_REG),Status, Loop));
        RetVal = EFI_BUFFER_TOO_SMALL;
        ASSERT(0);
        goto Exit;
      }
    }
  } while (Loop < Finish);

  // Wait for FIFO to drain
  Timer = MMCI0_TIMEOUT * 60;
  Status = MmioRead32(MCI_STATUS_REG);
#ifndef USE_STREAM
  // Single block
  while (((Status & MCI_STATUS_CMD_TXDONE) != MCI_STATUS_CMD_TXDONE) && Timer) {
#else
  // Stream
  while (((Status & MCI_STATUS_CMD_DATAEND) != MCI_STATUS_CMD_DATAEND) && Timer) {
#endif
    NanoSecondDelay(10);
    Status = MmioRead32(MCI_STATUS_REG);
    Timer--;
  }

  if(Timer == 0) {
    DEBUG ((EFI_D_ERROR, "MciWriteBlockData(): Data End timeout Number of bytes written 0x%x\n",Loop));
    ASSERT(Timer > 0);
    return EFI_TIMEOUT;
  }

  //Clear Status flags
  MmioWrite32(MCI_CLEAR_STATUS_REG, 0x7FF);
  if (Timer == 0) {
    RetVal = EFI_TIMEOUT;
  }

Exit:
	//Disable Data path
	DataCtrlReg = MmioRead32(MCI_DATA_CTL_REG);
	MmioWrite32(MCI_DATA_CTL_REG, (DataCtrlReg & 0xFE));
	return RetVal;
}

EFI_STATUS
MciNotifyState (
  IN MMC_STATE State
  )
{
  UINT32      Data32;

  switch(State) {
  case MmcInvalidState:
    ASSERT(0);
    break;
  case MmcHwInitializationState:
    // If device already turn on then restart it
    Data32 = MmioRead32(MCI_POWER_CONTROL_REG);
    if ((Data32 & 0x2) == MCI_POWER_UP) {
      MCI_TRACE("MciNotifyState(MmcHwInitializationState): TurnOff MCI");

      // Turn off
      MmioWrite32(MCI_CLOCK_CONTROL_REG, 0);
      MmioWrite32(MCI_POWER_CONTROL_REG, 0);
      MicroSecondDelay(100);
    }

    MCI_TRACE("MciNotifyState(MmcHwInitializationState): TurnOn MCI");
    // Setup clock
    //  - 0x1D = 29 => should be the clock divider to be less than 400kHz at MCLK = 24Mhz
    MmioWrite32(MCI_CLOCK_CONTROL_REG,0x1D | MCI_CLOCK_ENABLE | MCI_CLOCK_POWERSAVE);
    //MmioWrite32(MCI_CLOCK_CONTROL_REG,0x1D | MCI_CLOCK_ENABLE);

    // Set the voltage
    MmioWrite32(MCI_POWER_CONTROL_REG,MCI_POWER_OPENDRAIN | (15<<2));
    MmioWrite32(MCI_POWER_CONTROL_REG,MCI_POWER_ROD | MCI_POWER_OPENDRAIN | (15<<2) | MCI_POWER_UP);
    MicroSecondDelay(10);
    MmioWrite32(MCI_POWER_CONTROL_REG,MCI_POWER_ROD | MCI_POWER_OPENDRAIN | (15<<2) | MCI_POWER_ON);
    MicroSecondDelay(100);

    // Set Data Length & Data Timer
    MmioWrite32(MCI_DATA_TIMER_REG,0xFFFFF);
    MmioWrite32(MCI_DATA_LENGTH_REG,8);

    ASSERT((MmioRead32(MCI_POWER_CONTROL_REG) & 0x3) == MCI_POWER_ON);
    break;
  case MmcIdleState:
    MCI_TRACE("MciNotifyState(MmcIdleState)");
    break;
  case MmcReadyState:
    MCI_TRACE("MciNotifyState(MmcReadyState)");
    break;
  case MmcIdentificationState:
    MCI_TRACE("MciNotifyState(MmcIdentificationState)");
    break;
  case MmcStandByState:{
    volatile UINT32 PwrCtrlReg;
    MCI_TRACE("MciNotifyState(MmcStandByState)");

    // Enable MCICMD push-pull drive
    PwrCtrlReg = MmioRead32(MCI_POWER_CONTROL_REG);
    //Disable Open Drain output
    PwrCtrlReg &=~(MCI_POWER_OPENDRAIN);
    MmioWrite32(MCI_POWER_CONTROL_REG,PwrCtrlReg);

    // Set MMCI0 clock to 4MHz (24MHz may be possible with cache enabled)
    //
    // Note: Increasing clock speed causes TX FIFO under-run errors.
    //       So careful when optimising this driver for higher performance.
    //
    MmioWrite32(MCI_CLOCK_CONTROL_REG,0x02 | MCI_CLOCK_ENABLE | MCI_CLOCK_POWERSAVE);
    // Set MMCI0 clock to 24MHz (by bypassing the divider)
    //MmioWrite32(MCI_CLOCK_CONTROL_REG,MCI_CLOCK_BYPASS | MCI_CLOCK_ENABLE);
    break;
  }
  case MmcTransferState:
    //MCI_TRACE("MciNotifyState(MmcTransferState)");
    break;
  case MmcSendingDataState:
    MCI_TRACE("MciNotifyState(MmcSendingDataState)");
    break;
  case MmcReceiveDataState:
    MCI_TRACE("MciNotifyState(MmcReceiveDataState)");
    break;
  case MmcProgrammingState:
    MCI_TRACE("MciNotifyState(MmcProgrammingState)");
    break;
  case MmcDisconnectState:
    MCI_TRACE("MciNotifyState(MmcDisconnectState)");
    break;
  default:
    ASSERT(0);
  }
  return EFI_SUCCESS;
}

EFI_GUID mPL180MciDevicePathGuid = { 0x621b6fa5, 0x4dc1, 0x476f, 0xb9, 0xd8, 0x52, 0xc5, 0x57, 0xd8, 0x10, 0x70 };

EFI_STATUS
MciBuildDevicePath (
  IN EFI_DEVICE_PATH_PROTOCOL **DevicePath
  )
{
  EFI_DEVICE_PATH_PROTOCOL    *NewDevicePathNode;

  NewDevicePathNode = CreateDeviceNode(HARDWARE_DEVICE_PATH,HW_VENDOR_DP,sizeof(VENDOR_DEVICE_PATH));
  CopyGuid(&((VENDOR_DEVICE_PATH*)NewDevicePathNode)->Guid,&mPL180MciDevicePathGuid);

  *DevicePath = NewDevicePathNode;
  return EFI_SUCCESS;
}

EFI_MMC_HOST_PROTOCOL gMciHost = {
  MciIsCardPresent,
  MciIsReadOnly,
  MciBuildDevicePath,
  MciNotifyState,
  MciSendCommand,
  MciReceiveResponse,
  MciReadBlockData,
  MciWriteBlockData
};

EFI_STATUS
PL180MciDxeInitialize (
  IN EFI_HANDLE         ImageHandle,
  IN EFI_SYSTEM_TABLE   *SystemTable
  )
{
  EFI_STATUS    Status;
  EFI_HANDLE    Handle = NULL;

  MCI_TRACE("PL180MciDxeInitialize()");

  //Publish Component Name, BlockIO protocol interfaces
  Status = gBS->InstallMultipleProtocolInterfaces (
                  &Handle, 
                  &gEfiMmcHostProtocolGuid,         &gMciHost,
                  NULL
                  );
  ASSERT_EFI_ERROR (Status);

  return EFI_SUCCESS;
}
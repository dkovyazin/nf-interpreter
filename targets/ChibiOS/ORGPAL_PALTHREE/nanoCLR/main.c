//
// Copyright (c) .NET Foundation and Contributors
// See LICENSE file in the project root for full license information.
//

#include <ch.h>
#include <hal.h>
#include <hal_nf_community.h>
#include <cmsis_os.h>

#include "usbcfg.h"
#include <swo.h>
#include <CLR_Startup_Thread.h>
#include <WireProtocol_ReceiverThread.h>
#include <nanoCLR_Application.h>
#include <nanoPAL_BlockStorage.h>
#include <nanoHAL_v2.h>
#include <targetPAL.h>

extern int32_t hal_lfs_config();
extern void hal_lfs_mount();
extern void Target_ConfigMPU();

// need to declare the Receiver thread here
osThreadDef(ReceiverThread, osPriorityHigh, 2048, "ReceiverThread");
// declare CLRStartup thread here
osThreadDef(CLRStartupThread, osPriorityNormal, 6144, "CLRStartupThread");

#if HAL_USE_SDC
// declare SD Card working thread here
osThreadDef(SdCardWorkingThread, osPriorityNormal, 1024, "SDCWT");
#endif
#if HAL_USBH_USE_MSD
// declare USB MSD thread here
osThreadDef(UsbMsdWorkingThread, osPriorityNormal, 1024, "USBMSDWT");
#endif

//  Application entry point.
int main(void)
{
    // find out wakeup reason
    if ((PWR->CSR1 & PWR_CSR1_WUIF) == PWR_CSR1_WUIF)
    {
        // standby, match WakeupReason_FromStandby enum
        WakeupReasonStore = 1;
    }
    else if ((PWR->CSR2 & (PWR_CSR2_WUPF3 | PWR_CSR2_WUPF2 | PWR_CSR2_WUPF1)))
    {
        // wake from pin, match WakeupReason_FromPin enum
        WakeupReasonStore = 2;
    }
    else
    {
        // undetermined reason, match WakeupReason_Undetermined enum
        WakeupReasonStore = 0;
    }

    // first things first: need to clear any possible wakeup flags
    // clear wakeup flags from GPIOs
    PWR->CR2 |= (PWR_CR2_CWUPF6 | PWR_CR2_CWUPF5 | PWR_CR2_CWUPF4 | PWR_CR2_CWUPF3 | PWR_CR2_CWUPF2 | PWR_CR2_CWUPF1);
    // clear standby Flag
    PWR->CR1 |= PWR_CR1_CSBF;

    __ISB();

    // HAL initialization, this also initializes the configured device drivers
    // and performs the board-specific initializations.
    halInit();

    // init boot clipboard
    InitBootClipboard();

    // set default values for GPIOs
    palClearPad(GPIOE, GPIOE_PIN4);
    palClearLine(LINE_RELAY);
    palSetPad(GPIOJ, GPIOJ_PIN13);
    palClearPad(GPIOJ, GPIOJ_PIN14);
    palClearLine(LINE_LCD_ENABLE);

// init SWO as soon as possible to make it available to output ASAP
#if (SWO_OUTPUT == TRUE)
    SwoInit();
#endif

    // The kernel is initialized but not started yet, this means that
    // main() is executing with absolute priority but interrupts are already enabled.
    osKernelInitialize();

    // start watchdog
    Watchdog_Init();

#if (HAL_NF_USE_STM32_CRC == TRUE)
    // startup crc
    crcStart(NULL);
#endif

    // MPU configuration
    Target_ConfigMPU();

    // config and init external memory
    // this has to be called after osKernelInitialize, otherwise an hard fault will occur
    Target_ExternalMemoryInit();

    //  Initializes a serial-over-USB CDC driver.
    sduObjectInit(&SERIAL_DRIVER);
    sduStart(&SERIAL_DRIVER, &serusbcfg);

    // Activates the USB driver and then the USB bus pull-up on D+.
    // Note, a delay is inserted in order to not have to disconnect the cable after a reset
    usbDisconnectBus(serusbcfg.usbp);
    chThdSleepMilliseconds(100);
    usbStart(serusbcfg.usbp, &usbcfg);
    usbConnectBus(serusbcfg.usbp);

#if (NF_FEATURE_USE_LITTLEFS == TRUE)
    // config and init littlefs
    hal_lfs_config();
    hal_lfs_mount();
#endif

    // create the receiver thread
    osThreadCreate(osThread(ReceiverThread), NULL);

    // CLR settings to launch CLR thread
    CLR_SETTINGS clrSettings;
    (void)memset(&clrSettings, 0, sizeof(CLR_SETTINGS));

    clrSettings.MaxContextSwitches = 50;
    clrSettings.WaitForDebugger = false;
    clrSettings.EnterDebuggerLoopAfterExit = true;

    // create the CLR Startup thread
    osThreadCreate(osThread(CLRStartupThread), &clrSettings);

#if HAL_USE_SDC
    // creates the SD card working thread
    osThreadCreate(osThread(SdCardWorkingThread), NULL);
#endif

#if HAL_USBH_USE_MSD
    // create the USB MSD working thread
    osThreadCreate(osThread(UsbMsdWorkingThread), NULL);
#endif

    // start kernel, after this main() will behave like a thread with priority osPriorityNormal
    osKernelStart();

    while (true)
    {
        osDelay(100);
    }
}

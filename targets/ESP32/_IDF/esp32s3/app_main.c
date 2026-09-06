//
// Copyright (c) .NET Foundation and Contributors
// See LICENSE file in the project root for full license information.
//

#include <targetHAL.h>
#include <nanoCLR_Application.h>
#include <target_os.h>
#include <WireProtocol_ReceiverThread.h>
#include <LaunchCLR.h>
#include <string.h>

#if CONFIG_NF_FEATURE_OTA
#include <targetHAL_Ota.h>
#endif

extern void CLRStartupThread(void const *argument);
TaskHandle_t ReceiverTask;

void receiver_task(void *pvParameter)
{
    (void)pvParameter;

    ReceiverThread(0);

    vTaskDelete(NULL);
}

// Main task start point
void main_task(void *pvParameter)
{
    (void)pvParameter;

    // CLR settings to launch CLR thread
    CLR_SETTINGS clrSettings;
    (void)memset(&clrSettings, 0, sizeof(CLR_SETTINGS));

    clrSettings.MaxContextSwitches = 50;
    clrSettings.WaitForDebugger = false;
    clrSettings.EnterDebuggerLoopAfterExit = true;

    CLRStartupThread(&clrSettings);

    vTaskDelete(NULL);
}

#if CONFIG_NF_BUILD_RTM
// Dummy defauly log method to stop output from ESP32 IDF
int dummyLog(const char *format, va_list arg)
{
    (void)format;
    (void)arg;
    return 1;
}
#endif

// App_main
// Called from Esp32 IDF start up code before scheduler starts
void app_main()
{
#if CONFIG_NF_BUILD_RTM
    // Switch off logging so as not to interfere with WireProtocol over Uart0
    esp_log_level_set("*", ESP_LOG_NONE);

    // Stop any logging being directed to VS connection, was an issue with Nimble, outputting on Uart0
    // TODO : redirect these to debugger controlled from nanoframework.Hardware.Esp32
    esp_log_set_vprintf(dummyLog);
#endif

    // recover from a full or corrupted NVS instead of ESP_ERROR_CHECK-panicking
    // into a boot loop; the OTA state lives in its own partition and is not
    // affected by the erase
    esp_err_t nvsResult = nvs_flash_init();
    if (nvsResult == ESP_ERR_NVS_NO_FREE_PAGES || nvsResult == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvsResult = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvsResult);

#if CONFIG_NF_FEATURE_OTA
    // apply/rollback a staged OTA deployment image; must run before the CLR
    // starts, while the deploy partition is not in use
    NF_Ota_ApplyPending();
#endif

    // start receiver task
    // 6 kB stack: Monitor_StorageOperation runs FATFS+SDMMC writes on this task
    // (nanoff --filedeployment); the FAT allocation/flush path overflows 3 kB
    // LEDTREES: the receiver is needed by the rollout recipe delivery
    // (HaltedFileDeploy: pause/resume plus AddStorageFile); the dangerous
    // commands (WriteMemory/EraseMemory/Execute - deploying and running code)
    // are stripped from the RTM command table in
    // src/CLR/Debugger/Debugger_minimal.cpp
    xTaskCreatePinnedToCore(&receiver_task, "ReceiverThread", 6144, NULL, 5, &ReceiverTask, 0);

    // start the CLR main task
    // LEDTREES: core 1, not upstream's core 0 - moves the CLR away from Wi-Fi,
    // BLE, SDMMC and the frame-feed task, which all live on core 0; its only
    // neighbour is the lightweight LedTask (same scheme as the classic ESP32
    // target). Pinning itself is required (upstream PR #2695, SPIFFS init bug
    // with unpinned tasks) - the core choice was arbitrary there.
    xTaskCreatePinnedToCore(&main_task, "main_task", 15000, NULL, 5, NULL, 1);
}

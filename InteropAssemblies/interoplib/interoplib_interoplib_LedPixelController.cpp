//-----------------------------------------------------------------------------
//
//                   ** WARNING! **
//    This file was generated automatically by a tool.
//    Re-running the tool will overwrite this file.
//    You should copy this file to a custom location
//    before adding any customization in the copy to
//    prevent loss of your changes when the tool is
//    re-run.
//
//-----------------------------------------------------------------------------

#include "interoplib.h"
#include "interoplib_interoplib_LedPixelController.h"
#include "esp_log.h"
#include "interoplib_config.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <stdio.h>
#include <string.h>

using namespace interoplib::interoplib;

static const int FRAME_BUFFER_COUNT = 15;
static const char* TAG = "LedPixelController";

static TaskHandle_t Task1;
static TaskHandle_t ReaderTask;
static spi_device_handle_t spi = NULL;
static spi_host_device_t SPI_HOST = SPI2_HOST;
static DMA_ATTR uint8_t DATA_BUFFER[BUFF_SIZE];
static DMA_ATTR uint8_t FRAME_BUFFERS[FRAME_BUFFER_COUNT][BUFF_SIZE];
static int LEDS_COUNT = 0;
static int INIT_BUFFER_SIZE = 0;

static volatile int writeFrameIndex = 0;
static volatile int readFrameIndex = 0;
static volatile int framesAvailable = 0;
static SemaphoreHandle_t frameMutex = NULL;
static SemaphoreHandle_t frameReadySemaphore = NULL;
static SemaphoreHandle_t frameEmptySemaphore = NULL;

static volatile bool isPlayingFromFile = false;
static volatile bool stopPlayback = false;
static volatile bool pausePlayback = false;
static FILE* currentVideoFile = NULL;
static int targetFps = PROGRAM_TRANSITION_FPS;
static int totalFramesToPlay = 0;
static int framesPlayed = 0;

void LedPixelController::NativeInit( signed int mosiPin, signed int misoPin, signed int clkPin, signed int csPin, signed int pixelCount, uint8_t red, uint8_t green, uint8_t blue, HRESULT &hr  )
{
    if (LEDS_COUNT > 400) {
        hr = S_FALSE;
        return;
    }

    esp_err_t ret;

    if (spi != NULL) {
        ret = spi_bus_remove_device(spi);
        ESP_ERROR_CHECK(ret);

        ret = spi_bus_free(SPI_HOST);
        ESP_ERROR_CHECK(ret);

        spi = NULL;
        INIT_BUFFER_SIZE = 0;
    }

    LEDS_COUNT = pixelCount;
    INIT_BUFFER_SIZE = 4 * pixelCount * 3;

    if (frameMutex == NULL) {
        frameMutex = xSemaphoreCreateMutex();
    }
    if (frameReadySemaphore == NULL) {
        frameReadySemaphore = xSemaphoreCreateCounting(FRAME_BUFFER_COUNT, 0);
    }
    if (frameEmptySemaphore == NULL) {
        frameEmptySemaphore = xSemaphoreCreateCounting(FRAME_BUFFER_COUNT, FRAME_BUFFER_COUNT);
    }

    spi_bus_config_t bus_cfg {
        mosi_io_num: 		mosiPin,
        miso_io_num: 		misoPin,
        sclk_io_num: 		clkPin,
        quadwp_io_num:  	-1,
        quadhd_io_num:  	-1,
        data4_io_num:       -1,
        data5_io_num:       -1,
        data6_io_num:       -1,
        data7_io_num:       -1,
        max_transfer_sz:	INIT_BUFFER_SIZE,
        flags:              0,
        isr_cpu_id:         ESP_INTR_CPU_AFFINITY_1,
        intr_flags:         0
    };

    ret = spi_bus_initialize(SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO); // инициализируем и включаем DMA для spi
    ESP_ERROR_CHECK(ret);

    spi_device_interface_config_t dev_cfg {
        0,                   // Command bits
        0,                   // Address bits
        0,                   // Dummy bits
        0,                   // SPi Mode
        SPI_CLK_SRC_DEFAULT, // Clock source
        0,                   // Duty cycle 50/50
        0,                   // cs_ena_pretrans
        0,                   // cs_ena_posttrans
        SPI_LEDS_FREQ_HZ,         // Clock speed in Hz
        0,                   // Input_delay_ns
        SPI_SAMPLING_POINT_PHASE_0,
        csPin,               // Chip select, we will use manual chip select
        0,                   // SPI_DEVICE flags
        7,                   // Queue size
        0,                   // Callback before
        0,                   // Callback after transaction complete
    };

    ret = spi_bus_add_device(SPI_HOST, &dev_cfg, &spi);
    ESP_ERROR_CHECK(ret);

    LedPixelController::NativeSetFull(red, green, blue, hr);
    if (hr != S_OK)
        return;

    xTaskCreatePinnedToCore(
        Task1code,                                  /* Функция задачи. */
        "Task1",                                    /* Ее имя. */
        4096,                                       /* Размер стека функции */
        NULL,                                       /* Параметры */
        CONFIG_ESP32_PTHREAD_TASK_PRIO_DEFAULT,     /* Приоритет */
        &Task1,                                     /* Дескриптор задачи для отслеживания */
        1);

    //spi_send_data(DATA_BUFFER, INIT_BUFFER_SIZE);

    hr = S_OK;
}

void LedPixelController::NativeWrite( CLR_RT_TypedArray_UINT8 data, HRESULT &hr )
{
    if (spi == NULL) {
        hr = CLR_E_BUSY;
        return;
    }

    memcpy(DATA_BUFFER, (void*)data.GetBuffer(), INIT_BUFFER_SIZE);

    hr = S_OK;
}

void LedPixelController::NativeSetFull( uint8_t red, uint8_t green, uint8_t blue, HRESULT &hr )
{
    for (int i = 0 ; i < LEDS_COUNT * STRIPS_CNT; ++i) {
		DATA_BUFFER[i*3+0] = red;
		DATA_BUFFER[i*3+1] = green;
		DATA_BUFFER[i*3+2] = blue;
	}

    hr = S_OK;
}

void LedPixelController::NativeSetPixel( uint8_t line, uint16_t cell, uint8_t red, uint8_t green, uint8_t blue, HRESULT &hr )
{
    int i = (cell * 3 * 4) + (line * 3);
    DATA_BUFFER[i + 0] = red;
    DATA_BUFFER[i + 1] = green;
    DATA_BUFFER[i + 2] = blue;

    hr = S_OK;
}

void LedPixelController::NativePlayFromFile( CLR_RT_TypedArray_UINT8 filePath, signed int fps, signed int frameCount, HRESULT &hr )
{
    if (spi == NULL) {
        ESP_LOGE(TAG, "SPI not initialized");
        hr = CLR_E_NOT_SUPPORTED;
        return;
    }

    if (isPlayingFromFile) {
        ESP_LOGW(TAG, "Already playing, stopping previous playback");
        LedPixelController::NativeStopPlayback(hr);
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    char* path = (char*)filePath.GetBuffer();
    ESP_LOGI(TAG, "Opening file: %s, fps: %d, frames: %d", path, fps, frameCount);

    currentVideoFile = fopen(path, "rb");
    if (currentVideoFile == NULL) {
        ESP_LOGE(TAG, "Failed to open file: %s", path);
        hr = CLR_E_FILE_NOT_FOUND;
        return;
    }

    writeFrameIndex = 0;
    readFrameIndex = 0;
    framesAvailable = 0;
    framesPlayed = 0;
    targetFps = (fps > 0) ? fps : PROGRAM_TRANSITION_FPS;
    totalFramesToPlay = frameCount;
    stopPlayback = false;
    pausePlayback = false;
    isPlayingFromFile = true;

    if (frameReadySemaphore != NULL) {
        xQueueReset(frameReadySemaphore);
    }
    if (frameEmptySemaphore != NULL) {
        xQueueReset(frameEmptySemaphore);
        for (int i = 0; i < FRAME_BUFFER_COUNT; i++) {
            xSemaphoreGive(frameEmptySemaphore);
        }
    }

    BaseType_t result = xTaskCreatePinnedToCore(
        ReaderTaskCode,
        "VideoReader",
        8192,
        NULL,
        CONFIG_ESP32_PTHREAD_TASK_PRIO_DEFAULT + 1,
        &ReaderTask,
        0);

    if (result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create reader task");
        fclose(currentVideoFile);
        currentVideoFile = NULL;
        isPlayingFromFile = false;
        hr = CLR_E_FAIL;
        return;
    }

    ESP_LOGI(TAG, "Playback started");
    hr = S_OK;
}

void LedPixelController::NativeStopPlayback( HRESULT &hr )
{
    if (!isPlayingFromFile) {
        hr = S_OK;
        return;
    }

    ESP_LOGI(TAG, "Stopping playback");
    stopPlayback = true;

    int timeout = 50;
    while (isPlayingFromFile && timeout > 0) {
        vTaskDelay(pdMS_TO_TICKS(100));
        timeout--;
    }

    if (ReaderTask != NULL) {
        vTaskDelete(ReaderTask);
        ReaderTask = NULL;
    }

    if (currentVideoFile != NULL) {
        fclose(currentVideoFile);
        currentVideoFile = NULL;
    }

    isPlayingFromFile = false;
    ESP_LOGI(TAG, "Playback stopped");
    hr = S_OK;
}

bool LedPixelController::NativeIsPlaying( HRESULT &hr )
{
    hr = S_OK;
    return isPlayingFromFile;
}

void LedPixelController::NativePausePlayback( HRESULT &hr )
{
    if (!isPlayingFromFile) {
        hr = S_OK;
        return;
    }

    ESP_LOGI(TAG, "Pausing playback");
    pausePlayback = true;
    hr = S_OK;
}

void LedPixelController::NativeResumePlayback( HRESULT &hr )
{
    if (!isPlayingFromFile) {
        hr = S_OK;
        return;
    }

    ESP_LOGI(TAG, "Resuming playback");
    pausePlayback = false;
    hr = S_OK;
}

signed int LedPixelController::NativeGetFramesPlayed( HRESULT &hr )
{
    hr = S_OK;
    return framesPlayed;
}

void ReaderTaskCode(void* pvParameters) {
    ESP_LOGI(TAG, "Reader task started");

    while(!stopPlayback && framesPlayed < totalFramesToPlay) {
        if (pausePlayback) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (xSemaphoreTake(frameEmptySemaphore, pdMS_TO_TICKS(1000)) == pdTRUE) {
            size_t bytesRead = fread(FRAME_BUFFERS[writeFrameIndex], 1, INIT_BUFFER_SIZE, currentVideoFile);
            
            if (bytesRead == INIT_BUFFER_SIZE) {
                xSemaphoreTake(frameMutex, portMAX_DELAY);
                writeFrameIndex = (writeFrameIndex + 1) % FRAME_BUFFER_COUNT;
                framesAvailable++;
                xSemaphoreGive(frameMutex);
                
                xSemaphoreGive(frameReadySemaphore);
            }
            else {
                if (feof(currentVideoFile)) {
                    stopPlayback = true;
                }
                else {
                    ESP_LOGE(TAG, "Read error, bytes: %d", bytesRead);
                }
                xSemaphoreGive(frameEmptySemaphore);
                break;
            }
        }
    }

    if (currentVideoFile != NULL) {
        fclose(currentVideoFile);
        currentVideoFile = NULL;
    }

    isPlayingFromFile = false;
    ESP_LOGI(TAG, "Reader task finished, frames played: %d", framesPlayed);
    vTaskDelete(NULL);
}

void Task1code(void* pvParameters) {
    TickType_t lastWakeTime = xTaskGetTickCount();
    TickType_t frameDelay = pdMS_TO_TICKS(1000 / targetFps);

    while(1) {
        if (isPlayingFromFile) {
            if (pausePlayback) {
                vTaskDelay(pdMS_TO_TICKS(50));
                lastWakeTime = xTaskGetTickCount();
                continue;
            }

            frameDelay = pdMS_TO_TICKS(1000 / targetFps);

            if (xSemaphoreTake(frameReadySemaphore, pdMS_TO_TICKS(100)) == pdTRUE) {
                xSemaphoreTake(frameMutex, portMAX_DELAY);
                int currentReadIndex = readFrameIndex;
                readFrameIndex = (readFrameIndex + 1) % FRAME_BUFFER_COUNT;
                framesAvailable--;
                framesPlayed++;
                xSemaphoreGive(frameMutex);

                spi_send_data(FRAME_BUFFERS[currentReadIndex], INIT_BUFFER_SIZE);

                xSemaphoreGive(frameEmptySemaphore);

                vTaskDelayUntil(&lastWakeTime, frameDelay);
            }
            else {
                if (stopPlayback && framesAvailable == 0) {
                    ESP_LOGI(TAG, "Playback finished");
                    isPlayingFromFile = false;
                }
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        }
        else {
            spi_send_data(DATA_BUFFER, INIT_BUFFER_SIZE);
            vTaskDelay(1);
        }
    }

    vTaskDelete(NULL);
}

void spi_send_data(const uint8_t *data, int len) {
	int offset = 0;
	do {
		spi_transaction_t t;
		memset(&t, 0, sizeof(t));

		int tx_len = ((len - offset) < SPI_MAX_DMA_LEN) ? (len - offset) : SPI_MAX_DMA_LEN;
		t.length = tx_len * 8;
		t.tx_buffer = data + offset;
		ESP_ERROR_CHECK(spi_device_polling_transmit(spi, &t));
		offset += tx_len;
	} while (offset < len);

    vTaskDelay(1);
}

void spi_send_data2()
{
	esp_err_t ret;

    spi_transaction_t t;
	memset(&t, 0, sizeof(t));

	t.length = INIT_BUFFER_SIZE * 8;
	t.tx_buffer = DATA_BUFFER;
	ret = spi_device_polling_transmit(spi, &t);
	assert(ret == ESP_OK);
}

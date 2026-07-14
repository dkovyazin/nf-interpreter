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
#include <task.h>

#define CS_BEGIN(s) assert(xSemaphoreTake(s, portMAX_DELAY) == pdTRUE)
#define CS_END(s) assert(xSemaphoreGive(s) == pdTRUE)

using namespace interoplib::interoplib;

static TaskHandle_t LedTask;
static spi_device_handle_t spi = NULL;
static spi_host_device_t SPI_HOST = SPI2_HOST;
static DMA_ATTR uint8_t FRAME_BUFFER[BUFF_SIZE];
static uint16_t PREPARED_FRAMES = 0;
static short BUFFERED_FRAMES = 0;
static DMA_ATTR uint8_t* PREPARE_BUFFERS;    // буфер для подготовки кадров перед воспроизведением
static DMA_ATTR uint8_t* PROGRAM1_BUFFERS;   // 1-й буфер воспроизведения
static DMA_ATTR uint8_t* PROGRAM2_BUFFERS;   // 2-й буфер воспроизведения
static uint8_t CURRENT_PLAY_BUFFER = 1;
static int LEDS_COUNT = 0;
static int FRAME_SIZE = 0;

static volatile bool running = false;
static volatile bool requestedForStop = false;
static SemaphoreHandle_t bodySemaphore = NULL;
static SemaphoreHandle_t joinSemaphore = NULL;
static SemaphoreHandle_t writeSemaphore = NULL;

struct LedTaskParams {
    uint8_t fps;
    uint16_t countFrames;
    uint16_t transition;
};
static LedTaskParams params;

static volatile uint8_t brightness = 255;

class Transition {
    public:
        uint8_t current;
        uint8_t lenght;
        uint8_t from[BUFF_SIZE];
        uint8_t to[BUFF_SIZE];

        bool getNextFrame(uint8_t* frameData) {
            if (current < lenght) {
                for (int i = 0; i < BUFF_SIZE; ++i) {
                    uint8_t a = (current +1)*0xFF / lenght;
                    frameData[i] = ((from[i]*(0xFF-a) + to[i]*a))/0xFF;
                }
                ++current;
                return true;
            } else {
                return false;
            }
        }
};

static uint8_t* lastRawFrame;
static Transition transitionFrame;

void LedPixelController::NativeInit( signed int mosiPin, signed int misoPin, signed int clkPin, signed int csPin, signed int pixelCount, uint8_t red, uint8_t green, uint8_t blue, HRESULT &hr  )
{
    if (spi != NULL || pixelCount > 400) {
        hr = S_FALSE;
        return;
    }

    LEDS_COUNT = pixelCount;
    FRAME_SIZE = pixelCount * 4 * 3;

    PREPARE_BUFFERS = new uint8_t[BUFFER_FRAMES_COUNT * FRAME_SIZE];
    PROGRAM1_BUFFERS = new uint8_t[BUFFER_FRAMES_COUNT * FRAME_SIZE];
    PROGRAM2_BUFFERS = new uint8_t[BUFFER_FRAMES_COUNT * FRAME_SIZE];

    memset(PREPARE_BUFFERS, 0, BUFFER_FRAMES_COUNT * FRAME_SIZE);
    memset(PROGRAM1_BUFFERS, 0, BUFFER_FRAMES_COUNT * FRAME_SIZE);
    memset(PROGRAM2_BUFFERS, 0, BUFFER_FRAMES_COUNT * FRAME_SIZE);

    lastRawFrame = new uint8_t[FRAME_SIZE];

    esp_err_t ret;

    if (spi != NULL) {
        ret = spi_bus_remove_device(spi);
        ESP_ERROR_CHECK(ret);

        ret = spi_bus_free(SPI_HOST);
        ESP_ERROR_CHECK(ret);

        spi = NULL;
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
        data_io_default_level: 0,
        max_transfer_sz:	FRAME_SIZE,
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
        SPI_SAMPLING_POINT_PHASE_0, // Sampling point (new in IDF 5.5)
        csPin,               // Chip select, we will use manual chip select
        0,                   // SPI_DEVICE flags
        7,                   // Queue size
        0,                   // Callback before
        0,                   // Callback after transaction complete
    };

    ret = spi_bus_add_device(SPI_HOST, &dev_cfg, &spi);
    ESP_ERROR_CHECK(ret);

    writeSemaphore = xSemaphoreCreateMutex();
    vSemaphoreCreateBinary(bodySemaphore);
    vSemaphoreCreateBinary(joinSemaphore);

    LedPixelController::NativeSetFull(red, green, blue, hr);
    if (hr != S_OK)
        return;

    hr = S_OK;
}

void LedPixelController::NativeSetBrightness( uint8_t value, HRESULT &hr )
{
    brightness = value;

    hr = S_OK;
}

void LedPixelController::NativePrepareForPlay( uint16_t frame, CLR_RT_TypedArray_UINT8 data, HRESULT &hr )
{
    if (spi == NULL) {
        hr = S_FALSE;
        return;
    }

    if (frame >= BUFFER_FRAMES_COUNT) {
        hr = S_FALSE;
        return;
    }

    int offset = frame * FRAME_SIZE;
    uint8_t* frameData = (uint8_t*)data.GetBuffer();
    memcpy(PREPARE_BUFFERS + offset, frameData, FRAME_SIZE);

    if (frame == 0)
        PREPARED_FRAMES = 1; // сбрасываем счётчик подготовленных фреймов
    else
        PREPARED_FRAMES++;

    hr = S_OK;
}

void LedPixelController::NativeStartPlay( uint16_t countFrames, uint8_t fps, uint16_t transition, HRESULT &hr )
{
    if (spi == NULL) {
        hr = S_FALSE;
        return;
    }

    if (countFrames == 0 ||  fps < 1) {
        hr = S_FALSE;
        return;
    }

    LedTask_Stop();
    LedTask_Join();

    LedTask_Start(countFrames, fps, transition);

    hr = S_OK;
}

void LedPixelController::NativeWriteToPlayBuffer( uint16_t frame, CLR_RT_TypedArray_UINT8 data, HRESULT &hr )
{
    if (spi == NULL) {
        hr = S_FALSE;
        return;
    }

    uint8_t* buffer;
    if (CURRENT_PLAY_BUFFER == 1)
        buffer = PROGRAM2_BUFFERS;
    else
        buffer = PROGRAM1_BUFFERS;

    int offset = frame * FRAME_SIZE;
    uint8_t* frameData = (uint8_t*)data.GetBuffer();
    memcpy(buffer + offset, frameData, FRAME_SIZE);

    if (frame == 0)
        BUFFERED_FRAMES = 1; // сбрасываем счётчик фуфферизованных фреймов воспроизведения
    else
        BUFFERED_FRAMES++;

    hr = S_OK;
}

void LedPixelController::NativeWrite( CLR_RT_TypedArray_UINT8 data, HRESULT &hr )
{
    if (spi == NULL) {
        hr = S_FALSE;
        return;
    }

    if (spi == NULL) {
        hr = CLR_E_BUSY;
        return;
    }

    LedTask_Stop();
    LedTask_Join();

    xSemaphoreTake(bodySemaphore, portMAX_DELAY);

    // send exactly the managed array length (capped at frame size):
    // copying FRAME_SIZE unconditionally read past the end of shorter arrays
    int length = (int)data.GetSize();
    if (length > FRAME_SIZE)
        length = FRAME_SIZE;

    memcpy(FRAME_BUFFER, (void*)data.GetBuffer(), length);

    spi_send_data(FRAME_BUFFER, length);

    xSemaphoreGive(bodySemaphore);

    hr = S_OK;
}

void LedPixelController::NativeSetFull( uint8_t red, uint8_t green, uint8_t blue, HRESULT &hr )
{
    if (spi == NULL) {
        hr = S_FALSE;
        return;
    }

    LedTask_Stop();
    LedTask_Join();

    xSemaphoreTake(bodySemaphore, portMAX_DELAY);

    for (int i = 0 ; i < LEDS_COUNT * STRIPS_CNT; ++i) {
		FRAME_BUFFER[i * 3 + 0] = red;
		FRAME_BUFFER[i * 3 + 1] = green;
		FRAME_BUFFER[i * 3 + 2] = blue;
	}

    spi_send_data(FRAME_BUFFER, FRAME_SIZE);

    xSemaphoreGive(bodySemaphore);

    hr = S_OK;
}

void LedPixelController::NativeSetPixel( uint8_t line, uint16_t cell, uint8_t red, uint8_t green, uint8_t blue, HRESULT &hr )
{
    if (spi == NULL) {
        hr = S_FALSE;
        return;
    }

    LedTask_Stop();
    LedTask_Join();

    xSemaphoreTake(bodySemaphore, portMAX_DELAY);

    int i = (cell * 3 * 4) + (line * 3);
    FRAME_BUFFER[i + 0] = red;
    FRAME_BUFFER[i + 1] = green;
    FRAME_BUFFER[i + 2] = blue;

    spi_send_data(FRAME_BUFFER, FRAME_SIZE);

    xSemaphoreGive(bodySemaphore);

    hr = S_OK;
}

void LedTask_Start(uint16_t countFrames, uint8_t fps, uint16_t transition)
{
    CS_BEGIN(bodySemaphore);

    if (!running) {
        params.fps = fps;
        params.countFrames = countFrames;
        params.transition = transition;

        xTaskCreatePinnedToCore(
            LedTask_Handler,                                  /* Функция задачи. */
            "LedTask",                                    /* Ее имя. */
            4096,                                       /* Размер стека функции */
            (void*) &params,                                       /* Параметры */
            CONFIG_ESP32_PTHREAD_TASK_PRIO_DEFAULT,     /* Приоритет */
            &LedTask,                                     /* Дескриптор задачи для отслеживания */
            1);

    }

    CS_END(bodySemaphore);
}

void LedTask_Stop()
{
    CS_BEGIN(bodySemaphore);

    if (running) {
        requestedForStop = true;
        assert(xSemaphoreTake(joinSemaphore, portMAX_DELAY) == pdTRUE);
    }

    CS_END(bodySemaphore);
}

void LedTask_Join()
{
    assert(xSemaphoreTake(joinSemaphore, portMAX_DELAY) == pdTRUE);
    assert(xSemaphoreGive(joinSemaphore) == pdTRUE);
}

void LedTask_Handler( void * pvParameters )
{
    LedTaskParams* taskParams = (LedTaskParams*) pvParameters;

    uint16_t transitionTime = taskParams->transition;
    if (1000 / taskParams->fps < transitionTime) {
        transitionFrame.lenght = transitionTime / (1000 / PROGRAM_TRANSITION_FPS);
        transitionFrame.lenght += 1; // for first frame of next program
        transitionFrame.current = 0;

        memcpy(transitionFrame.to, PREPARE_BUFFERS, FRAME_SIZE);
        memcpy(transitionFrame.from, lastRawFrame, FRAME_SIZE);
    } else
        transitionFrame.lenght = 0;

    CS_BEGIN(bodySemaphore);
    running = true;
    const TickType_t programTimeIncrement = 1000.0 / taskParams->fps / portTICK_PERIOD_MS;
    const TickType_t transitionTimeIncrement = 1000.0 / PROGRAM_TRANSITION_FPS / portTICK_PERIOD_MS;
    CS_END(bodySemaphore);

	TickType_t lastWakeTime = xTaskGetTickCount();
    //TickType_t frameDelay = pdMS_TO_TICKS(1000 / taskParams->fps);

    // выбираем буфер воспроизведения, куда скопируем буфер подготовки
    uint8_t* buffer;
    if (CURRENT_PLAY_BUFFER == 1) {
        buffer = PROGRAM2_BUFFERS;
        CURRENT_PLAY_BUFFER = 2;
    }
    else {
        buffer = PROGRAM1_BUFFERS;
        CURRENT_PLAY_BUFFER = 1;
    }

    // копируем буфер подготовки в буфер воспроизведения
    memcpy(buffer, PREPARE_BUFFERS, BUFFER_FRAMES_COUNT * FRAME_SIZE);
    uint16_t bufferFramesCount = PREPARED_FRAMES;

    uint32_t frameIndex = 0;
    uint16_t bufferFrameIndex = 0;
    while (1) {
        CS_BEGIN(bodySemaphore);

        if (bufferFrameIndex == 0) {
            BUFFERED_FRAMES = -1; // сбрасываем счётчик буфера воспроизведения

            if (taskParams->countFrames > BUFFER_FRAMES_COUNT) {
                // уведомляем о том, что нужно готовить следующую порцию кадров
                // передаём index следующего кадра, с которого нужно буфферизовать их
                PostManagedEvent(EVENT_CUSTOM, 0, 1, frameIndex + bufferFramesCount);
            }
        }
        else if(bufferFrameIndex == bufferFramesCount) {
            // вычитали весь буфер воспроизведения

            // vTaskDelay(pdMS_TO_TICKS(2000));

            bufferFrameIndex = 0; // сбрасываем счётчик фреймов из буффера

            if (BUFFERED_FRAMES == -1) {
                // нет новых буфферизованных кадров, тогда просто начинаем заново читать тот же буфер что и был до этого
            }
            else {
                // есть новые буфферизованные кадры, значита меняем буффер воспроизведения на него
                if (CURRENT_PLAY_BUFFER == 1) {
                    buffer = PROGRAM2_BUFFERS;
                    CURRENT_PLAY_BUFFER = 2;
                }
                else {
                    buffer = PROGRAM1_BUFFERS;
                    CURRENT_PLAY_BUFFER = 1;
                }

                bufferFramesCount = BUFFERED_FRAMES;

                int prepareFramesFrom = frameIndex + bufferFramesCount;
                if(prepareFramesFrom >= taskParams->countFrames)
                    prepareFramesFrom = prepareFramesFrom - taskParams->countFrames;

                // уведомляем о подготовке новых кадров
                PostManagedEvent(EVENT_CUSTOM, 0, 1, prepareFramesFrom);
            }
        }

        if (buffer == NULL)
            break;

        // передаём кадр на ленту
        int offset = bufferFrameIndex * FRAME_SIZE;
        //memcpy(FRAME_BUFFER, buffer + offset, FRAME_SIZE);

        TickType_t xTimeIncrement = transitionTimeIncrement;
        if (!transitionFrame.getNextFrame(lastRawFrame)) {
            memcpy(lastRawFrame, buffer + offset, FRAME_SIZE);
            xTimeIncrement = programTimeIncrement;
        }

        for (int i = 0; i < BUFF_SIZE; i++)
            FRAME_BUFFER[i] = lastRawFrame[i] * brightness / 0xFF;

        spi_send_data(FRAME_BUFFER, FRAME_SIZE);

        bool exit = requestedForStop;
        requestedForStop = false;

        CS_END(bodySemaphore);

        if (exit) break;

		vTaskDelayUntil(&lastWakeTime, xTimeIncrement);

        bufferFrameIndex++;

        if (frameIndex == taskParams->countFrames - 1)
            frameIndex = 0;
        else
            frameIndex++;
    }

    CS_BEGIN(bodySemaphore);
    running = false;
    CS_END(bodySemaphore);

    assert(xSemaphoreGive(joinSemaphore) == pdTRUE);

    vTaskDelete(NULL);
}

void spi_send_data(const uint8_t *data, int len)
{
	uint32_t offset = 0;
	do {
        int tx_len = len;//((len - offset) < SPI_MAX_DMA_LEN) ? (len - offset) : SPI_MAX_DMA_LEN;

		spi_transaction_t t;
		memset(&t, 0, sizeof(t));
		t.length = tx_len * 8;
		t.tx_buffer = data + offset;

		spi_device_polling_transmit(spi, &t);
		offset += tx_len;
		break;
	} while (offset < len);

    vTaskDelay(1);
}

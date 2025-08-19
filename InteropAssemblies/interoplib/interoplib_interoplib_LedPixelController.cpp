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

using namespace interoplib::interoplib;

static TaskHandle_t Task1;
static spi_device_handle_t spi = NULL;
static spi_host_device_t SPI_HOST = SPI2_HOST;
static DMA_ATTR uint8_t DATA_BUFFER[BUFF_SIZE];
static int LEDS_COUNT = 0;
static int INIT_BUFFER_SIZE = 0;

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

void Task1code( void * pvParameters ) {
    while(1) {
        spi_send_data(DATA_BUFFER, INIT_BUFFER_SIZE);
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

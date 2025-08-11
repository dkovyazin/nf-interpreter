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

using namespace interoplib::interoplib;

#define MOSI_PIN    41
#define MISO_PIN    39
#define CLK_PIN     40
#define CS_PIN      21 // 37 for ver 2

#define SPI_FREQ_HZ		    2500000
#define STRIPS_CNT 		    4
#define MAX_BUFFER_SIZE     STRIPS_CNT * 1000 * 3

spi_device_handle_t spi = NULL;
spi_host_device_t SPI_HOST = SPI2_HOST;
DMA_ATTR uint8_t DATA_BUFFER[MAX_BUFFER_SIZE];
int INIT_BUFFER_SIZE = 0;

void LedPixelController::NativeInit( signed int mosiPin, signed int misoPin, signed int clkPin, signed int csPin, signed int pixelCount, uint8_t red, uint8_t green, uint8_t blue, HRESULT &hr  )
{
    esp_err_t ret;

    if (spi != NULL) {
        ret = spi_bus_remove_device(spi);
        ESP_ERROR_CHECK(ret);

        ret = spi_bus_free(SPI_HOST);
        ESP_ERROR_CHECK(ret);

        spi = NULL;
        INIT_BUFFER_SIZE = 0;
    }

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
        SPI_CLK_SRC_XTAL,    // Clock source
        0,                   // Duty cycle 50/50
        0,                   // cs_ena_pretrans
        0,                   // cs_ena_posttrans
        SPI_FREQ_HZ,         // Clock speed in Hz
        0,                   // Input_delay_ns
        csPin,              // Chip select, we will use manual chip select
        0,                   // SPI_DEVICE flags
        7,                   // Queue size
        0,                   // Callback before
        0,                   // Callback after transaction complete
    };

    ret = spi_bus_add_device(SPI_HOST, &dev_cfg, &spi);
    ESP_ERROR_CHECK(ret);

    int color = 0;
    for (uint16_t led_num = 0; led_num < INIT_BUFFER_SIZE; led_num++) {
        if (color == 0)
            DATA_BUFFER[led_num] = red;
        else if (color == 1)
            DATA_BUFFER[led_num] = green;
        else if (color == 2)
            DATA_BUFFER[led_num] = blue;

        if (color == 2)
            color = 0;
        else
            color++;
    }

    spi_send_data(DATA_BUFFER, INIT_BUFFER_SIZE);

    hr = S_OK;
}

void LedPixelController::NativeWrite( CLR_RT_TypedArray_UINT8 data, HRESULT &hr )
{
    if (spi == NULL) {
        hr = CLR_E_BUSY;
        return;
    }

    memcpy(DATA_BUFFER, (void*)data.GetBuffer(), INIT_BUFFER_SIZE);

    spi_send_data2();

    hr = S_OK;
}

void spi_send_data(const uint8_t *data, int len)
{
	if (len == 0)
		return; // no need to send anything

	esp_err_t ret;
	int offset = 0;
	do
	{
		spi_transaction_t t;
		memset(&t, 0, sizeof(t)); // Zero out the transaction

		int tx_len = ((len - offset) < SPI_MAX_DMA_LEN) ? (len - offset) : SPI_MAX_DMA_LEN;
		t.length = tx_len * 8;						// Len is in bytes, transaction length is in bits.
		t.tx_buffer = data + offset;				// pointer to the data
		//t.flags = SPI_TRANS_CS_KEEP_ACTIVE;
		ret = spi_device_polling_transmit(spi, &t); // Transmit!
		assert(ret == ESP_OK);						// Should have had no issues.
		offset += tx_len;							// Add the transmission length to the offset
	} while (offset < len);
}

void spi_send_data2()
{
	//if (len == 0)
	//	return;

	esp_err_t ret;

    spi_transaction_t t;
	memset(&t, 0, sizeof(t));

	t.length = INIT_BUFFER_SIZE * 8;
	t.tx_buffer = DATA_BUFFER;
	ret = spi_device_polling_transmit(spi, &t);
	assert(ret == ESP_OK);
}

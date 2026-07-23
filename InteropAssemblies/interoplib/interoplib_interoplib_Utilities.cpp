//-----------------------------------------------------------------------------
//
// LEDTREES: мелкие нативные утилиты (реализация пишется руками — генерируются
// только interoplib.cpp/.h и *_mshl.cpp, их копируют из Stubs).
//
//-----------------------------------------------------------------------------

#include "interoplib.h"
#include "interoplib_interoplib_Utilities.h"
#include <esp_mac.h>
#include <esp_rom_crc.h>
#include <esp_log.h>
#include <driver/sdmmc_host.h>
#include <sdmmc_cmd.h>

using namespace interoplib::interoplib;


void Utilities::NativeGetBaseMac( CLR_RT_TypedArray_UINT8 param0, HRESULT &hr )
{
    uint8_t baseMac[6];

    esp_base_mac_addr_get(baseMac);

    memcpy((void*)param0.GetBuffer(), baseMac, 6);
}

// Инкрементальный zlib-совместимый CRC32 через ROM-функцию: табличный цикл по байту
// на nanoCLR считает CRC мегабайтного .ltf десятки секунд, ROM-функция — миллисекунды.
//
// Семантика esp_rom_crc32_le совпадает с zlib.crc32: состояние между вызовами —
// финализированное значение (инверсия на входе/выходе внутри ROM-реализации).
unsigned int Utilities::NativeCrc32( unsigned int param0, CLR_RT_TypedArray_UINT8 param1, signed int param2, signed int param3, HRESULT &hr )
{
    const uint8_t *data = (const uint8_t *)param1.GetBuffer();
    signed int offset = param2;
    signed int count = param3;

    // Границы managed-массива проверяем сами: bounds-check CLR в нативном коде не
    // работает. offset/count складываем уже беззнаковыми — у signed int сумма двух
    // больших положительных переполняется (UB), и проверку компилятор вправе выкинуть.
    if (data == NULL || offset < 0 || count < 0 ||
        (uint32_t)offset + (uint32_t)count > param1.GetSize())
    {
        hr = CLR_E_INVALID_PARAMETER;
        return param0;
    }

    return esp_rom_crc32_le(param0, data + offset, (uint32_t)count);
}

// Диагностическая проба SD-шины: полная инициализация карты драйвером ESP-IDF с
// заданной шириной (1|4) и частотой, затем деинициализация хоста. Managed
// комбинирует пробы для локализации отказа (см. Storage.Init): молчит совсем —
// CMD/CLK/питание; живёт только 1-бит — линии D1-D3; живёт только на низкой
// частоте — плохой контакт. Вызывается ТОЛЬКО при свободном SDMMC-хосте:
// конкурентная инициализация с примонтированной nanoFramework-картой недопустима.
//
// Результат: 0 ok, 1 таймаут (нет ответа), 2 CRC/данные, 3 прочее — сырой
// esp_err уходит в нативный лог.
signed int Utilities::NativeSdProbe( uint8_t width, uint16_t freqKhz, CLR_RT_TypedArray_UINT8 pins, HRESULT &hr )
{
    hr = S_OK;

    if (pins.GetSize() < 6 || (width != 1 && width != 4) || freqKhz == 0) {
        hr = CLR_E_INVALID_PARAMETER;
        return 3;
    }

    const uint8_t* p = (const uint8_t*)pins.GetBuffer();

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = freqKhz;
    if (width == 1)
        host.flags = (host.flags & ~(SDMMC_HOST_FLAG_4BIT | SDMMC_HOST_FLAG_8BIT)) | SDMMC_HOST_FLAG_1BIT;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = width;
#if SOC_SDMMC_USE_GPIO_MATRIX
    // S3 маршрутизирует SDMMC через GPIO-матрицу — линии как в Storage.Init
    slot.clk = (gpio_num_t)p[0];
    slot.cmd = (gpio_num_t)p[1];
    slot.d0  = (gpio_num_t)p[2];
    slot.d1  = (gpio_num_t)p[3];
    slot.d2  = (gpio_num_t)p[4];
    slot.d3  = (gpio_num_t)p[5];
#endif

    esp_err_t err = sdmmc_host_init();
    if (err != ESP_OK) {
        // хост занят (карта примонтирована?) — проба вне протокола, не судим о железе
        ESP_LOGE("interoplib", "sd probe: host init 0x%x", err);
        return 3;
    }

    int result;
    err = sdmmc_host_init_slot(SDMMC_HOST_SLOT_1, &slot);
    if (err != ESP_OK) {
        ESP_LOGE("interoplib", "sd probe: slot init 0x%x", err);
        result = 3;
    }
    else {
        // структура карты великовата для стека CLR-потока; interop-вызовы не
        // перекрываются (nanoCLR исполняет managed на одной нативной задаче) —
        // static безопасен
        static sdmmc_card_t card;
        err = sdmmc_card_init(&host, &card);

        if (err == ESP_OK)
            result = 0;
        else if (err == ESP_ERR_TIMEOUT)
            result = 1;
        else if (err == ESP_ERR_INVALID_CRC)
            result = 2;
        else
            result = 3;

        ESP_LOGI("interoplib", "sd probe: width=%u freq=%u kHz -> err=0x%x", width, freqKhz, err);
    }

    sdmmc_host_deinit();
    return result;
}

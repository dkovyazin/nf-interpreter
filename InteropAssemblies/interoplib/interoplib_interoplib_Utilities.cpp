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
#include <esp_wifi.h>
#include <esp_system.h>
#include <esp_heap_caps.h>
#include <esp_core_dump.h>
#include <esp_partition.h>
#include <driver/sdmmc_host.h>
#include <sdmmc_cmd.h>

// NF_ESP32_IsToConnect — флаг авто-реконнекта сетевого модуля
// (NF_ESP32_Wireless.cpp): пока он выставлен, обработчик
// WIFI_EVENT_STA_DISCONNECTED сам вызывает esp_wifi_connect — см.
// NativeWifiReconnect. Декларация из заголовка, а не локальный extern:
// локальная копия могла бы разъехаться с настоящим типом.
#include <NF_ESP32_Network.h>

using namespace interoplib::interoplib;


// Принудительный реконнект Wi-Fi STA — лекарство от «мёртвого линка», которого
// не видит драйвер: узел считает себя подключённым к ПРЕЖНЕМУ инстансу AP
// (перезагрузка MAIN прошла быстрее beacon-timeout либо тот не отработал) и
// вечно шлёт кадры со старыми ключами в никуда. Managed-сторожок
// (ScreenCommandService) зовёт этот метод по тишине от MAIN.
//
// esp_wifi_disconnect + безусловный esp_wifi_connect. Одного disconnect'а мало:
// событие STA_DISCONNECTED (его разбирает обработчик targetHAL_Network.cpp — при
// выставленном NF_ESP32_IsToConnect он сам вызывает esp_wifi_connect) драйвер
// порождает только из connected/connecting-состояния. В idle (авто-реконнект
// оборвался: результат esp_wifi_connect в обработчике не проверяется, одна
// коллизия со сканом рвёт цепочку навсегда) disconnect события не даёт — без
// прямого connect узел оставался бы офлайн до передёргивания питания. Прямой
// вызов безвреден и в остальных состояниях: connected/connecting вернёт ошибку,
// а реконнект после disconnect сделает обработчик события.
// Флаг поднимаем явно (не полагаясь на текущее состояние): вызов имеет смысл
// только на STA-девайсе, где подключение штатно и ожидается. Ошибки в hr не
// поднимаем: повторная попытка в любом состоянии драйвера безвредна, а сторожок
// всё равно повторит через свой интервал.
void Utilities::NativeWifiReconnect( HRESULT &hr )
{
    hr = S_OK;

    // Гейт по режиму: реконнект осмыслен только там, где есть STA-интерфейс.
    // На AP-девайсе (MAIN, несконфигурированный конфиг-режим) вызов — no-op:
    // esp_wifi_disconnect там и так вернул бы ошибку, но главное — не трогаем
    // NF_ESP32_IsToConnect, у AP-конфигурации флаг обязан оставаться false.
    // Не инициализирован Wi-Fi — esp_wifi_get_mode вернёт ошибку, тоже no-op.
    wifi_mode_t mode = WIFI_MODE_NULL;
    if (esp_wifi_get_mode(&mode) != ESP_OK || (mode != WIFI_MODE_STA && mode != WIFI_MODE_APSTA))
    {
        return;
    }

    // Флаг поднимаем явно, не полагаясь на текущее значение: на STA-девайсе
    // он уже true (выставлен штатным connect'ом с AutoConnect) — присваивание
    // идемпотентно, но защищает от вызова в окне, когда connect ещё не прошёл.
    NF_ESP32_IsToConnect = true;
    esp_wifi_disconnect();
    esp_wifi_connect();
}

void Utilities::NativeGetBaseMac( CLR_RT_TypedArray_UINT8 param0, HRESULT &hr )
{
    // managed обязан прислать буфер >= 6 байт: короче/NULL — memcpy пишет за концом
    if (param0.GetBuffer() == NULL || param0.GetSize() < 6) {
        hr = CLR_E_INVALID_PARAMETER;
        return;
    }

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

// Причина последнего ресета — сырое значение esp_reset_reason_t (0 unknown,
// 1 poweron, 2 external, 3 software, 4 panic, 5 int wdt, 6 task wdt, 7 wdt,
// 8 deep sleep, 9 brownout, 10 sdio). Managed-сторона повторяет ровно эти
// номера (TelemetryEventCodes.ResetReason): таблица перевода здесь только
// добавила бы место, где два перечисления разъезжаются при обновлении IDF.
//
// Значение живёт в RTC-памяти и переживает перезагрузку, но НЕ отключение
// питания — после него честно приходит poweron.
uint8_t Utilities::NativeGetResetReason( HRESULT &hr )
{
    hr = S_OK;
    return (uint8_t)esp_reset_reason();
}

// Watermark: минимум свободной памяти за всё время работы. Текущее «свободно»
// отвечает на вопрос «хватает ли сейчас», а этот — «подходили ли мы к краю»:
// пик потребления между периодическими замерами телеметрии не виден никак иначе.
//
// MALLOC_CAP_8BIT — тот же набор, что у nanoFramework.Hardware.Esp32.NativeMemory
// (GetCaps в nanoFramework_hardware_esp32_native_...NativeMemory.cpp), чтобы
// watermark и текущее свободно считались по одной и той же куче.
unsigned int Utilities::NativeGetMinFreeHeap( bool spiRam, HRESULT &hr )
{
    hr = S_OK;

    uint32_t caps = MALLOC_CAP_32BIT | MALLOC_CAP_8BIT | (spiRam ? MALLOC_CAP_SPIRAM : MALLOC_CAP_INTERNAL);
    return (unsigned int)heap_caps_get_minimum_free_size(caps);
}

// ---------------------------------------------------------------------------
// Core dump нативной паники (раздел coredump, docs/telemetry.md).
//
// Managed-кольцо лога (LogRing) панику не переживает: при ней CLR уже не
// исполняется, и стек упавшей задачи виден ТОЛЬКО отсюда. Дамп пишет ESP-IDF
// сам в обработчике паники; managed-сторона его только отдаёт наружу.
//
// Адрес и размер спрашиваем у esp_core_dump_image_get на КАЖДЫЙ вызов, а не
// кэшируем: между чтениями дамп могут стереть (кнопка в админке), а держать
// протухший адрес и читать по нему мусор — худший исход из возможных.
// Проверку целостности (SHA256) IDF делает внутри get, поэтому ненулевой
// размер означает пригодный к разбору дамп, а не «что-то лежит в разделе».
// ---------------------------------------------------------------------------

static const esp_partition_t *CoredumpPartition()
{
    return esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, NULL);
}

unsigned int Utilities::NativeGetCoredumpSize( HRESULT &hr )
{
    hr = S_OK;

    size_t addr = 0;
    size_t size = 0;
    if (esp_core_dump_image_get(&addr, &size) != ESP_OK)
    {
        // раздела нет (прошивка со старой таблицей), дамп не писался или битый
        return 0;
    }

    return (unsigned int)size;
}

signed int Utilities::NativeReadCoredump( unsigned int offset, CLR_RT_TypedArray_UINT8 buffer, signed int count, HRESULT &hr )
{
    hr = S_OK;

    // Границы managed-массива проверяем сами: bounds-check CLR в нативном коде
    // не работает (та же логика, что в NativeCrc32).
    if (buffer.GetBuffer() == NULL || count < 0 || (uint32_t)count > buffer.GetSize())
    {
        hr = CLR_E_INVALID_PARAMETER;
        return 0;
    }

    if (count == 0)
    {
        return 0;
    }

    size_t addr = 0;
    size_t size = 0;
    if (esp_core_dump_image_get(&addr, &size) != ESP_OK || offset >= size)
    {
        return 0;
    }

    const esp_partition_t *partition = CoredumpPartition();
    if (partition == NULL)
    {
        return 0;
    }

    // Хвост короче запрошенного — отдаём сколько есть; читатель идёт по
    // возвращённой длине, а не по запрошенной.
    size_t available = size - offset;
    size_t toRead = (size_t)count < available ? (size_t)count : available;

    // esp_core_dump_image_get отдаёт АБСОЛЮТНЫЙ адрес во флеши, а
    // esp_partition_read ждёт смещение внутри раздела — переводим одно в другое.
    // Без этого чтение ушло бы за пределы раздела и вернуло бы ошибку (в лучшем
    // случае) или чужие данные.
    size_t partitionOffset = addr - partition->address + offset;

    if (esp_partition_read(partition, partitionOffset, (void *)buffer.GetBuffer(), toRead) != ESP_OK)
    {
        return 0;
    }

    return (signed int)toRead;
}

bool Utilities::NativeEraseCoredump( HRESULT &hr )
{
    hr = S_OK;
    return esp_core_dump_image_erase() == ESP_OK;
}

//-----------------------------------------------------------------------------
//
// LEDTREES: адаптер стабов Utilities к компоненту ledtrees_sysinfo.
//
// Идентификация, CRC, проба SD-шины, причина ресета, watermark кучи и выдача
// core dump живут в отдельном IDF-компоненте — репозиторий
// ledtrees-idf-components, components/ledtrees_sysinfo. Здесь остаётся
// разворачивание managed-массивов с проверкой их размеров.
//
// ИСКЛЮЧЕНИЕ — NativeWifiReconnect: он завязан на сетевой модуль самого
// nf-interpreter (NF_ESP32_IsToConnect из NF_ESP32_Network.h) и в компонент не
// выносится, реализация целиком здесь.
//
//-----------------------------------------------------------------------------

#include "interoplib.h"
#include "interoplib_interoplib_Utilities.h"

#include <esp_wifi.h>
#include <ledtrees_sysinfo.h>

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
    // managed обязан прислать буфер >= 6 байт: короче/NULL — запись за концом
    if (param0.GetBuffer() == NULL || param0.GetSize() < 6) {
        hr = CLR_E_INVALID_PARAMETER;
        return;
    }

    lt_sys_base_mac(param0.GetBuffer());
}

// Инкрементальный zlib-совместимый CRC32: табличный цикл по байту на nanoCLR
// считает CRC мегабайтного .ltf десятки секунд, ROM-функция — миллисекунды.
unsigned int Utilities::NativeCrc32( unsigned int param0, CLR_RT_TypedArray_UINT8 param1, signed int param2, signed int param3, HRESULT &hr )
{
    const uint8_t *data = (const uint8_t *)param1.GetBuffer();
    signed int offset = param2;
    signed int count = param3;

    // Границы managed-массива проверяем здесь: bounds-check CLR в нативном коде не
    // работает. offset/count складываем уже беззнаковыми — у signed int сумма двух
    // больших положительных переполняется (UB), и проверку компилятор вправе выкинуть.
    if (data == NULL || offset < 0 || count < 0 ||
        (uint32_t)offset + (uint32_t)count > param1.GetSize())
    {
        hr = CLR_E_INVALID_PARAMETER;
        return param0;
    }

    return lt_sys_crc32(param0, data + offset, (size_t)count);
}

// Диагностическая проба SD-шины (см. Storage.Init на managed-стороне):
// 0 ok, 1 таймаут (нет ответа), 2 CRC/данные, 3 прочее.
signed int Utilities::NativeSdProbe( uint8_t width, uint16_t freqKhz, CLR_RT_TypedArray_UINT8 pins, HRESULT &hr )
{
    hr = S_OK;

    if (pins.GetSize() < 6) {
        hr = CLR_E_INVALID_PARAMETER;
        return LT_SD_OTHER;
    }

    signed int result = lt_sys_sd_probe(width, freqKhz, (const uint8_t *)pins.GetBuffer());

    if (result == LT_SD_PARAM) {
        hr = CLR_E_INVALID_PARAMETER;
        return LT_SD_OTHER;
    }

    return result;
}

// Причина последнего ресета — сырое значение esp_reset_reason_t (0 unknown,
// 1 poweron, 2 external, 3 software, 4 panic, 5 int wdt, 6 task wdt, 7 wdt,
// 8 deep sleep, 9 brownout, 10 sdio). Managed-сторона повторяет ровно эти
// номера (TelemetryEventCodes.ResetReason): таблица перевода здесь только
// добавила бы место, где два перечисления разъезжаются при обновлении IDF.
uint8_t Utilities::NativeGetResetReason( HRESULT &hr )
{
    hr = S_OK;
    return lt_sys_reset_reason();
}

// Watermark: минимум свободной памяти за всё время работы. Текущее «свободно»
// отвечает на вопрос «хватает ли сейчас», а этот — «подходили ли мы к краю».
unsigned int Utilities::NativeGetMinFreeHeap( bool spiRam, HRESULT &hr )
{
    hr = S_OK;
    return lt_sys_min_free_heap(spiRam);
}

// ---------------------------------------------------------------------------
// Core dump нативной паники (раздел coredump, docs/telemetry.md).
//
// Managed-кольцо лога (LogRing) панику не переживает: при ней CLR уже не
// исполняется, и стек упавшей задачи виден ТОЛЬКО отсюда.
// ---------------------------------------------------------------------------

unsigned int Utilities::NativeGetCoredumpSize( HRESULT &hr )
{
    hr = S_OK;
    return lt_sys_coredump_size();
}

signed int Utilities::NativeReadCoredump( unsigned int offset, CLR_RT_TypedArray_UINT8 buffer, signed int count, HRESULT &hr )
{
    hr = S_OK;

    // Границы managed-массива проверяем здесь: bounds-check CLR в нативном коде
    // не работает (та же логика, что в NativeCrc32).
    if (buffer.GetBuffer() == NULL || count < 0 || (uint32_t)count > buffer.GetSize())
    {
        hr = CLR_E_INVALID_PARAMETER;
        return 0;
    }

    return lt_sys_coredump_read(offset, buffer.GetBuffer(), (size_t)count);
}

bool Utilities::NativeEraseCoredump( HRESULT &hr )
{
    hr = S_OK;
    return lt_sys_coredump_erase();
}

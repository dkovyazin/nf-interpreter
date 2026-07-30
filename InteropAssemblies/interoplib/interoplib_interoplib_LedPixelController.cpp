//-----------------------------------------------------------------------------
//
// LEDTREES: адаптер стабов LedPixelController к компоненту ledtrees_ledpixel.
//
// Весь вывод на ленту (SPI, задачи LedTask/FeedTask, буферы воспроизведения,
// подкормка с SD, ремап, цветокоррекция) живёт в отдельном IDF-компоненте —
// репозиторий ledtrees-idf-components, components/ledtrees_ledpixel. Здесь
// остаётся разворачивание managed-массивов в (указатель, размер) и перевод
// lt_led_status_t в HRESULT.
//
// Реализация пишется руками (в отличие от interoplib.cpp/.h и *_mshl.cpp — те
// генерируются сборкой managed-проекта и лежат готовыми в
// ledtrees-esp32/nanoframework/src/interoplib/Stubs/interoplib; их надо копировать
// оттуда, а не править здесь: таблица method_lookup индексируется порядковым номером
// метода в ассембли, и ручная вставка сдвигает индексы — вызовы уходят в чужие слоты).
//
//-----------------------------------------------------------------------------

#include "interoplib.h"
#include "interoplib_interoplib_LedPixelController.h"

#include <ledtrees_ledpixel.h>

using namespace interoplib::interoplib;

// LT_LED_ERR_STATE — прежний S_FALSE (состояние не позволяет), LT_LED_ERR_PARAM —
// прежний CLR_E_INVALID_PARAMETER (аргументы не сходятся). Разделение то же, что
// было в теле каждого метода до выноса.
static HRESULT ToHResult(lt_led_status_t status)
{
    switch (status)
    {
        case LT_LED_OK:
            return S_OK;
        case LT_LED_ERR_PARAM:
            return CLR_E_INVALID_PARAMETER;
        default:
            return S_FALSE;
    }
}

void LedPixelController::NativeInit(
    signed int mosiPin,
    signed int misoPin,
    signed int clkPin,
    signed int csPin,
    signed int pixelCount,
    uint8_t red,
    uint8_t green,
    uint8_t blue,
    uint8_t powerLimitPercent,
    HRESULT &hr)
{
    lt_led_config_t config;
    config.mosiPin = mosiPin;
    config.misoPin = misoPin;
    config.clkPin = clkPin;
    config.csPin = csPin;
    config.pixelCount = pixelCount;
    config.initRed = red;
    config.initGreen = green;
    config.initBlue = blue;
    // Ограничитель мощности — заводская доля из конфигурации сборки
    // (ledtrees-esp32 docs/assembly.md); 0 либо >= 100 компонент понимает как
    // «лимитера нет».
    config.powerLimitPercent = powerLimitPercent;

    hr = ToHResult(lt_led_init(&config));
}

void LedPixelController::NativeSetSource(
    CLR_RT_TypedArray_UINT8 path,
    unsigned int header,
    uint16_t frameSize,
    uint16_t countFrames,
    uint16_t startFrame,
    HRESULT &hr)
{
    hr = ToHResult(
        lt_led_set_source(path.GetBuffer(), path.GetSize(), header, frameSize, countFrames, startFrame));
}

void LedPixelController::NativeSetBrightness( uint8_t value, HRESULT &hr )
{
    hr = ToHResult(lt_led_set_brightness(value));
}

signed int LedPixelController::NativeGetPlayPosition( HRESULT &hr )
{
    hr = S_OK;
    return lt_led_get_play_position();
}

signed int LedPixelController::NativeGetFeedReadErrors( HRESULT &hr )
{
    hr = S_OK;
    return lt_led_get_feed_read_errors();
}

// Таблица пер-нодового ремапа (docs/node-calibration.md). Пустой массив — снять
// ремап; битую длину/индекс компонент отвергает, действующую таблицу не трогая.
void LedPixelController::NativeSetRemap( CLR_RT_TypedArray_UINT8 param0, HRESULT &hr )
{
    hr = ToHResult(lt_led_set_remap(param0.GetBuffer(), param0.GetSize()));
}

// Подсветка прохода калибровки: диапазон физических пикселей линии мигает
// красным ↔ чёрным. count == 0 — снять подсветку.
void LedPixelController::NativeSetHighlight( uint8_t line, uint16_t start, uint16_t count, uint16_t blinkMs, HRESULT &hr )
{
    hr = ToHResult(lt_led_set_highlight(line, start, count, blinkMs));
}

// Смена доли ограничителя мощности на ходу. Штатно значение приезжает один раз
// в NativeInit из конфигурации сборки; этот путь — для сервисного подбора и
// будущего управления с MAIN.
void LedPixelController::NativeSetPowerLimit( uint8_t percent, HRESULT &hr )
{
    hr = ToHResult(lt_led_set_power_limit(percent));
}

// Кадров, вышедших с урезанной ограничителем яркостью, за время работы.
signed int LedPixelController::NativeGetPowerLimitedFrames( HRESULT &hr )
{
    hr = S_OK;
    return lt_led_get_power_limited_frames();
}

// Ре-якорь SyncPlay: сверить свою позицию с кадром, который группа играет прямо
// сейчас. Возвращает назначенную коррекцию в кадрах; 0 — в фазе либо очень
// близкая коррекция уже идёт.
signed int LedPixelController::NativeSyncPlayPosition( uint16_t groupFrame, HRESULT &hr )
{
    hr = S_OK;
    return lt_led_sync_play_position(groupFrame);
}

void LedPixelController::NativePrepareForPlay( uint16_t frame, CLR_RT_TypedArray_UINT8 data, HRESULT &hr )
{
    hr = ToHResult(lt_led_prepare_for_play(frame, data.GetBuffer(), data.GetSize()));
}

void LedPixelController::NativeStartPlay( uint16_t countFrames, uint8_t outputFps, uint8_t contentFps, uint16_t transition, HRESULT &hr )
{
    hr = ToHResult(lt_led_start_play(countFrames, outputFps, contentFps, transition));
}

// ВНИМАНИЕ по контракту: пишет НЕиграющий буфер без Stop/Join и без блокировок —
// те же данные во время воспроизведения ведёт задача подкормки. Безопасно только
// при вызове на ОСТАНОВЛЕННОМ плеере. Сейчас managed эту точку не зовёт вовсе
// (обёртка LedPixelController.WriteToPlayBuffer без вызывающих).
void LedPixelController::NativeWriteToPlayBuffer( uint16_t frame, CLR_RT_TypedArray_UINT8 data, HRESULT &hr )
{
    hr = ToHResult(lt_led_write_to_play_buffer(frame, data.GetBuffer(), data.GetSize()));
}

void LedPixelController::NativeWrite( CLR_RT_TypedArray_UINT8 data, HRESULT &hr )
{
    hr = ToHResult(lt_led_write(data.GetBuffer(), data.GetSize()));
}

// Заливка цветом с плавным переходом от текущего состояния ленты (transition мс;
// 0 — мгновенно). Блокирует вызвавший поток CLR на время перехода.
void LedPixelController::NativeSetFull( uint8_t red, uint8_t green, uint8_t blue, uint16_t transition, HRESULT &hr )
{
    hr = ToHResult(lt_led_set_full(red, green, blue, transition));
}

void LedPixelController::NativeSetPixel( uint8_t line, uint16_t cell, uint8_t red, uint8_t green, uint8_t blue, HRESULT &hr )
{
    hr = ToHResult(lt_led_set_pixel(line, cell, red, green, blue));
}

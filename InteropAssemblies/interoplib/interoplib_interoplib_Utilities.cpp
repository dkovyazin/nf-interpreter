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

using namespace interoplib::interoplib;


void Utilities::NativeGetBaseMac( CLR_RT_TypedArray_UINT8 param0, HRESULT &hr )
{
    uint8_t baseMac[6];

    esp_base_mac_addr_get(baseMac);

    memcpy((void*)param0.GetBuffer(), baseMac, 6);
}

// Инкрементальный zlib-совместимый CRC32 через ROM-функцию: табличный цикл по байту
// на nanoCLR считает CRC мегабайтного .ltfw десятки секунд, ROM-функция — миллисекунды.
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

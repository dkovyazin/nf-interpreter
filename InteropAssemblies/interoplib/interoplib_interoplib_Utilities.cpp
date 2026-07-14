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

// LEDTREES: инкрементальный zlib-совместимый CRC32 через ROM-функцию.
// Семантика esp_rom_crc32_le совпадает с zlib.crc32: состояние между вызовами —
// финализированное значение (инверсия на входе/выходе внутри ROM-реализации).
// Managed-версия считала табличный цикл по байту интерпретатором — на мегабайтном
// .ltfw это десятки секунд на каждый OTA.
unsigned int Utilities::NativeCrc32( unsigned int param0, CLR_RT_TypedArray_UINT8 param1, signed int param2, signed int param3, HRESULT &hr )
{
    const uint8_t *data = (const uint8_t *)param1.GetBuffer();
    signed int offset = param2;
    signed int count = param3;

    // валидация границ managed-массива: не читаем мимо
    if (data == NULL || offset < 0 || count < 0 || (uint32_t)(offset + count) > param1.GetSize())
    {
        hr = CLR_E_INVALID_PARAMETER;
        return param0;
    }

    return esp_rom_crc32_le(param0, data + offset, (uint32_t)count);
}

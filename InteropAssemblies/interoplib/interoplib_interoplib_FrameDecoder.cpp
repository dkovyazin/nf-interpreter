//-----------------------------------------------------------------------------
//
// LEDTREES: адаптер стаба FrameDecoder к компоненту ledtrees_framecodec.
//
// Сама логика декода (delta-RLE, формат зеркалит managed FrameCodec.cs и
// TS-энкодер) живёт в отдельном IDF-компоненте — репозиторий
// ledtrees-idf-components, components/ledtrees_framecodec. Здесь остаётся
// только разворачивание managed-массивов в (указатель, размер) и перевод кода
// возврата в HRESULT.
//
//-----------------------------------------------------------------------------

#include "interoplib.h"
#include "interoplib_interoplib_FrameDecoder.h"

#include <ledtrees_framecodec.h>

using namespace interoplib::interoplib;

signed int FrameDecoder::NativeDecodeFrame(
    CLR_RT_TypedArray_UINT8 param0,
    signed int param1,
    signed int param2,
    CLR_RT_TypedArray_UINT8 param3,
    CLR_RT_TypedArray_UINT8 param4,
    uint16_t param5,
    HRESULT &hr)
{
    // Границы managed-массивов проверяет компонент — ему для этого и передаются
    // фактические размеры: bounds-check CLR в нативном коде не работает, промах
    // ушёл бы прямо в память за блоком кучи.
    int32_t result = lt_frame_decode(
        (const uint8_t *)param0.GetBuffer(),
        param0.GetSize(),
        param1,
        param2,
        (uint8_t *)param3.GetBuffer(),
        param3.GetSize(),
        (uint8_t *)param4.GetBuffer(),
        param4.GetSize(),
        param5);

    if (result == LT_FRAME_ERR_PARAM)
    {
        // Managed-контракт прежний: аргументы не сошлись — исключение по hr, а в
        // возврате тот же код «битый формат», что и раньше.
        hr = CLR_E_INVALID_PARAMETER;
        return LT_FRAME_ERR_FORMAT;
    }

    return result;
}

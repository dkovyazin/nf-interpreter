//-----------------------------------------------------------------------------
//
// LEDTREES: adapter from the FrameDecoder stub to the ledtrees_framecodec
// component.
//
// The decoding itself (delta-RLE, a format that mirrors the managed
// FrameCodec.cs and the TypeScript encoder) lives in a separate IDF component -
// the ledtrees-idf-components repository, components/ledtrees_framecodec. All
// that is left here is unwrapping the managed arrays into a pointer and a size,
// and translating the return code into an HRESULT.
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
    // The component checks the bounds of the managed arrays, which is why it is
    // handed the actual sizes: the CLR bounds check does not apply to native
    // code, so an overrun would go straight into the memory past the heap block.
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
        // The managed contract is unchanged: when the arguments do not add up it
        // raises an exception through hr, and returns the same "malformed format"
        // code as before.
        hr = CLR_E_INVALID_PARAMETER;
        return LT_FRAME_ERR_FORMAT;
    }

    return result;
}

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

#ifndef INTEROPLIB_INTEROPLIB_INTEROPLIB_LEDPIXELCONTROLLER_H
#define INTEROPLIB_INTEROPLIB_INTEROPLIB_LEDPIXELCONTROLLER_H

namespace interoplib
{
    namespace interoplib
    {
        struct LedPixelController
        {
            // Helper Functions to access fields of managed object
            // Declaration of stubs. These functions are implemented by Interop code developers

            static void NativeInit( signed int param0, signed int param1, signed int param2, signed int param3, signed int param4, uint8_t param5, uint8_t param6, uint8_t param7, uint8_t param8, HRESULT &hr );

            static void NativeSetBrightness( uint8_t param0, HRESULT &hr );

            static void NativePrepareForPlay( uint16_t param0, CLR_RT_TypedArray_UINT8 param1, HRESULT &hr );

            static void NativeStartPlay( uint16_t param0, uint8_t param1, uint8_t param2, uint16_t param3, HRESULT &hr );

            static void NativeWriteToPlayBuffer( uint16_t param0, CLR_RT_TypedArray_UINT8 param1, HRESULT &hr );

            static void NativeSetSource( CLR_RT_TypedArray_UINT8 param0, unsigned int param1, uint16_t param2, uint16_t param3, uint16_t param4, HRESULT &hr );

            static void NativeWrite( CLR_RT_TypedArray_UINT8 param0, HRESULT &hr );

            static void NativeSetFull( uint8_t param0, uint8_t param1, uint8_t param2, uint16_t param3, HRESULT &hr );

            static void NativeSetPixel( uint8_t param0, uint16_t param1, uint8_t param2, uint8_t param3, uint8_t param4, HRESULT &hr );

            static signed int NativeGetPlayPosition(  HRESULT &hr );

            static signed int NativeSyncPlayPosition( uint16_t param0, HRESULT &hr );

            static signed int NativeGetFeedReadErrors(  HRESULT &hr );

            static void NativeSetRemap( CLR_RT_TypedArray_UINT8 param0, HRESULT &hr );

            static void NativeSetHighlight( uint8_t param0, uint16_t param1, uint16_t param2, uint16_t param3, HRESULT &hr );

            static void NativeSetPowerLimit( uint8_t param0, HRESULT &hr );

            static signed int NativeGetPowerLimitedFrames(  HRESULT &hr );

            static void NativeSetColorMatrix( CLR_RT_TypedArray_INT16 param0, HRESULT &hr );

            static void NativeSetColorLut( CLR_RT_TypedArray_UINT8 param0, HRESULT &hr );

            static void NativeSetColorLutEnabled( bool param0, HRESULT &hr );

            static signed int NativeGetComposeStats(  HRESULT &hr );

            static void NativeSetOutputCorrection( uint16_t param0, uint8_t param1, uint16_t param2, uint16_t param3, uint16_t param4, int16_t param5, HRESULT &hr );

        };
    }
}

#endif // INTEROPLIB_INTEROPLIB_INTEROPLIB_LEDPIXELCONTROLLER_H

// LEDTREES: there are no hand written declarations on top of the generated code
// here any more - the output and feeding tasks and the SPI moved inside the
// ledtrees_ledpixel component (the ledtrees-idf-components repository) and are
// not visible from outside. This file is now purely generated: when regenerating
// from Stubs it can be copied as is.

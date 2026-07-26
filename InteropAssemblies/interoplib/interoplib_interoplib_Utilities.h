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

#ifndef INTEROPLIB_INTEROPLIB_INTEROPLIB_UTILITIES_H
#define INTEROPLIB_INTEROPLIB_INTEROPLIB_UTILITIES_H

namespace interoplib
{
    namespace interoplib
    {
        struct Utilities
        {
            // Helper Functions to access fields of managed object
            // Declaration of stubs. These functions are implemented by Interop code developers

            static void NativeGetBaseMac( CLR_RT_TypedArray_UINT8 param0, HRESULT &hr );

            static unsigned int NativeCrc32( unsigned int param0, CLR_RT_TypedArray_UINT8 param1, signed int param2, signed int param3, HRESULT &hr );

            static signed int NativeSdProbe( uint8_t param0, uint16_t param1, CLR_RT_TypedArray_UINT8 param2, HRESULT &hr );

            static void NativeWifiReconnect(  HRESULT &hr );

            static uint8_t NativeGetResetReason(  HRESULT &hr );

            static unsigned int NativeGetMinFreeHeap( bool param0, HRESULT &hr );

            static unsigned int NativeGetCoredumpSize(  HRESULT &hr );

            static signed int NativeReadCoredump( unsigned int param0, CLR_RT_TypedArray_UINT8 param1, signed int param2, HRESULT &hr );

            static bool NativeEraseCoredump(  HRESULT &hr );

        };
    }
}

#endif // INTEROPLIB_INTEROPLIB_INTEROPLIB_UTILITIES_H

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

#ifndef INTEROPLIB_INTEROPLIB_INTEROPLIB_OTA_H
#define INTEROPLIB_INTEROPLIB_INTEROPLIB_OTA_H

namespace interoplib
{
    namespace interoplib
    {
        struct Ota
        {
            // Helper Functions to access fields of managed object
            // Declaration of stubs. These functions are implemented by Interop code developers

            static bool NativeFirmwareBegin( signed int param0, HRESULT &hr );

            static bool NativeFirmwareWrite( CLR_RT_TypedArray_UINT8 param0, signed int param1, HRESULT &hr );

            static bool NativeFirmwareEnd(  HRESULT &hr );

            static bool NativeGetRunningSha256( CLR_RT_TypedArray_UINT8 param0, HRESULT &hr );

            static bool NativeStageBegin( signed int param0, HRESULT &hr );

            static bool NativeStageWrite( CLR_RT_TypedArray_UINT8 param0, signed int param1, HRESULT &hr );

            static bool NativeStageCommit( unsigned int param0, HRESULT &hr );

            static bool NativeCommitFull(  HRESULT &hr );

            static bool NativeConfirm(  HRESULT &hr );

            static uint8_t NativeGetState(  HRESULT &hr );

            static bool NativeIsPendingConfirm(  HRESULT &hr );

        };
    }
}

#endif // INTEROPLIB_INTEROPLIB_INTEROPLIB_OTA_H

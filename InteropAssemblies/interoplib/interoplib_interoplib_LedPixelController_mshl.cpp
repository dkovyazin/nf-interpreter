//-----------------------------------------------------------------------------
//
//    ** DO NOT EDIT THIS FILE! **
//    This file was generated by a tool
//    re-running the tool will overwrite this file.
//
//-----------------------------------------------------------------------------

#include "interoplib.h"
#include "interoplib_interoplib_LedPixelController.h"

using namespace interoplib::interoplib;


HRESULT Library_interoplib_interoplib_LedPixelController::NativeInit___STATIC__VOID__I4__U1__U1__U1( CLR_RT_StackFrame& stack )
{
    NANOCLR_HEADER(); hr = S_OK;
    {

        signed int param0;
        NANOCLR_CHECK_HRESULT( Interop_Marshal_INT32( stack, 0, param0 ) );

        uint8_t param1;
        NANOCLR_CHECK_HRESULT( Interop_Marshal_UINT8( stack, 1, param1 ) );

        uint8_t param2;
        NANOCLR_CHECK_HRESULT( Interop_Marshal_UINT8( stack, 2, param2 ) );

        uint8_t param3;
        NANOCLR_CHECK_HRESULT( Interop_Marshal_UINT8( stack, 3, param3 ) );

        LedPixelController::NativeInit( param0, param1, param2, param3, hr );
        NANOCLR_CHECK_HRESULT( hr );

    }
    NANOCLR_NOCLEANUP();
}

HRESULT Library_interoplib_interoplib_LedPixelController::NativeWrite___STATIC__VOID__SZARRAY_U1( CLR_RT_StackFrame& stack )
{
    NANOCLR_HEADER(); hr = S_OK;
    {

        CLR_RT_TypedArray_UINT8 param0;
        NANOCLR_CHECK_HRESULT( Interop_Marshal_UINT8_ARRAY( stack, 0, param0 ) );

        LedPixelController::NativeWrite( param0, hr );
        NANOCLR_CHECK_HRESULT( hr );

    }
    NANOCLR_NOCLEANUP();
}

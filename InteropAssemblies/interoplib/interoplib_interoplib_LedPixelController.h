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

            static void NativeInit( signed int param0, uint8_t param1, uint8_t param2, uint8_t param3, HRESULT &hr );

            static void NativeWrite( CLR_RT_TypedArray_UINT8 param0, HRESULT &hr );
        };
    }
}

void spi_send_data(const uint8_t *data, int len);
void spi_send_data2(const uint8_t *data, int len);

#endif // INTEROPLIB_INTEROPLIB_INTEROPLIB_LEDPIXELCONTROLLER_H

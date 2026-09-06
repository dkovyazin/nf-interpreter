//-----------------------------------------------------------------------------
//
// LEDTREES: adapter from the Crypto stubs to the ledtrees_crypto component.
//
// The streaming SHA-256 and the ECDSA P-256 signature check on top of mbedTLS
// live in a separate IDF component - the ledtrees-idf-components repository,
// components/ledtrees_crypto. All that is left here is unwrapping the managed
// arrays and validating their sizes.
//
//-----------------------------------------------------------------------------

#include "interoplib.h"
#include "interoplib_interoplib_Crypto.h"

#include <ledtrees_crypto.h>

using namespace interoplib::interoplib;

bool Crypto::NativeSha256Begin(HRESULT &hr)
{
    (void)hr;

    return lt_sha256_begin();
}

bool Crypto::NativeSha256Update(CLR_RT_TypedArray_UINT8 param0, signed int param1, HRESULT &hr)
{
    if (param0.GetBuffer() == NULL || param1 < 0 || (uint32_t)param1 > param0.GetSize())
    {
        hr = CLR_E_INVALID_PARAMETER;
        return false;
    }

    return lt_sha256_update(param0.GetBuffer(), (size_t)param1);
}

bool Crypto::NativeSha256Final(CLR_RT_TypedArray_UINT8 param0, HRESULT &hr)
{
    if (param0.GetBuffer() == NULL || param0.GetSize() < 32)
    {
        hr = CLR_E_INVALID_PARAMETER;
        return false;
    }

    return lt_sha256_final(param0.GetBuffer());
}

// param0 = pubkey X||Y (64), param1 = digest SHA-256 (32), param2 = signature r||s (64).
// A wrong signature, or a failure to parse the point or the numbers, is a plain
// false rather than an exception; hr is only set for invalid buffer sizes.
bool Crypto::NativeVerifyP256(
    CLR_RT_TypedArray_UINT8 param0,
    CLR_RT_TypedArray_UINT8 param1,
    CLR_RT_TypedArray_UINT8 param2,
    HRESULT &hr)
{
    if (param0.GetBuffer() == NULL || param0.GetSize() < 64 || param1.GetBuffer() == NULL ||
        param1.GetSize() < 32 || param2.GetBuffer() == NULL || param2.GetSize() < 64)
    {
        hr = CLR_E_INVALID_PARAMETER;
        return false;
    }

    return lt_verify_p256(param0.GetBuffer(), param1.GetBuffer(), param2.GetBuffer());
}

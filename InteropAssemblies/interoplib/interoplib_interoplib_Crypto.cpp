//-----------------------------------------------------------------------------
//
// LEDTREES: крипто-примитивы для аутентификации OTA (реализация пишется руками —
// генерируются только interoplib.cpp/.h и *_mshl.cpp, их копируют из Stubs).
//
// Потоковый SHA-256 и проверка подписи ECDSA P-256 поверх mbedTLS из ESP-IDF.
// Используются для проверки подписи manifest.json и sha256 артефакта .ltfw
// (docs/ota.md §11). Требует, чтобы в конфиге mbedTLS таргета были включены
// (для esp32s3 это значения по умолчанию — проверить в sdkconfig при сборке):
//   CONFIG_MBEDTLS_SHA256_C, CONFIG_MBEDTLS_ECDSA_C,
//   CONFIG_MBEDTLS_ECP_C, CONFIG_MBEDTLS_ECP_DP_SECP256R1_ENABLED
//
//-----------------------------------------------------------------------------

#include "interoplib.h"
#include "interoplib_interoplib_Crypto.h"

#include <string.h>
#include <mbedtls/sha256.h>
#include <mbedtls/ecp.h>
#include <mbedtls/ecdsa.h>
#include <mbedtls/bignum.h>

using namespace interoplib::interoplib;

// Единственный потоковый контекст: OTA считает хэши строго последовательно
// (одноразовый хэш манифеста + потоковый хэш .ltfw при скачивании, не одновременно).
static mbedtls_sha256_context s_shaCtx;
static bool s_shaActive = false;

bool Crypto::NativeSha256Begin(HRESULT &hr)
{
    (void)hr;

    // повторный Begin без Final не течёт: освобождаем предыдущий контекст
    if (s_shaActive)
    {
        mbedtls_sha256_free(&s_shaCtx);
        s_shaActive = false;
    }

    mbedtls_sha256_init(&s_shaCtx);

    // второй аргумент 0 = SHA-256 (1 было бы SHA-224)
    if (mbedtls_sha256_starts(&s_shaCtx, 0) != 0)
    {
        mbedtls_sha256_free(&s_shaCtx);
        return false;
    }

    s_shaActive = true;
    return true;
}

bool Crypto::NativeSha256Update(CLR_RT_TypedArray_UINT8 param0, signed int param1, HRESULT &hr)
{
    if (param0.GetBuffer() == NULL || param1 < 0 || (uint32_t)param1 > param0.GetSize())
    {
        hr = CLR_E_INVALID_PARAMETER;
        return false;
    }

    if (!s_shaActive)
    {
        return false;
    }

    return mbedtls_sha256_update(&s_shaCtx, param0.GetBuffer(), (size_t)param1) == 0;
}

bool Crypto::NativeSha256Final(CLR_RT_TypedArray_UINT8 param0, HRESULT &hr)
{
    if (param0.GetBuffer() == NULL || param0.GetSize() < 32)
    {
        hr = CLR_E_INVALID_PARAMETER;
        return false;
    }

    if (!s_shaActive)
    {
        return false;
    }

    int rc = mbedtls_sha256_finish(&s_shaCtx, param0.GetBuffer());
    mbedtls_sha256_free(&s_shaCtx);
    s_shaActive = false;

    return rc == 0;
}

// param0 = pubkey X||Y (64), param1 = digest SHA-256 (32), param2 = signature r||s (64).
// Неверная подпись или ошибка разбора точки/чисел — обычный false (не исключение);
// hr выставляем только на некорректных размерах буферов.
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

    bool ok = false;

    mbedtls_ecp_group grp;
    mbedtls_ecp_point Q;
    mbedtls_mpi r;
    mbedtls_mpi s;
    mbedtls_ecp_group_init(&grp);
    mbedtls_ecp_point_init(&Q);
    mbedtls_mpi_init(&r);
    mbedtls_mpi_init(&s);

    do
    {
        if (mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1) != 0)
        {
            break;
        }

        // mbedtls читает точку в несжатом формате 0x04 || X || Y
        uint8_t point[65];
        point[0] = 0x04;
        memcpy(point + 1, param0.GetBuffer(), 64);
        if (mbedtls_ecp_point_read_binary(&grp, &Q, point, sizeof(point)) != 0)
        {
            break;
        }

        if (mbedtls_mpi_read_binary(&r, param2.GetBuffer(), 32) != 0)
        {
            break;
        }
        if (mbedtls_mpi_read_binary(&s, param2.GetBuffer() + 32, 32) != 0)
        {
            break;
        }

        ok = (mbedtls_ecdsa_verify(&grp, param1.GetBuffer(), 32, &Q, &r, &s) == 0);
    } while (0);

    mbedtls_mpi_free(&r);
    mbedtls_mpi_free(&s);
    mbedtls_ecp_point_free(&Q);
    mbedtls_ecp_group_free(&grp);

    return ok;
}

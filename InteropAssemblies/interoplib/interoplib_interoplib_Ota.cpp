//
// OTA interop implementation: thin wrappers over the NF_Ota_* core
// (targets/ESP32/_common/targetHAL_Ota.c, requires NF_FEATURE_OTA).
//

#include "interoplib.h"
#include "interoplib_interoplib_Ota.h"

#include <targetHAL_Ota.h>

#if !CONFIG_NF_FEATURE_OTA
#error "interoplib.Ota requires a target built with CONFIG_NF_FEATURE_OTA=y"
#endif

using namespace interoplib::interoplib;

bool Ota::NativeFirmwareBegin(signed int param0, HRESULT &hr)
{
    (void)hr;
    return NF_Ota_FirmwareBegin((uint32_t)param0);
}

bool Ota::NativeFirmwareWrite(CLR_RT_TypedArray_UINT8 param0, signed int param1, HRESULT &hr)
{
    if (param0.GetBuffer() == NULL || param1 < 0 || (uint32_t)param1 > param0.GetSize())
    {
        hr = CLR_E_INVALID_PARAMETER;
        return false;
    }

    return NF_Ota_FirmwareWrite(param0.GetBuffer(), (uint32_t)param1);
}

bool Ota::NativeFirmwareEnd(HRESULT &hr)
{
    (void)hr;
    return NF_Ota_FirmwareEnd();
}

bool Ota::NativeGetRunningSha256(CLR_RT_TypedArray_UINT8 param0, HRESULT &hr)
{
    if (param0.GetBuffer() == NULL || param0.GetSize() < 32)
    {
        hr = CLR_E_INVALID_PARAMETER;
        return false;
    }

    return NF_Ota_GetRunningSha256(param0.GetBuffer());
}

bool Ota::NativeStageBegin(signed int param0, HRESULT &hr)
{
    (void)hr;
    return NF_Ota_StageBegin((uint32_t)param0);
}

bool Ota::NativeStageWrite(CLR_RT_TypedArray_UINT8 param0, signed int param1, HRESULT &hr)
{
    if (param0.GetBuffer() == NULL || param1 < 0 || (uint32_t)param1 > param0.GetSize())
    {
        hr = CLR_E_INVALID_PARAMETER;
        return false;
    }

    return NF_Ota_StageWrite(param0.GetBuffer(), (uint32_t)param1);
}

bool Ota::NativeStageCommit(unsigned int param0, HRESULT &hr)
{
    (void)hr;
    return NF_Ota_StageCommit((uint32_t)param0);
}

bool Ota::NativeCommitFull(HRESULT &hr)
{
    (void)hr;
    return NF_Ota_CommitFull();
}

bool Ota::NativeConfirm(HRESULT &hr)
{
    (void)hr;
    return NF_Ota_Confirm();
}

uint8_t Ota::NativeGetState(HRESULT &hr)
{
    (void)hr;
    return NF_Ota_GetState();
}

bool Ota::NativeIsPendingConfirm(HRESULT &hr)
{
    (void)hr;
    return NF_Ota_IsPendingConfirm();
}

// LEDTREES: reading our own partitions, so a bundle can be served straight from
// them (PartitionBundle): the clr section of the artefact is the running OTA
// slot, the managed section is the deploy partition. Reading flash, unlike
// writing it, does not stall XIP, so serving while the strip is playing is safe.
// Returns either count or -1 (no such partition, a read past the partition, or a
// read error); malformed arguments raise an exception.

signed int Ota::NativeReadRunningFirmware(signed int param0, CLR_RT_TypedArray_UINT8 param1, signed int param2, HRESULT &hr)
{
    if (param1.GetBuffer() == NULL || param0 < 0 || param2 <= 0 || (uint32_t)param2 > param1.GetSize())
    {
        hr = CLR_E_INVALID_PARAMETER;
        return -1;
    }

    return NF_Ota_ReadRunningFirmware((uint32_t)param0, param1.GetBuffer(), (uint32_t)param2);
}

signed int Ota::NativeReadDeploy(signed int param0, CLR_RT_TypedArray_UINT8 param1, signed int param2, HRESULT &hr)
{
    if (param1.GetBuffer() == NULL || param0 < 0 || param2 <= 0 || (uint32_t)param2 > param1.GetSize())
    {
        hr = CLR_E_INVALID_PARAMETER;
        return -1;
    }

    return NF_Ota_ReadDeploy((uint32_t)param0, param1.GetBuffer(), (uint32_t)param2);
}

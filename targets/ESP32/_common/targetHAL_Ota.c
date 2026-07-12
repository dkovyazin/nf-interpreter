//
// Copyright (c) .NET Foundation and Contributors
// See LICENSE file in the project root for full license information.
//

#include <targetHAL_Ota.h>

#if CONFIG_NF_FEATURE_OTA

#include <string.h>
#include <stdlib.h>

#include <sdkconfig.h>
#include <esp_partition.h>
#include <esp_ota_ops.h>
#include <esp_rom_crc.h>
#include <nvs.h>

// The FULL-update rollback path relies on the IDF bootloader marking a freshly
// switched slot PENDING_VERIFY and rolling it back when the app never confirms.
// Without this option esp_ota_set_boot_partition() records the slot as
// UNDEFINED (see esp_ota_ops.c), there is no rollback window and a broken
// nanoCLR image boot-loops with USB recovery as the only way out.
#ifndef CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
#error "NF_FEATURE_OTA requires CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y in the IDF sdkconfig defaults"
#endif

// partition subtypes, mirroring partitions_nanoclr_*_ota.csv
#define OTA_PARTITION_SUBTYPE_DEPLOY ((esp_partition_subtype_t)0x84)
#define OTA_PARTITION_SUBTYPE_STAGE  ((esp_partition_subtype_t)0x85)
#define OTA_PARTITION_SUBTYPE_BACKUP ((esp_partition_subtype_t)0x86)

// NVS storage
#define OTA_NVS_NAMESPACE "nf_ota"
#define OTA_NVS_KEY_STATE "state"
// subtype of the ota_x slot the staged deployment is bound to, or OTA_TARGET_NONE
#define OTA_NVS_KEY_TARGET "target"
#define OTA_NVS_KEY_STAGE_LENGTH "stage_len"
#define OTA_NVS_KEY_STAGE_CRC "stage_crc"
#define OTA_NVS_KEY_BOOT_ATTEMPTS "attempts"

#define OTA_COPY_BUFFER_SIZE 4096

// in-flight nanoCLR image write
static esp_ota_handle_t otaHandle;
static const esp_partition_t *otaUpdatePartition;
// set only by a successful FirmwareEnd: CommitFull refuses to switch the boot
// partition unless the image in otaUpdatePartition was fully written and
// validated in THIS session (statics survive failed sessions and CLR restarts)
static bool otaFirmwareReady;

// in-flight stage write
static const esp_partition_t *stagePartition;
static uint32_t stageWriteOffset;

static const esp_partition_t *FindDataPartition(esp_partition_subtype_t subtype)
{
    return esp_partition_find_first(ESP_PARTITION_TYPE_DATA, subtype, NULL);
}

static bool NvsOpen(nvs_handle_t *handle)
{
    return nvs_open(OTA_NVS_NAMESPACE, NVS_READWRITE, handle) == ESP_OK;
}

static uint8_t NvsGetU8(nvs_handle_t handle, const char *key, uint8_t defaultValue)
{
    uint8_t value = defaultValue;
    nvs_get_u8(handle, key, &value);
    return value;
}

static uint32_t NvsGetU32(nvs_handle_t handle, const char *key, uint32_t defaultValue)
{
    uint32_t value = defaultValue;
    nvs_get_u32(handle, key, &value);
    return value;
}

// copy 'length' bytes from one partition to another (destination erased first)
static bool CopyPartition(const esp_partition_t *destination, const esp_partition_t *source, uint32_t length)
{
    if (length > destination->size || length > source->size)
    {
        return false;
    }

    // erase full destination partition (erase range must be sector aligned)
    if (esp_partition_erase_range(destination, 0, destination->size) != ESP_OK)
    {
        return false;
    }

    uint8_t *buffer = (uint8_t *)malloc(OTA_COPY_BUFFER_SIZE);
    if (!buffer)
    {
        return false;
    }

    bool success = true;
    for (uint32_t offset = 0; offset < length; offset += OTA_COPY_BUFFER_SIZE)
    {
        uint32_t chunk = length - offset;
        if (chunk > OTA_COPY_BUFFER_SIZE)
        {
            chunk = OTA_COPY_BUFFER_SIZE;
        }

        if (esp_partition_read(source, offset, buffer, chunk) != ESP_OK ||
            esp_partition_write(destination, offset, buffer, chunk) != ESP_OK)
        {
            success = false;
            break;
        }
    }

    free(buffer);
    return success;
}

// CRC32 over the first 'length' bytes of a partition
static bool ComputePartitionCrc(const esp_partition_t *partition, uint32_t length, uint32_t *crc)
{
    uint8_t *buffer = (uint8_t *)malloc(OTA_COPY_BUFFER_SIZE);
    if (!buffer)
    {
        return false;
    }

    uint32_t currentCrc = 0;
    bool success = true;
    for (uint32_t offset = 0; offset < length; offset += OTA_COPY_BUFFER_SIZE)
    {
        uint32_t chunk = length - offset;
        if (chunk > OTA_COPY_BUFFER_SIZE)
        {
            chunk = OTA_COPY_BUFFER_SIZE;
        }

        if (esp_partition_read(partition, offset, buffer, chunk) != ESP_OK)
        {
            success = false;
            break;
        }

        // standard (zlib-compatible) CRC32: matches System.IO.Hashing.Crc32
        // on the managed side and zlib.crc32 in the CI bundle packer
        currentCrc = esp_rom_crc32_le(currentCrc, buffer, chunk);
    }

    free(buffer);
    *crc = currentCrc;
    return success;
}

//////////////////////////////////////////////////////////////////////
// nanoCLR image (inactive ota_x slot)
//////////////////////////////////////////////////////////////////////

bool NF_Ota_FirmwareBegin(uint32_t totalSize)
{
    if (otaHandle)
    {
        // discard a previous unfinished session
        esp_ota_abort(otaHandle);
        otaHandle = 0;
    }

    otaFirmwareReady = false;

    otaUpdatePartition = esp_ota_get_next_update_partition(NULL);
    if (!otaUpdatePartition)
    {
        // no OTA layout on this device
        return false;
    }

    if (esp_ota_begin(otaUpdatePartition, totalSize == 0 ? OTA_SIZE_UNKNOWN : totalSize, &otaHandle) != ESP_OK)
    {
        otaUpdatePartition = NULL;
        return false;
    }

    return true;
}

bool NF_Ota_FirmwareWrite(const uint8_t *data, uint32_t length)
{
    if (!otaHandle)
    {
        return false;
    }

    return esp_ota_write(otaHandle, data, length) == ESP_OK;
}

bool NF_Ota_FirmwareEnd(void)
{
    if (!otaHandle)
    {
        return false;
    }

    esp_err_t result = esp_ota_end(otaHandle);
    otaHandle = 0;

    if (result != ESP_OK)
    {
        otaUpdatePartition = NULL;
        return false;
    }

    otaFirmwareReady = true;
    return true;
}

bool NF_Ota_GetRunningSha256(uint8_t sha256[32])
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!running)
    {
        return false;
    }

    // for app partitions this hashes the image content only
    return esp_partition_get_sha256(running, sha256) == ESP_OK;
}

//////////////////////////////////////////////////////////////////////
// managed deployment image (stage partition)
//////////////////////////////////////////////////////////////////////

bool NF_Ota_StageBegin(uint32_t totalSize)
{
    stagePartition = FindDataPartition(OTA_PARTITION_SUBTYPE_STAGE);
    if (!stagePartition || totalSize == 0 || totalSize > stagePartition->size)
    {
        return false;
    }

    stageWriteOffset = 0;

    return esp_partition_erase_range(stagePartition, 0, stagePartition->size) == ESP_OK;
}

bool NF_Ota_StageWrite(const uint8_t *data, uint32_t length)
{
    if (!stagePartition || stageWriteOffset + length > stagePartition->size)
    {
        return false;
    }

    if (esp_partition_write(stagePartition, stageWriteOffset, data, length) != ESP_OK)
    {
        return false;
    }

    stageWriteOffset += length;
    return true;
}

bool NF_Ota_StageCommit(uint32_t crc32)
{
    if (!stagePartition || stageWriteOffset == 0)
    {
        return false;
    }

    // read back and verify
    uint32_t actualCrc;
    if (!ComputePartitionCrc(stagePartition, stageWriteOffset, &actualCrc) || actualCrc != crc32)
    {
        return false;
    }

    nvs_handle_t nvs;
    if (!NvsOpen(&nvs))
    {
        return false;
    }

    bool success = nvs_set_u32(nvs, OTA_NVS_KEY_STAGE_LENGTH, stageWriteOffset) == ESP_OK &&
                   nvs_set_u32(nvs, OTA_NVS_KEY_STAGE_CRC, crc32) == ESP_OK &&
                   nvs_set_u8(nvs, OTA_NVS_KEY_TARGET, OTA_TARGET_NONE) == ESP_OK &&
                   nvs_set_u8(nvs, OTA_NVS_KEY_BOOT_ATTEMPTS, 0) == ESP_OK &&
                   // state written last: everything above is passive until this key flips
                   nvs_set_u8(nvs, OTA_NVS_KEY_STATE, OTA_STATE_STAGED) == ESP_OK &&
                   nvs_commit(nvs) == ESP_OK;

    nvs_close(nvs);
    return success;
}

//////////////////////////////////////////////////////////////////////
// commit / confirm
//////////////////////////////////////////////////////////////////////

bool NF_Ota_CommitFull(void)
{
    // otaFirmwareReady guards against committing a slot whose image was not
    // fully written and validated in this session (aborted write, stale
    // pointer from an earlier attempt); repeating CommitFull after a
    // successful one stays allowed - it is idempotent
    if (!otaUpdatePartition || !otaFirmwareReady)
    {
        return false;
    }

    nvs_handle_t nvs;
    if (!NvsOpen(&nvs))
    {
        return false;
    }

    // bind the staged deployment to the new slot; still passive: the boot-hook
    // applies it only when actually running from that slot
    bool success = NvsGetU8(nvs, OTA_NVS_KEY_STATE, OTA_STATE_IDLE) == OTA_STATE_STAGED &&
                   nvs_set_u8(nvs, OTA_NVS_KEY_TARGET, (uint8_t)otaUpdatePartition->subtype) == ESP_OK &&
                   nvs_commit(nvs) == ESP_OK;
    nvs_close(nvs);

    if (!success)
    {
        return false;
    }

    // COMMIT POINT: atomic otadata write
    return esp_ota_set_boot_partition(otaUpdatePartition) == ESP_OK;
}

bool NF_Ota_Confirm(void)
{
    // cancel IDF rollback if this image is pending verification
    // (returns an error when there is nothing pending - that is fine)
    esp_ota_mark_app_valid_cancel_rollback();

    nvs_handle_t nvs;
    if (!NvsOpen(&nvs))
    {
        return false;
    }

    bool success = nvs_set_u8(nvs, OTA_NVS_KEY_STATE, OTA_STATE_CONFIRMED) == ESP_OK &&
                   nvs_set_u8(nvs, OTA_NVS_KEY_BOOT_ATTEMPTS, 0) == ESP_OK && nvs_commit(nvs) == ESP_OK;

    nvs_close(nvs);
    return success;
}

//////////////////////////////////////////////////////////////////////
// state
//////////////////////////////////////////////////////////////////////

uint8_t NF_Ota_GetState(void)
{
    nvs_handle_t nvs;
    if (!NvsOpen(&nvs))
    {
        return OTA_STATE_IDLE;
    }

    uint8_t state = NvsGetU8(nvs, OTA_NVS_KEY_STATE, OTA_STATE_IDLE);
    nvs_close(nvs);
    return state;
}

bool NF_Ota_IsPendingConfirm(void)
{
    if (NF_Ota_GetState() == OTA_STATE_APPLIED)
    {
        return true;
    }

    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t imageState;
    if (running && esp_ota_get_state_partition(running, &imageState) == ESP_OK)
    {
        return imageState == ESP_OTA_IMG_PENDING_VERIFY;
    }

    return false;
}

void NF_Ota_NotifyDeploymentErased(void)
{
    nvs_handle_t nvs;
    if (!NvsOpen(&nvs))
    {
        return;
    }

    // a manual deployment supersedes any in-flight OTA update: without this
    // the boot-hook would apply the stale stage over the fresh deployment
    // (STAGED) or restore 'backup' over it after a few reboots (APPLIED)
    uint8_t state = NvsGetU8(nvs, OTA_NVS_KEY_STATE, OTA_STATE_IDLE);
    if (state == OTA_STATE_STAGED || state == OTA_STATE_COPYING || state == OTA_STATE_APPLIED)
    {
        nvs_set_u8(nvs, OTA_NVS_KEY_STATE, OTA_STATE_IDLE);
        nvs_commit(nvs);
    }

    nvs_close(nvs);
}

//////////////////////////////////////////////////////////////////////
// boot-hook
//////////////////////////////////////////////////////////////////////

static void RestoreBackup(
    nvs_handle_t nvs,
    const esp_partition_t *deploy,
    const esp_partition_t *backup)
{
    // idempotent: 'backup' is not modified, so a power loss here just repeats
    // the restore on the next boot (state stays below CONFIRMED)
    if (CopyPartition(deploy, backup, deploy->size))
    {
        nvs_set_u8(nvs, OTA_NVS_KEY_STATE, OTA_STATE_ROLLED_BACK);
        nvs_commit(nvs);
    }
}

void NF_Ota_ApplyPending(void)
{
    nvs_handle_t nvs;
    if (!NvsOpen(&nvs))
    {
        return;
    }

    uint8_t state = NvsGetU8(nvs, OTA_NVS_KEY_STATE, OTA_STATE_IDLE);
    if (state == OTA_STATE_IDLE || state == OTA_STATE_CONFIRMED || state == OTA_STATE_ROLLED_BACK)
    {
        nvs_close(nvs);
        return;
    }

    const esp_partition_t *deploy = FindDataPartition(OTA_PARTITION_SUBTYPE_DEPLOY);
    const esp_partition_t *stage = FindDataPartition(OTA_PARTITION_SUBTYPE_STAGE);
    const esp_partition_t *backup = FindDataPartition(OTA_PARTITION_SUBTYPE_BACKUP);
    if (!deploy || !stage || !backup)
    {
        // not an OTA partition layout
        nvs_close(nvs);
        return;
    }

    uint8_t target = NvsGetU8(nvs, OTA_NVS_KEY_TARGET, OTA_TARGET_NONE);
    uint32_t stageLength = NvsGetU32(nvs, OTA_NVS_KEY_STAGE_LENGTH, 0);
    uint32_t stageCrc = NvsGetU32(nvs, OTA_NVS_KEY_STAGE_CRC, 0);

    const esp_partition_t *running = esp_ota_get_running_partition();
    bool isFull = target != OTA_TARGET_NONE;
    bool onTargetSlot = isFull && running && (uint8_t)running->subtype == target;

    switch (state)
    {
        case OTA_STATE_STAGED:

            if (isFull && !onTargetSlot)
            {
                // committed to another slot but running here: either not yet
                // rebooted or the bootloader rolled the new slot back before
                // the deployment was touched - stay passive, deploy is intact
                break;
            }

            // save the current deployment for rollback
            if (!CopyPartition(backup, deploy, deploy->size))
            {
                break;
            }

            nvs_set_u8(nvs, OTA_NVS_KEY_STATE, OTA_STATE_COPYING);
            nvs_commit(nvs);

            // apply the staged image
            __attribute__((fallthrough));

        case OTA_STATE_COPYING:

            if (isFull && !onTargetSlot)
            {
                // slot rolled back while the deployment was being rewritten
                RestoreBackup(nvs, deploy, backup);
                break;
            }

            if (stageLength == 0 || stageLength > deploy->size || !CopyPartition(deploy, stage, stageLength))
            {
                RestoreBackup(nvs, deploy, backup);
                break;
            }

            // verify what actually landed in deploy
            {
                uint32_t actualCrc;
                if (!ComputePartitionCrc(deploy, stageLength, &actualCrc) || actualCrc != stageCrc)
                {
                    RestoreBackup(nvs, deploy, backup);
                    break;
                }
            }

            nvs_set_u8(nvs, OTA_NVS_KEY_BOOT_ATTEMPTS, 1);
            nvs_set_u8(nvs, OTA_NVS_KEY_STATE, OTA_STATE_APPLIED);
            nvs_commit(nvs);
            break;

        case OTA_STATE_APPLIED:

            if (isFull)
            {
                if (!onTargetSlot)
                {
                    // the IDF bootloader rolled the slot back (new nanoCLR never
                    // confirmed): bring the old deployment back too
                    RestoreBackup(nvs, deploy, backup);
                }
                // else: rollback window is managed by the IDF pending-verify
                // mechanism; nothing to do here
                break;
            }

            // light update: count boots without managed confirmation
            {
                uint8_t attempts = NvsGetU8(nvs, OTA_NVS_KEY_BOOT_ATTEMPTS, 1) + 1;
                if (attempts > OTA_MAX_BOOT_ATTEMPTS)
                {
                    RestoreBackup(nvs, deploy, backup);
                }
                else
                {
                    nvs_set_u8(nvs, OTA_NVS_KEY_BOOT_ATTEMPTS, attempts);
                    nvs_commit(nvs);
                }
            }
            break;

        default:
            break;
    }

    nvs_close(nvs);
}

#endif // CONFIG_NF_FEATURE_OTA

//
// Copyright (c) .NET Foundation and Contributors
// See LICENSE file in the project root for full license information.
//

#include <targetHAL_Ota.h>

#if CONFIG_NF_FEATURE_OTA

#include <string.h>
#include <stdlib.h>
#include <stddef.h>

#include <sdkconfig.h>
#include <esp_partition.h>
#include <esp_ota_ops.h>
#include <esp_rom_crc.h>

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
#define OTA_PARTITION_SUBTYPE_STATE  ((esp_partition_subtype_t)0x87)

#define OTA_COPY_BUFFER_SIZE 4096

//////////////////////////////////////////////////////////////////////
// OTA state record
//
// The state machine is persisted in its own raw 'ota_state' partition
// (two 4 KB sectors) instead of NVS, so it survives an NVS erase or
// corruption recovery. Writes ping-pong between the sectors, like the
// IDF otadata partition: record with sequence N lives in sector N % 2,
// a new record goes to the other sector, so a power loss mid-write
// always leaves the previous record intact. A whole state transition
// is one CRC-protected record write - atomic by construction.
//////////////////////////////////////////////////////////////////////

#define OTA_STATE_MAGIC       0x544F464E // 'NFOT', little-endian
#define OTA_STATE_SECTOR_SIZE 4096

typedef struct __attribute__((packed))
{
    uint32_t magic;
    // monotonically increasing; valid records start at 1
    uint32_t sequence;
    uint8_t state;
    // subtype of the ota_x slot the staged deployment is bound to, or OTA_TARGET_NONE
    uint8_t target;
    uint8_t attempts;
    uint8_t reserved;
    uint32_t stageLength;
    uint32_t stageCrc;
    // zlib crc32 over all preceding bytes
    uint32_t crc;
} OtaStateRecord;

static const esp_partition_t *statePartition;
static OtaStateRecord currentState;
static bool stateLoaded;

static uint32_t StateRecordCrc(const OtaStateRecord *record)
{
    return esp_rom_crc32_le(0, (const uint8_t *)record, offsetof(OtaStateRecord, crc));
}

// load the newest valid record into currentState; defaults to IDLE when the
// partition is missing or holds no valid record
static void StateLoad(void)
{
    if (stateLoaded)
    {
        return;
    }

    memset(&currentState, 0, sizeof(currentState));
    currentState.magic = OTA_STATE_MAGIC;
    currentState.state = OTA_STATE_IDLE;
    currentState.target = OTA_TARGET_NONE;

    if (!statePartition)
    {
        statePartition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, OTA_PARTITION_SUBTYPE_STATE, NULL);
    }

    if (statePartition)
    {
        for (int sector = 0; sector < 2; sector++)
        {
            OtaStateRecord record;
            if (esp_partition_read(statePartition, sector * OTA_STATE_SECTOR_SIZE, &record, sizeof(record)) != ESP_OK)
            {
                continue;
            }

            if (record.magic != OTA_STATE_MAGIC || record.crc != StateRecordCrc(&record))
            {
                continue;
            }

            // valid sequences start at 1, the IDLE default holds sequence 0
            if (record.sequence > currentState.sequence)
            {
                currentState = record;
            }
        }
    }

    stateLoaded = true;
}

// persist currentState as a new record in the inactive sector
static bool StateStore(void)
{
    StateLoad();

    if (!statePartition)
    {
        return false;
    }

    currentState.sequence++;
    currentState.crc = StateRecordCrc(&currentState);

    uint32_t offset = (currentState.sequence % 2) * OTA_STATE_SECTOR_SIZE;
    if (esp_partition_erase_range(statePartition, offset, OTA_STATE_SECTOR_SIZE) != ESP_OK ||
        esp_partition_write(statePartition, offset, &currentState, sizeof(currentState)) != ESP_OK)
    {
        // keep targeting the same sector on a retry; the current on-flash
        // record (previous sector) is still intact
        currentState.sequence--;
        return false;
    }

    return true;
}

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
        stagePartition = NULL;
        return false;
    }

    // starting a new session cancels a committed-but-not-applied update: the
    // erase below invalidates the staged image, and a record left in STAGED
    // would send the boot-hook copying erased flash on the next boot (false
    // rollback; after CommitFull - a rollback of a perfectly good firmware)
    StateLoad();
    if (currentState.state == OTA_STATE_STAGED)
    {
        currentState.state = OTA_STATE_IDLE;
        if (!StateStore())
        {
            stagePartition = NULL;
            return false;
        }
    }

    stageWriteOffset = 0;

    if (esp_partition_erase_range(stagePartition, 0, stagePartition->size) != ESP_OK)
    {
        stagePartition = NULL;
        return false;
    }

    return true;
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

    // one atomic record write: nothing is committed until it lands.
    // In a FULL session (a firmware image was validated before staging) the
    // record is bound to the new slot right away: a reboot in the window
    // between this commit and CommitFull must NOT light-apply the new managed
    // image against the still-running old nanoCLR - a bound record stays
    // passive until the device actually boots from the target slot
    StateLoad();
    currentState.stageLength = stageWriteOffset;
    currentState.stageCrc = crc32;
    currentState.target = otaFirmwareReady ? (uint8_t)otaUpdatePartition->subtype : OTA_TARGET_NONE;
    currentState.attempts = 0;
    currentState.state = OTA_STATE_STAGED;
    if (!StateStore())
    {
        return false;
    }

    // close the write session: a stray StageWrite after the commit point must
    // fail loudly instead of silently appending to a committed image
    stagePartition = NULL;
    return true;
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

    // bind the staged deployment to the new slot; still passive: the boot-hook
    // applies it only when actually running from that slot
    StateLoad();
    if (currentState.state != OTA_STATE_STAGED)
    {
        return false;
    }

    currentState.target = (uint8_t)otaUpdatePartition->subtype;
    if (!StateStore())
    {
        return false;
    }

    // COMMIT POINT: atomic otadata write
    return esp_ota_set_boot_partition(otaUpdatePartition) == ESP_OK;
}

bool NF_Ota_Confirm(void)
{
    StateLoad();

    if (currentState.state == OTA_STATE_APPLIED)
    {
        // cancel IDF rollback if this image is pending verification
        // (returns an error when there is nothing pending - that is fine)
        esp_ota_mark_app_valid_cancel_rollback();

        currentState.state = OTA_STATE_CONFIRMED;
        currentState.attempts = 0;
        return StateStore();
    }

    if (currentState.state == OTA_STATE_CONFIRMED)
    {
        // idempotent re-confirm (managed retry)
        esp_ota_mark_app_valid_cancel_rollback();
        return true;
    }

    if (currentState.state == OTA_STATE_IDLE)
    {
        // defensive: the running image awaits verification but the state
        // record carries no update context (fresh ota_state partition) -
        // cancel the IDF rollback so the running bundle keeps running
        const esp_partition_t *running = esp_ota_get_running_partition();
        esp_ota_img_states_t imageState;
        if (running && esp_ota_get_state_partition(running, &imageState) == ESP_OK &&
            imageState == ESP_OTA_IMG_PENDING_VERIFY)
        {
            return esp_ota_mark_app_valid_cancel_rollback() == ESP_OK;
        }
    }

    // STAGED / COPYING / ROLLED_BACK (and IDLE with nothing pending):
    // confirming here would corrupt the state machine - a staged-but-not-yet-
    // applied update would silently vanish, a rollback would be masked as
    // confirmed - so refuse without touching the record
    return false;
}

//////////////////////////////////////////////////////////////////////
// state
//////////////////////////////////////////////////////////////////////

uint8_t NF_Ota_GetState(void)
{
    StateLoad();
    return currentState.state;
}

bool NF_Ota_IsPendingConfirm(void)
{
    StateLoad();

    if (currentState.state == OTA_STATE_APPLIED)
    {
        return true;
    }

    // mirror the NF_Ota_Confirm() contract: outside of APPLIED a confirmation
    // is only expected for a pending-verify image with no update context
    // (otherwise Confirm() refuses and this must not report true)
    if (currentState.state != OTA_STATE_IDLE)
    {
        return false;
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
    // a manual deployment supersedes any in-flight OTA update: without this
    // the boot-hook would apply the stale stage over the fresh deployment
    // (STAGED) or restore 'backup' over it after a few reboots (APPLIED)
    StateLoad();
    if (currentState.state == OTA_STATE_STAGED || currentState.state == OTA_STATE_COPYING ||
        currentState.state == OTA_STATE_APPLIED)
    {
        currentState.state = OTA_STATE_IDLE;
        StateStore();
    }
}

//////////////////////////////////////////////////////////////////////
// boot-hook
//////////////////////////////////////////////////////////////////////

static void RestoreBackup(const esp_partition_t *deploy, const esp_partition_t *backup)
{
    // idempotent: 'backup' is not modified, so a power loss here just repeats
    // the restore on the next boot (state stays below CONFIRMED)
    if (CopyPartition(deploy, backup, deploy->size))
    {
        currentState.state = OTA_STATE_ROLLED_BACK;
        StateStore();
    }
}

void NF_Ota_ApplyPending(void)
{
    StateLoad();

    uint8_t state = currentState.state;
    if (state == OTA_STATE_IDLE || state == OTA_STATE_CONFIRMED || state == OTA_STATE_ROLLED_BACK)
    {
        return;
    }

    const esp_partition_t *deploy = FindDataPartition(OTA_PARTITION_SUBTYPE_DEPLOY);
    const esp_partition_t *stage = FindDataPartition(OTA_PARTITION_SUBTYPE_STAGE);
    const esp_partition_t *backup = FindDataPartition(OTA_PARTITION_SUBTYPE_BACKUP);
    if (!deploy || !stage || !backup)
    {
        // not an OTA partition layout
        return;
    }

    const esp_partition_t *running = esp_ota_get_running_partition();
    bool isFull = currentState.target != OTA_TARGET_NONE;
    bool onTargetSlot = isFull && running && (uint8_t)running->subtype == currentState.target;

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

            currentState.state = OTA_STATE_COPYING;
            if (!StateStore())
            {
                break;
            }

            // apply the staged image
            __attribute__((fallthrough));

        case OTA_STATE_COPYING:

            if (isFull && !onTargetSlot)
            {
                // slot rolled back while the deployment was being rewritten
                RestoreBackup(deploy, backup);
                break;
            }

            if (currentState.stageLength == 0 || currentState.stageLength > deploy->size ||
                !CopyPartition(deploy, stage, currentState.stageLength))
            {
                RestoreBackup(deploy, backup);
                break;
            }

            // verify what actually landed in deploy
            {
                uint32_t actualCrc;
                if (!ComputePartitionCrc(deploy, currentState.stageLength, &actualCrc) ||
                    actualCrc != currentState.stageCrc)
                {
                    RestoreBackup(deploy, backup);
                    break;
                }
            }

            currentState.attempts = 1;
            currentState.state = OTA_STATE_APPLIED;
            StateStore();
            break;

        case OTA_STATE_APPLIED:

            if (isFull)
            {
                if (!onTargetSlot)
                {
                    // the IDF bootloader rolled the slot back (new nanoCLR never
                    // confirmed): bring the old deployment back too
                    RestoreBackup(deploy, backup);
                }
                // else: rollback window is managed by the IDF pending-verify
                // mechanism; nothing to do here
                break;
            }

            // light update: count boots without managed confirmation
            if (currentState.attempts >= OTA_MAX_BOOT_ATTEMPTS)
            {
                RestoreBackup(deploy, backup);
            }
            else
            {
                currentState.attempts++;
                StateStore();
            }
            break;

        default:
            break;
    }
}

#endif // CONFIG_NF_FEATURE_OTA

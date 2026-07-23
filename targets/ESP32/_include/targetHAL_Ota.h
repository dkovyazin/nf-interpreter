//
// Copyright (c) .NET Foundation and Contributors
// See LICENSE file in the project root for full license information.
//

#ifndef TARGETHAL_OTA_H
#define TARGETHAL_OTA_H

#include <stdint.h>
#include <stdbool.h>

// OTA update support (NF_FEATURE_OTA).
//
// Two update flavours share one state machine, persisted in the dedicated
// 'ota_state' raw partition (subtype 0x87, two 4 KB sectors written in a
// ping-pong pattern with CRC, like the IDF otadata partition). The state is
// deliberately NOT in NVS so it survives an NVS erase/corruption recovery.
// - FULL:  a new nanoCLR image is written to the inactive ota_x slot and a new
//          managed deployment image is written to the 'stage' partition.
//          Commit point is esp_ota_set_boot_partition() (atomic). The staged
//          deployment is applied by the boot-hook ONLY when running from the
//          target slot; if the IDF bootloader rolls the slot back, the boot-hook
//          restores the previous deployment from 'backup'.
// - LIGHT: only the managed deployment changes. Commit point is the ota_state
//          record write. Rollback is driven by a boot-attempt counter: if the
//          managed stack fails to confirm within OTA_MAX_BOOT_ATTEMPTS boots,
//          the boot-hook restores 'backup'.
//
// All functions return true on success. The API is not thread-safe: it is
// intended to be driven by a single managed updater thread.

#ifdef __cplusplus
extern "C"
{
#endif

    typedef enum NF_Ota_State_
    {
        OTA_STATE_IDLE = 0,
        // stage partition holds a verified deployment image, not yet applied
        OTA_STATE_STAGED = 1,
        // 'backup' holds the previous deployment; deploy partition is being rewritten
        OTA_STATE_COPYING = 2,
        // deploy partition holds the new image; waiting for managed confirmation
        OTA_STATE_APPLIED = 3,
        // update confirmed by the managed stack
        OTA_STATE_CONFIRMED = 4,
        // update failed; previous deployment restored from 'backup'
        OTA_STATE_ROLLED_BACK = 5,
    } NF_Ota_State;

// OtaStateRecord 'target' value for a light update (no slot switch)
#define OTA_TARGET_NONE 0xFF

// light updates: boots without managed confirmation before rollback
#define OTA_MAX_BOOT_ATTEMPTS 3

    // ---- nanoCLR image (inactive ota_x slot) ----

    // start writing a new nanoCLR image to the inactive slot; totalSize 0 = unknown
    bool NF_Ota_FirmwareBegin(uint32_t totalSize);
    bool NF_Ota_FirmwareWrite(const uint8_t *data, uint32_t length);
    // finalize and validate the image: magic, sha256, and - with
    // SECURE_SIGNED_ON_UPDATE builds - the RSA app signature, so an
    // unsigned/foreign image fails right here
    bool NF_Ota_FirmwareEnd(void);
    // sha256 of the currently running nanoCLR image (to decide full vs light)
    bool NF_Ota_GetRunningSha256(uint8_t sha256[32]);

    // ---- managed deployment image (stage partition) ----

    // erase the stage partition and start writing a new deployment image
    bool NF_Ota_StageBegin(uint32_t totalSize);
    bool NF_Ota_StageWrite(const uint8_t *data, uint32_t length);
    // verify the staged image against crc32 and persist STAGED state (light
    // commit point). When a firmware image was validated in this session
    // (FULL flow) the record is bound to that slot and stays passive until
    // the device boots from it - so a reboot before CommitFull cannot
    // light-apply the new deployment against the old nanoCLR.
    bool NF_Ota_StageCommit(uint32_t crc32);

    // ---- commit / confirm ----

    // FULL commit point: bind the staged deployment to the slot written by
    // FirmwareBegin/End and switch the boot partition (atomic). Reboot after this.
    bool NF_Ota_CommitFull(void);
    // confirm the running bundle: cancels IDF rollback and finishes the state machine
    bool NF_Ota_Confirm(void);

    // ---- partition reads (serving the bundle from own partitions) ----

    // read 'count' bytes at 'offset' of the running nanoCLR slot / the deploy
    // partition. Returns count, or -1 (no partition / out of range / io error).
    // Flash reads do not stall XIP - safe while the LED output task is running.
    int32_t NF_Ota_ReadRunningFirmware(uint32_t offset, uint8_t *buffer, uint32_t count);
    int32_t NF_Ota_ReadDeploy(uint32_t offset, uint8_t *buffer, uint32_t count);

    // ---- state ----

    uint8_t NF_Ota_GetState(void);
    // true when the managed stack is expected to call NF_Ota_Confirm()
    bool NF_Ota_IsPendingConfirm(void);

    // boot-hook: apply/rollback a staged deployment. Must run after nvs_flash_init()
    // and BEFORE the CLR starts (deploy partition must not be in use).
    void NF_Ota_ApplyPending(void);

    // called when the deploy region is rewritten OUTSIDE of the OTA flow (Wire
    // Protocol deployment from VS/nanoff): cancels any pending OTA state so the
    // boot-hook does not clobber the fresh deployment with a stale stage or
    // roll it back from 'backup' on the next boot
    void NF_Ota_NotifyDeploymentErased(void);

#ifdef __cplusplus
}
#endif

#endif // TARGETHAL_OTA_H

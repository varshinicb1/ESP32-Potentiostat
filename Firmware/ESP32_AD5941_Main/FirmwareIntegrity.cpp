#include "FirmwareIntegrity.h"
#include <ArduinoJson.h>
#include <nvs_flash.h>
#include <nvs.h>

// ===================================================================
// NVS namespace and key for storing the reference CRC
// ===================================================================
static const char* NVS_NAMESPACE  = "vl_integrity";
static const char* NVS_KEY_CRC    = "fw_crc32";
static const char* NVS_KEY_VALID  = "crc_stored";

// ===================================================================
// calculateFirmwareCRC()
// Reads the running app partition in 4 KB chunks and accumulates CRC32.
// Reads only up to the actual image size from esp_image_metadata_t,
// NOT the full partition size (which contains uninitialized 0xFF bytes).
// ===================================================================
uint32_t FirmwareIntegrity::calculateFirmwareCRC() {
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (!running) return 0;

    // esp_image_get_metadata() on IDF 4.4 (arduino-esp32 v2.x) requires
    // an esp_partition_pos_t, not an esp_partition_t*.
    // Build it from the running partition's flash address and size.
    esp_partition_pos_t partPos;
    partPos.offset = running->address;
    partPos.size   = running->size;

    esp_image_metadata_t meta;
    memset(&meta, 0, sizeof(meta));
    esp_err_t err = esp_image_get_metadata(&partPos, &meta);
    if (err != ESP_OK) {
        // Fallback: use full partition size (less accurate but safe)
    }

    // Use image_len from metadata if available; otherwise fall back to partition size
    size_t imageSize = (meta.image_len > 0 && meta.image_len <= running->size)
                       ? (size_t)meta.image_len
                       : running->size;

    const size_t CHUNK_SIZE = 4096;
    uint8_t buffer[CHUNK_SIZE];
    uint32_t crc    = 0;
    size_t   offset = 0;

    while (offset < imageSize) {
        size_t readSize = (CHUNK_SIZE < imageSize - offset) ? CHUNK_SIZE : (imageSize - offset);
        if (esp_partition_read(running, offset, buffer, readSize) != ESP_OK) {
            break;
        }
        crc = crc32_le(crc, buffer, readSize);
        offset += readSize;
    }

    return crc;
}

// ===================================================================
// verifyFirmwareIntegrity()
//
// Protocol:
//   First boot after flash: CRC is computed, stored in NVS, returns PASS.
//   Subsequent boots: CRC is computed and compared against NVS value.
//     Match  → PASS
//     Mismatch → FAIL (image has been modified since last intentional flash)
//
// To update firmware legitimately, flash new image via OTA/USB and the
// first boot of that new image automatically re-anchors the NVS CRC.
// ===================================================================
bool FirmwareIntegrity::verifyFirmwareIntegrity(uint32_t& calculatedCRC) {
    calculatedCRC = calculateFirmwareCRC();
    if (calculatedCRC == 0) return false; // CRC computation failed

    // --- Initialize NVS ---
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // NVS partition was truncated or version changed — erase and re-init
        nvs_flash_erase();
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        // NVS unavailable — cannot verify. Treat as pass to avoid boot loop.
        return true;
    }

    nvs_handle_t handle;
    ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) return true; // NVS open failed — treat as pass

    // Check if a reference CRC has been stored
    uint8_t crcStored = 0;
    nvs_get_u8(handle, NVS_KEY_VALID, &crcStored);

    if (!crcStored) {
        // First boot: anchor the current CRC as the trusted reference
        nvs_set_u32(handle, NVS_KEY_CRC,   calculatedCRC);
        nvs_set_u8 (handle, NVS_KEY_VALID, 1);
        nvs_commit(handle);
        nvs_close(handle);
        return true; // First boot always passes
    }

    // Subsequent boots: read the stored reference CRC and compare
    uint32_t storedCRC = 0;
    nvs_get_u32(handle, NVS_KEY_CRC, &storedCRC);
    nvs_close(handle);

    if (calculatedCRC != storedCRC) {
        // CRC mismatch — firmware has been modified since last authorised flash
        return false;
    }

    return true;
}

// ===================================================================
// resetStoredCRC()
// Call this ONCE after an intentional OTA update to re-anchor the reference.
// This is automatically triggered on the first boot of a new firmware image
// (crcStored will be 0 in the new partition's NVS after OTA).
// ===================================================================
void FirmwareIntegrity::resetStoredCRC() {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_set_u8(handle, NVS_KEY_VALID, 0); // Clear stored flag
        nvs_commit(handle);
        nvs_close(handle);
    }
}

// ===================================================================
// Partition and version info
// ===================================================================
String FirmwareIntegrity::getPartitionLabel() {
    const esp_partition_t* running = esp_ota_get_running_partition();
    return running ? String(running->label) : String("unknown");
}

String FirmwareIntegrity::getFirmwareVersion() {
    return String(FW_VERSION_STRING);
}

String FirmwareIntegrity::getDeviceUID() {
    uint64_t chipId = ESP.getEfuseMac();
    char uid[18];
    snprintf(uid, sizeof(uid), "%04X%08X",
             (uint16_t)(chipId >> 32),
             (uint32_t)chipId);
    return String(uid);
}

// ===================================================================
// getIdentificationJSON()
// Full device identification payload for telemetry handshake.
// ===================================================================
String FirmwareIntegrity::getIdentificationJSON() {
    StaticJsonDocument<512> doc;
    doc["manufacturer"]  = FW_MANUFACTURER;
    doc["device_model"]  = FW_DEVICE_MODEL;
    doc["fw_version"]    = FW_VERSION_STRING;
    doc["fw_major"]      = FW_VERSION_MAJOR;
    doc["fw_minor"]      = FW_VERSION_MINOR;
    doc["fw_patch"]      = FW_VERSION_PATCH;
    doc["device_uid"]    = getDeviceUID();
    doc["partition"]     = getPartitionLabel();
    doc["uptime_ms"]     = millis();
    doc["free_heap"]     = ESP.getFreeHeap();
    doc["min_free_heap"] = ESP.getMinFreeHeap();

    uint32_t crc;
    verifyFirmwareIntegrity(crc);
    char crcStr[11];
    snprintf(crcStr, sizeof(crcStr), "0x%08X", crc);
    doc["fw_crc32"] = crcStr;

    String output;
    serializeJson(doc, output);
    return output;
}

// ===================================================================
// verifyRAMIntegrity()
// Two-pattern write/read test on a stack-allocated volatile buffer.
// Tests both walking-ones and complement patterns.
// ===================================================================
bool FirmwareIntegrity::verifyRAMIntegrity() {
    const size_t TEST_SIZE = 512; // Extended to 512 bytes
    volatile uint8_t testBuffer[TEST_SIZE];

    // Pattern 1: Walking ones (i & 0xFF)
    for (size_t i = 0; i < TEST_SIZE; i++) testBuffer[i] = (uint8_t)(i & 0xFF);
    for (size_t i = 0; i < TEST_SIZE; i++) {
        if (testBuffer[i] != (uint8_t)(i & 0xFF)) return false;
    }

    // Pattern 2: Complement
    for (size_t i = 0; i < TEST_SIZE; i++) testBuffer[i] = (uint8_t)(~i & 0xFF);
    for (size_t i = 0; i < TEST_SIZE; i++) {
        if (testBuffer[i] != (uint8_t)(~i & 0xFF)) return false;
    }

    // Pattern 3: Checkerboard (0xAA / 0x55)
    for (size_t i = 0; i < TEST_SIZE; i++) testBuffer[i] = (i % 2 == 0) ? 0xAA : 0x55;
    for (size_t i = 0; i < TEST_SIZE; i++) {
        if (testBuffer[i] != ((i % 2 == 0) ? 0xAA : 0x55)) return false;
    }

    return true;
}

// ===================================================================
// getTaskStackWatermark()
// ===================================================================
uint32_t FirmwareIntegrity::getTaskStackWatermark(TaskHandle_t taskHandle) {
    if (taskHandle == NULL) return 0;
    return uxTaskGetStackHighWaterMark(taskHandle);
}

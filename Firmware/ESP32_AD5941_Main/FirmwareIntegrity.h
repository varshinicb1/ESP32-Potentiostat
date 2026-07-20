#ifndef FIRMWARE_INTEGRITY_H
#define FIRMWARE_INTEGRITY_H

#include <Arduino.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_image_format.h>
#include <rom/crc.h>

// ===================================================================
// Firmware Version & Build Identification (IEC 62304 §5.8)
// ===================================================================
#define FW_VERSION_MAJOR    0
#define FW_VERSION_MINOR    2
#define FW_VERSION_PATCH    0
#define FW_VERSION_STRING   "0.2.0"
#define FW_DEVICE_MODEL     "AnalyteX-V1"
#define FW_MANUFACTURER     "VidyuthLabs Technologies Pvt Ltd"

class FirmwareIntegrity {
public:
    // Calculate CRC32 of the running firmware image (actual image size, not partition)
    static uint32_t calculateFirmwareCRC();

    // Verify firmware integrity against NVS-stored reference CRC.
    // First boot: stores CRC and returns true.
    // Subsequent boots: compares and returns true only if CRC matches.
    static bool verifyFirmwareIntegrity(uint32_t& calculatedCRC);

    // Reset the stored NVS CRC reference (call after intentional OTA update).
    // Normally called automatically on first boot of new firmware image.
    static void resetStoredCRC();

    // Partition and version info
    static String getPartitionLabel();
    static String getFirmwareVersion();
    static String getDeviceUID();

    // Full JSON identification payload for telemetry handshake
    static String getIdentificationJSON();

    // RAM integrity: multi-pattern write/read test
    static bool verifyRAMIntegrity();

    // FreeRTOS stack watermark for a given task
    static uint32_t getTaskStackWatermark(TaskHandle_t taskHandle);
};

#endif // FIRMWARE_INTEGRITY_H

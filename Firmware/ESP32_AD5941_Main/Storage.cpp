#include "Storage.h"

// ===================================================================
// Static member definitions
// ===================================================================
File              Storage::activeFile;
String            Storage::activeFileName = "";
bool              Storage::sdReady        = false;
SemaphoreHandle_t Storage::sdMutex        = nullptr;

// ===================================================================
// Internal mutex helpers
// ===================================================================
bool Storage::acquireMutex() {
    if (sdMutex == nullptr) return false;
    return (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(100)) == pdTRUE);
}

void Storage::releaseMutex() {
    if (sdMutex != nullptr) {
        xSemaphoreGive(sdMutex);
    }
}

// ===================================================================
// Storage::init()
// Must be called once before FreeRTOS tasks start.
// ===================================================================
void Storage::init() {
    // Create mutex before tasks start (safe to call from setup())
    sdMutex = xSemaphoreCreateMutex();
    configASSERT(sdMutex != nullptr);

    if (SD.begin(SD_CS_PIN)) {
        sdReady = true;
    } else {
        sdReady = false;
    }
}

bool Storage::isReady() {
    return sdReady;
}

// ===================================================================
// Storage::createNewFile()
// Finds a unique filename and opens it for writing.
// ===================================================================
bool Storage::createNewFile(const char* techniqueName) {
    if (!sdReady) return false;
    if (!acquireMutex()) return false;

    // Close any currently open file first
    if (activeFile) {
        activeFile.close();
        activeFileName = "";
    }

    // Find a unique, non-colliding filename: /CV_001.csv, /CV_002.csv, ...
    char filename[40];
    bool found = false;
    for (uint32_t fileIndex = 1; fileIndex < 10000; fileIndex++) {
        snprintf(filename, sizeof(filename), "/%s_%04u.csv", techniqueName, fileIndex);
        if (!SD.exists(filename)) {
            found = true;
            break;
        }
    }

    bool success = false;
    if (found) {
        activeFileName = String(filename);
        activeFile = SD.open(activeFileName, FILE_WRITE);
        success = (bool)activeFile;
    }

    releaseMutex();
    return success;
}

// ===================================================================
// Storage::writeCSVHeader()
// ===================================================================
void Storage::writeCSVHeader(const char* columns) {
    if (!acquireMutex()) return;
    if (activeFile) {
        activeFile.println(columns);
        activeFile.flush();
    }
    releaseMutex();
}

// ===================================================================
// Storage::logDataRow()
// High-frequency call during measurements — mutex-protected.
// ===================================================================
void Storage::logDataRow(const String& row) {
    if (!acquireMutex()) return;
    if (activeFile) {
        activeFile.println(row);
        activeFile.flush();
    }
    releaseMutex();
}

// ===================================================================
// Storage::closeActiveFile()
// ===================================================================
void Storage::closeActiveFile() {
    if (!acquireMutex()) return;
    if (activeFile) {
        activeFile.close();
        activeFileName = "";
    }
    releaseMutex();
}

// ===================================================================
// Storage::logEvent()
// Opens /syslog.txt in APPEND mode, writes a timestamped entry, closes.
// FILE_APPEND is explicit to avoid library-version ambiguity.
// ===================================================================
void Storage::logEvent(const char* severity, const String& message) {
    if (!sdReady) return;
    if (!acquireMutex()) return;

    // Use FILE_APPEND explicitly — guaranteed to open at end on all SD lib versions
    File logFile = SD.open("/syslog.txt", FILE_APPEND);
    if (logFile) {
        // Format: [uptime_ms][SEVERITY] message
        logFile.print("[");
        logFile.print(millis());
        logFile.print("][");
        logFile.print(severity);
        logFile.print("] ");
        logFile.println(message);
        logFile.flush();
        logFile.close();
    }

    releaseMutex();
}

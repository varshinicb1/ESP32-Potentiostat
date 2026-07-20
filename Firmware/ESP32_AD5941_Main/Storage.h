#ifndef STORAGE_H
#define STORAGE_H

#include <Arduino.h>
#include <FS.h>
#include <SD.h>
#include <SPI.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// Corrected against the real schematic (net "CS.SD" -> ESP32-S3 GPIO14; was
// 42, which is actually TOUCH_CS on this board — see AD5941_Driver.h/
// Display_LVGL.cpp for the rest of the shared-bus pin corrections).
//
// NOTE: Storage.cpp calls SD.begin(SD_CS_PIN) with no explicit SPIClass or
// MOSI/MISO/SCK arguments, so it relies on the Arduino core's *default*
// global `SPI` object pin mapping for this board profile. That needs to
// actually resolve to GPIO11/12/13 (the real shared bus — see
// AD5941_Driver.h's shared-bus note) for this to work; it has not been
// confirmed against whichever exact "ESP32S3 Dev Module"-style board
// definition this project compiles against. If it doesn't match, either
// pass an explicit SPIClass/pin set to SD.begin(), or set
// SPI.begin(12, 13, 11, SD_CS_PIN) before calling SD.begin().
#define SD_CS_PIN 14

class Storage {
private:
    static File activeFile;
    static String activeFileName;
    static bool sdReady;
    // Mutex protecting all SD SPI operations — shared by TaskComm and TaskDAQ
    static SemaphoreHandle_t sdMutex;

    // Acquire the SD mutex before any SD operation (100 ms timeout)
    static bool acquireMutex();
    static void releaseMutex();

public:
    static void init();
    static bool isReady();

    // File lifecycle controls
    static bool createNewFile(const char* techniqueName);
    static void writeCSVHeader(const char* columns);
    static void logDataRow(const String& row);
    static void closeActiveFile();

    // Safety Event Logger → /syslog.txt (append mode, mutex-protected)
    static void logEvent(const char* severity, const String& message);
};

#endif // STORAGE_H

#ifndef STORAGE_H
#define STORAGE_H

#include <Arduino.h>
#include <FS.h>
#include <SD.h>
#include <SPI.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#define SD_CS_PIN 42

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

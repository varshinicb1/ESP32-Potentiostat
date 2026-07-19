#ifndef AD5941_DRIVER_H
#define AD5941_DRIVER_H

#include <Arduino.h>
#include <SPI.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// ===================================================================
// Pin Definitions for AD5941 on ESP32-S3
// ===================================================================
#define AD5941_SCK      12
#define AD5941_MOSI     11
#define AD5941_MISO     13
#define AD5941_CS       10
#define AD5941_RESET    9
#define AD5941_INT      14
#define AD5941_RELAY    8   // Physical Isolation Relay (GPIO 8)

class AD5941_Driver {
private:
    // SPIClass is allocated once in initSPI() and never recreated.
    // A guard prevents re-allocation on repeated calls (e.g., POST retries).
    static SPIClass* spi;
    static bool spiInitialized;
    static volatile bool interruptOccurred;

    // Abort flag: set by main .ino via setAbortFlag(), polled by measurement loops
    static volatile bool abortPending;

    static void IRAM_ATTR isrHandler();

public:
    static void initSPI();
    static void initMCU();
    static void resetChip();

    // Safety & Certification routines
    static void setRelay(bool connected);
    static bool performPOST(String& errorMsg);
    static void enterSafeState();
    static float readInternalTemperature();
    static float readInternalVref();

    // SPI Transfer functions compatible with AD5940/AD5941 SDK
    static void writeBytes(uint8_t* pTxBuffer, uint32_t size);
    static void readBytes(uint8_t* pRxBuffer, uint32_t size);
    static void writeReadBytes(uint8_t* pTxBuffer, uint8_t* pRxBuffer, uint32_t size);

    // Pin control functions
    static void setCS(bool high);
    static void setReset(bool high);
    static uint32_t getMCUTick();
    static void delayMs(uint32_t ms);

    // Interrupt functions
    static bool checkInterrupt();
    static void clearInterrupt();

    // Abort flag — allows measurement loops to check without calling back to main
    static void setAbortFlag(bool val);
    static bool isAbortPending();
};

#endif // AD5941_DRIVER_H

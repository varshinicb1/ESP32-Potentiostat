#ifndef SHARED_SPI_BUS_H
#define SHARED_SPI_BUS_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// ===================================================================
// SharedSPIBus
//
// The AD5941, the TFT display + XPT2046 touch controller, and the microSD
// card all physically share ONE SPI bus (GPIO11/12/13 — MOSI/SCLK/MISO),
// differentiated only by separate chip-select lines. This was discovered by
// cross-checking the actual schematic/netlist (AnalyteX.kicad_pcb +
// production/netlist.ipc) against the ESP32-S3-WROOM-1 datasheet pin table
// — see the pin-definition comments in AD5941_Driver.h and Display_LVGL.cpp
// for the full derivation.
//
// AD5941_Driver runs exclusively on Core 1 (TaskDAQ); the display/touch
// (TaskUI) and SD card (Storage, called from both TaskComm and TaskDAQ) run
// on Core 0 and various tasks. Without arbitration, two cores can drive the
// same physical MOSI/SCLK/MISO wires concurrently — not just a logical race,
// a genuine electrical bus contention that can corrupt whichever transaction
// loses. This mutex is the arbitration point every driver that touches the
// shared bus must take before starting a SPI transaction and release
// immediately after.
//
// AD5941 transactions must NOT silently skip on contention (a dropped
// register read/write corrupts a measurement in progress) — they block with
// a long timeout. Display/LVGL rendering CAN tolerate a bounded wait and
// skip-if-busy (dropping one UI frame is harmless) — see the timeout
// conventions used at each call site.
// ===================================================================
class SharedSPIBus {
public:
    static void init() {
        if (mutex == nullptr) {
            mutex = xSemaphoreCreateMutex();
        }
    }

    // Blocking acquire — use for AD5941 register transactions, where a
    // skipped/corrupted transfer would corrupt an in-progress measurement.
    // Returns true once acquired (should essentially always succeed; a
    // false return after this long a wait indicates the bus is stuck).
    static bool lockBlocking() {
        if (mutex == nullptr) return true; // not yet initialized (e.g. very early boot) — nothing to arbitrate against yet
        return xSemaphoreTake(mutex, pdMS_TO_TICKS(2000)) == pdTRUE;
    }

    // Bounded acquire — use for non-critical bus users (LVGL rendering, SD
    // logging) where skipping this cycle on contention is acceptable.
    static bool lockBounded(uint32_t timeoutMs) {
        if (mutex == nullptr) return true;
        return xSemaphoreTake(mutex, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
    }

    static void unlock() {
        if (mutex != nullptr) xSemaphoreGive(mutex);
    }

private:
    static SemaphoreHandle_t mutex;
};

#endif // SHARED_SPI_BUS_H

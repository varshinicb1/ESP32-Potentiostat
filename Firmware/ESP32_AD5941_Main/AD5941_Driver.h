#ifndef AD5941_DRIVER_H
#define AD5941_DRIVER_H

// ===================================================================
// AFE PLATFORM INITIALIZATION — history and current state
//
// This firmware originally never called AD5940_SWMatrixCfgS(), never
// configured the ADC input mux, never called AD5940_REFCfgS(), and never
// configured the LP-loop or HS-loop anywhere — verified against the official
// ADI ad5940lib/ad5940-examples source and against this board's own
// schematic/netlist. EIS_Method.cpp's own comments admitted this, referring
// to a "platform-level setup (ad5941_FreiStat_setup.cpp or equivalent)" that
// does not exist anywhere in this repository.
//
// STATUS: now implemented and cross-checked against three sources — the
// AD5940/AD5941 datasheet (Rev. G), the official ad5940lib source
// (ad5940.c, not just the header), and this board's actual schematic
// (AnalyteX/AnalyteX.kicad_pcb + production/netlist.ipc, AD5941BCPZ = U1):
//
//   AD5941_Driver::configureAFE() — call once at boot, after initMCU()/
//   resetChip(). Sets up AD5940_REFCfgS (reference buffers for both loops)
//   and AD5940_LPLoopCfgS (LPDAC+LPTIA for CV/CA/SWV), plus the LP-loop's
//   default ADC mux (ADCMUXP_AIN4/ADCMUXN_VZERO0 — the exact mux selection
//   the datasheet recommends for the LPTIA output).
//
//   EIS_Method::configureHSTIA()/applyDCBias()/teardownHSLoop() — full
//   HS-loop bring-up/teardown per EIS sweep (HSTIA/HSDAC power rails via
//   AFECON[11] etc., HSTIACfg_Type fields, HSTIA-side ADC mux, and DC bias
//   via the LPDAC's Vbias/Vzero outputs + LPDACSW_VZERO2HSTIA). Field values
//   taken directly from ADI's Impedance.c, not independently derived.
//   LP-loop and HS-loop share the single ADC mux, so EIS's teardown restores
//   the LP-loop's mux selection afterward — this is why HS-loop setup lives
//   per-sweep in EIS_Method rather than once in configureAFE().
//
// Electrode pin mapping (verified against the schematic, not assumed from
// ADI's eval board):
//   U1 pin 47 (CE0) -- net "CE" -- J1 pin 1  (counter electrode)
//   U1 pin 45 (SE0) -- net "WE" -- J1 pin 2  (working electrode, sense)
//   U1 pin 48 (RE0) -- net "RE" -- J1 pin 3  (reference electrode)
//   U1 pin 32/33 (RCAL0/RCAL1) -- internal only, no board dependency
//   U1 pin 46 (DE0) -- not connected to anything on this board, and per the
//     datasheet (p.50) that's correct: DE0 is only needed for an EXTERNAL
//     HSTIA gain resistor, which this design doesn't use (internal RTIA).
//
// Switch-matrix roles used in EIS_Method.cpp's EIS_SWITCH_CELL_* (datasheet
// p.74): D -> CE0 (excitation output, confirmed), P -> RE0 (datasheet: "for
// most applications, this pin is RE0" — corrected from ADI's eval-board
// example, which uses CE0), T -> SE0 (HSTIA current sense, confirmed).
//
// STILL UNVERIFIED — genuinely open, not just caution-flagged:
//   - The N-switch role for a standard 3-electrode HS-loop setup. No
//     "for most applications" default is given for N in the datasheet text
//     found so far, and no worked 3-electrode HS-loop example was found
//     (only the LP-loop has a full recommended-settings table, Table 21).
//   - None of this has been tested against real hardware. It's grounded in
//     three independent sources (datasheet + SDK source + this board's own
//     schematic) rather than assumption, but bring-up on the actual device
//     is the only way to confirm it end-to-end.
// ===================================================================

#include <Arduino.h>
#include <SPI.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// ===================================================================
// Pin Definitions for AD5941 on ESP32-S3
//
// CORRECTED against the real schematic/netlist (AnalyteX/AnalyteX.kicad_pcb +
// production/netlist.ipc, cross-checked against the official Espressif
// ESP32-S3-WROOM-1 datasheet pin table). Previous values (CS=10, RESET=9,
// INT=14) did not match the board — verified via net names shared between
// ESP33 (the ESP32-S3-WROOM-1, module pins) and U1 (the AD5941):
//   net "CS.AFE"  -> ESP33 module pin 8  -> GPIO15
//   net "AFE.RST" -> ESP33 module pin 18 -> GPIO10
//   net "GPIO.0"  -> ESP33 module pin 4  -> GPIO4  (U1 pin 19 = AD5941's own
//                    GP0 pin, the datasheet's standard interrupt-out pin)
//   net "MOSI"    -> ESP33 module pin 19 -> GPIO11 (unchanged, was correct)
//   net "SCLK"    -> ESP33 module pin 20 -> GPIO12 (unchanged, was correct)
//   net "MISO"    -> ESP33 module pin 21 -> GPIO13 (unchanged, was correct)
//
// IMPORTANT — shared bus: MOSI/SCLK/MISO (GPIO11/12/13) are NOT exclusive to
// the AD5941. The same three nets also run to the TFT display, the XPT2046
// touch controller, and the microSD card (see Display_LVGL.h and Storage.h)
// — differentiated only by separate chip-select lines. TaskDAQ's own comment
// ("SPI is used exclusively here at runtime") was no longer accurate once
// this was known: Display/touch/SD all run on Core 0 (TaskUI) while the
// AD5941 driver runs on Core 1 (TaskDAQ), so two cores can drive the SAME
// physical SPI wires concurrently.
//
// RESOLVED: see SharedSPIBus.h. AD5941_Driver::setCS() acquires/releases the
// shared-bus lock on CS assert/deassert (covering every AD5941 register
// access transparently, since the SDK's own CsClr/transfer/CsSet pattern
// goes through it), Display_LVGL::handleLVGL() takes a bounded/skip-if-busy
// lock around lv_timer_handler() (the actual panel-flush/touch-poll call),
// and Storage::acquireMutex()/releaseMutex() take the same lock alongside
// sdMutex. Not yet verified on real hardware.
//
// IMPORTANT — no physical relay exists on this board. There is no relay
// component, footprint, or net anywhere in the schematic/BOM/netlist. GPIO8
// below is toggled by firmware (setRelay(), enterSafeState(), POST checks)
// but isn't wired to anything on this revision — the "Physical Isolation
// Relay" safety claims in this firmware's own header comments and the
// project README do not reflect real hardware on this board. Either this
// needs to be added in a future PCB revision, or the safety-relevant claims
// need to be corrected to not overstate what this hardware actually does.
// ===================================================================
#define AD5941_SCK      12
#define AD5941_MOSI     11
#define AD5941_MISO     13
#define AD5941_CS       15
#define AD5941_RESET    10
#define AD5941_INT      4
#define AD5941_RELAY    8   // NOT WIRED ON THIS BOARD — see note above. Toggling this GPIO is a no-op.

// ===================================================================
// Calibration Constants
// ===================================================================
// Precision RCAL resistor value on RCAL0/RCAL1 pins.
// MUST match the actual component on your PCB (e.g., 10 kΩ 0.1% 25 ppm/°C).
#define RCAL_OHM        10000.0f

// AD5941 LPDAC reference: datasheet specifies 1.82 V (calibrated value).
// Used by voltage conversion routines. Adjust if your board trims differently.
#define AD5941_DAC_REF_V  1.82f

// Recalibration trigger: if chip temperature changed by more than this,
// a new calibration cycle is needed (IEC 61010-1 §5.4.3 stability requirement).
#define CAL_TEMP_HYSTERESIS_C  5.0f

class AD5941_Driver {
private:
    // SPIClass is allocated once in initSPI() and never recreated.
    // A guard prevents re-allocation on repeated calls (e.g., POST retries).
    static SPIClass* spi;
    static bool spiInitialized;
    static volatile bool interruptOccurred;

    // Abort flag: set by main .ino via setAbortFlag(), polled by measurement loops
    static volatile bool abortPending;

    // Calibration results (stored across measurement calls)
    static float calGain;        // Magnitude correction factor (unitless, ideal = 1.0)
    static float calPhaseRad;    // Phase offset correction (radians, ideal = 0.0)
    static float calTempC;       // Temperature at which calibration was performed
    static bool  calValid;       // true once performCalibration() succeeds

    static void IRAM_ATTR isrHandler();

public:
    static void initSPI();
    static void initMCU();
    static void resetChip();

    // One-time AFE bring-up: reference buffers + LP-loop (LPDAC/LPTIA, used
    // by CV/CA/SWV) + its default ADC mux. Call once after initMCU()/
    // resetChip(), before any measurement. EIS_Method owns its own HS-loop
    // bring-up/teardown per sweep (see EIS_Method::configureHSTIA()/
    // teardownHSLoop()) since HS-loop and LP-loop share the ADC mux and
    // can't both be routed to it at once.
    static void configureAFE();

    // Safety & Certification routines
    static void setRelay(bool connected);
    static bool performPOST(String& errorMsg);
    static void enterSafeState();
    static float readInternalTemperature();
    static float readInternalVref();

    // ---------------------------------------------------------------
    // Startup Calibration (IEC 61010-1 §5.4 — accuracy maintenance)
    // Must be called after POST passes and before any measurement.
    // Measures the known RCAL resistor to compute gain/phase correction.
    // Results are stored in NVS and loaded on subsequent boots if the
    // temperature delta is within CAL_TEMP_HYSTERESIS_C.
    // ---------------------------------------------------------------
    static bool performCalibration(String& errorMsg);
    static float getCalGain();         // Returns current magnitude correction factor
    static float getCalPhaseRad();     // Returns current phase correction (radians)
    static bool  isCalibrationValid(); // true if performCalibration() succeeded
    static bool  needsRecalibration(); // true if temp drift exceeds hysteresis

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

// ===================================================================
// ESP32-S3 + AD5941 Research-Grade Potentiostat Platform
// VidyuthLabs — Firmware v0.2.0
//
// Compliance Targets:
//   IEC 61010-1   — Safety of electrical equipment for measurement
//   IEC 62304     — Medical device software lifecycle
//   ISO 13485     — Quality management systems
//
// Architecture:
//   Core 0: UI (LVGL) + Communication (BLE/WiFi WebSocket)
//   Core 1: Critical DAQ (Voltammetry, EIS) — highest priority
//
// Safety Features:
//   - Power-On Self-Test (POST) with SPI, Vref, Temp, RAM checks
//   - Safe-State on error/abort: AFE switch matrix opened, waveform generator
//     off, DAC parked at 0 V cell bias (this is the real electrode-isolation
//     mechanism on this board — there is NO physical relay; GPIO8 is an
//     unwired forward-compat line, see AD5941_Driver.h)
//   - Task Watchdog Timer (TWDT) for all critical tasks
//   - Firmware CRC32 integrity verification at boot (NVS reference)
//   - Non-volatile syslog event logging to SD card
//   - FreeRTOS mutex-protected state machine and SD access
//   - FreeRTOS queue-based inter-task communication (no shared globals)
//   - Input parameter validation before any measurement begins
// ===================================================================

#include <Arduino.h>
#include <math.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include "AD5941_Driver.h"
#include "EIS_Method.h"
#include "Voltammetry_Methods.h"
#include "Display_LVGL.h"
#include "Communication.h"
#include "Storage.h"
#include "FirmwareIntegrity.h"

// ===================================================================
// Configuration Constants
// ===================================================================
#define TWDT_TIMEOUT_SEC        10      // Task watchdog timeout (seconds)
#define POST_RETRY_COUNT        3       // POST retries before halt
#define STACK_WARN_THRESHOLD    512     // Min free stack words before warning

// ===================================================================
// FreeRTOS Task Handles
// ===================================================================
TaskHandle_t TaskUIHandle   = NULL;
TaskHandle_t TaskCommHandle = NULL;
TaskHandle_t TaskDAQHandle  = NULL;

// ===================================================================
// Global State Machine — protected by stateMutex
// ===================================================================
enum PotentiostatState {
    STATE_IDLE,
    STATE_RUNNING_CV,
    STATE_RUNNING_CA,
    STATE_RUNNING_SWV,
    STATE_RUNNING_EIS,
    STATE_RUNNING_CAL,     // Operator-triggered RCAL gain/phase calibration
    STATE_ERROR,           // Entered on POST failure or critical fault
    STATE_SAFE_SHUTDOWN    // Entered on emergency stop
};

// stateMutex protects systemState from concurrent read/write between Core 0 and Core 1
static SemaphoreHandle_t stateMutex = nullptr;
static PotentiostatState systemState = STATE_IDLE;

// Abort flag: written by TaskComm (Core 0), read by TaskDAQ (Core 1)
// Using portMUX_TYPE for lightweight spinlock — abort is time-critical
static portMUX_TYPE abortMux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool abortRequested = false;

// POST result flag
static volatile bool postPassed = false;

// Cached diagnostic readings — only refreshed from hardware when the system is
// IDLE, so the DIAG command never races TaskDAQ for the AD5941 SPI bus/IRQ.
static float    lastDiagTempC    = NAN;
static float    lastDiagVrefV    = NAN;
static uint32_t lastDiagUpdateMs = 0;

// ===================================================================
// State machine helpers — always call these, never access systemState directly
// ===================================================================
static PotentiostatState getState() {
    PotentiostatState s;
    if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        s = systemState;
        xSemaphoreGive(stateMutex);
    } else {
        s = STATE_ERROR; // Treat mutex timeout as error
    }
    return s;
}

static void setState(PotentiostatState s) {
    if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        systemState = s;
        xSemaphoreGive(stateMutex);
    }
}

static bool isAbortRequested() {
    bool val;
    portENTER_CRITICAL(&abortMux);
    val = abortRequested;
    portEXIT_CRITICAL(&abortMux);
    return val;
}

static void setAbort(bool val) {
    portENTER_CRITICAL(&abortMux);
    abortRequested = val;
    portEXIT_CRITICAL(&abortMux);

    // IMPORTANT: Voltammetry_Methods and EIS_Method poll AD5941_Driver::isAbortPending(),
    // a *separate* flag from the one above. Both must be kept in sync or ABORT will
    // stop data being forwarded to the client while the hardware scan keeps running
    // underneath (electrodes stay biased at the scan potential) until it finishes on
    // its own.
    AD5941_Driver::setAbortFlag(val);
}

// ===================================================================
// Measurement Parameter Buffers — written by TaskComm, read by TaskDAQ
// Protected by stateMutex (parameter update always accompanies state change)
// ===================================================================
CV_Params   activeCVParams;
CA_Params   activeCAParams;
SWV_Params  activeSWVParams;
float       activeEISStart, activeEISStop, activeEISAmp, activeEISBias;
uint32_t    activeEISSteps;

// ===================================================================
// Input Validation — all measurements validated before starting
// ===================================================================
static bool validateCVParams(const CV_Params& p, String& errMsg) {
    if (p.scanRate <= 0.0f || p.scanRate > 10.0f) {
        errMsg = "scan_rate must be 0 < rate <= 10 V/s"; return false;
    }
    if (p.startVoltage < -2.0f || p.startVoltage > 2.0f) {
        errMsg = "start_voltage out of range [-2.0, 2.0] V"; return false;
    }
    if (p.vertexVoltage1 < -2.0f || p.vertexVoltage1 > 2.0f) {
        errMsg = "vertex_1 out of range [-2.0, 2.0] V"; return false;
    }
    if (p.vertexVoltage2 < -2.0f || p.vertexVoltage2 > 2.0f) {
        errMsg = "vertex_2 out of range [-2.0, 2.0] V"; return false;
    }
    if (p.cycles == 0 || p.cycles > 100) {
        errMsg = "cycles must be 1-100"; return false;
    }
    return true;
}

static bool validateCAParams(const CA_Params& p, String& errMsg) {
    if (p.stepVoltage < -2.0f || p.stepVoltage > 2.0f) {
        errMsg = "step_voltage out of range [-2.0, 2.0] V"; return false;
    }
    if (p.duration <= 0.0f || p.duration > 3600.0f) {
        errMsg = "duration must be 0 < d <= 3600 s"; return false;
    }
    // Floor raised from "any positive value" to 10ms: readCurrentResponse()'s
    // ADC poll alone can take up to 5ms, plus SPI/loop overhead, so intervals
    // much below this aren't actually achievable — a smaller requested value
    // was previously accepted but silently sampled slower than requested.
    if (p.sampleInterval < 0.01f || p.sampleInterval > p.duration) {
        errMsg = "interval must be 0.01 s <= interval <= duration"; return false;
    }
    return true;
}

static bool validateSWVParams(const SWV_Params& p, String& errMsg) {
    if (p.startVoltage < -2.0f || p.startVoltage > 2.0f) {
        errMsg = "start_voltage out of range [-2.0, 2.0] V"; return false;
    }
    if (p.stopVoltage < -2.0f || p.stopVoltage > 2.0f) {
        errMsg = "stop_voltage out of range [-2.0, 2.0] V"; return false;
    }
    if (p.startVoltage == p.stopVoltage) {
        errMsg = "start_voltage and stop_voltage must differ"; return false;
    }
    if (p.stepHeight <= 0.0f || p.stepHeight > 0.1f) {
        errMsg = "step_height must be 0 < h <= 0.1 V"; return false;
    }
    if (p.amplitude <= 0.0f || p.amplitude > 0.5f) {
        errMsg = "amplitude must be 0 < a <= 0.5 V"; return false;
    }
    if (p.frequency <= 0.0f || p.frequency > 1000.0f) {
        errMsg = "frequency must be 0 < f <= 1000 Hz"; return false;
    }
    return true;
}

static bool validateEISParams(float start, float stop, uint32_t steps,
                               float amp, float bias, String& errMsg) {
    if (start <= 0.0f || start > 1e6f) {
        errMsg = "start_freq must be 0 < f <= 1 MHz"; return false;
    }
    if (stop <= 0.0f || stop > 1e6f || stop <= start) {
        errMsg = "stop_freq must be > start_freq and <= 1 MHz"; return false;
    }
    if (steps == 0 || steps > 1000) {
        errMsg = "steps must be 1-1000"; return false;
    }
    if (amp <= 0.0f || amp > 200.0f) {
        errMsg = "amplitude must be 0 < a <= 200 mV"; return false;
    }
    if (bias < -1000.0f || bias > 1000.0f) {
        errMsg = "bias must be -1000 to 1000 mV"; return false;
    }
    return true;
}

// ===================================================================
// Data Callbacks — Called from measurement routines on Core 1
// ===================================================================
void onVoltammetryData(Voltammetry_DataPoint dp) {
    if (isAbortRequested()) return;

    // 1. Log to SD card
    if (Storage::isReady()) {
        String row = String(dp.time, 3) + "," +
                     String(dp.voltage, 4) + "," +
                     String(dp.current, 9) + "," +
                     String(dp.diffCurrent, 9);
        Storage::logDataRow(row);
    }

    // 2. Enqueue data for transmission (thread-safe queue, no BLE call here)
    PotentiostatState currentState = getState();
    StaticJsonDocument<256> doc;
    doc["type"]    = "data";
    doc["voltage"] = dp.voltage;
    doc["current"] = dp.current;
    doc["time"]    = dp.time;
    if (currentState == STATE_RUNNING_SWV) {
        doc["diffCurrent"] = dp.diffCurrent;
    }
    String payload;
    serializeJson(doc, payload);
    Communication::sendDataPoint(payload);

    // 3. Update on-device TFT chart
    if (currentState == STATE_RUNNING_SWV) {
        Display_LVGL::addChartPoint(dp.voltage, dp.diffCurrent * 1e6f);
    } else {
        Display_LVGL::addChartPoint(dp.voltage, dp.current * 1e6f);
    }

    // 4. Feed watchdog
    esp_task_wdt_reset();
}

void onEISData(EIS_DataPoint dp) {
    if (isAbortRequested()) return;

    // 1. Log to SD card
    if (Storage::isReady()) {
        String row = String(dp.frequency, 2) + "," +
                     String(dp.realZ, 2) + "," +
                     String(dp.imagZ, 2) + "," +
                     String(dp.magnitude, 2) + "," +
                     String(dp.phase, 2);
        Storage::logDataRow(row);
    }

    // 2. Enqueue for transmission
    StaticJsonDocument<300> doc;
    doc["type"]      = "eis_data";
    doc["frequency"] = dp.frequency;
    doc["realZ"]     = dp.realZ;
    doc["imagZ"]     = dp.imagZ;
    doc["magnitude"] = dp.magnitude;
    doc["phase"]     = dp.phase;
    String payload;
    serializeJson(doc, payload);
    Communication::sendDataPoint(payload);

    // 3. Update Nyquist plot
    Display_LVGL::addChartPoint(dp.realZ, dp.imagZ);

    // 4. Feed watchdog
    esp_task_wdt_reset();
}

// ===================================================================
// Helper: Send status message — enqueues to tx queue (safe from any task)
// ===================================================================
void sendStatusMessage(const char* status, const char* detail = nullptr) {
    StaticJsonDocument<256> doc;
    doc["status"] = status;
    if (detail) doc["detail"] = detail;
    String payload;
    serializeJson(doc, payload);
    Communication::sendDataPoint(payload);
}

// ===================================================================
// Helper: Execute measurement within the safety envelope
//
// NOTE (finalized for this hardware revision): there is no physical
// isolation relay on this board (see AD5941_Driver.h). Electrode connection
// is via the AD5941's internal AFE loop, configured once in configureAFE()
// and per-sweep for EIS. The setRelay() calls here are forward-compatible
// no-ops for a future relay-equipped revision; they are NOT what actually
// connects or isolates the cell, so the log/claims below don't pretend they
// are. On every exit path the cell is returned to a safe 0 V / no-excitation
// state via enterSafeState() — the real thing this board can do.
// ===================================================================
void executeMeasurementSafe(const char* technique, std::function<void()> measurementFn) {
    Storage::logEvent("INFO", String("Starting ") + technique + " measurement");

    // Forward-compat only (no-op on this board): drive the relay line to
    // "connected" for a future revision that populates the relay.
    AD5941_Driver::setRelay(true);

    // Execute the measurement.
    measurementFn();

    // Return the cell to a safe idle state on EVERY exit path (previously only
    // done on abort — a normal completion used to leave the cell polarized at
    // the last scan potential). enterSafeState() opens the HS switch matrix,
    // stops the waveform generator, parks the DAC at 0 V bias, and drives the
    // (unwired) relay line open.
    AD5941_Driver::enterSafeState();

    if (isAbortRequested()) {
        Storage::logEvent("WARN", String(technique) + " measurement ABORTED by user");
        setAbort(false);
    } else {
        Storage::logEvent("INFO", String(technique) + " measurement completed successfully");
    }
    Storage::logEvent("INFO", "Cell returned to 0 V bias (no galvanic relay on this board)");
}

// ===================================================================
// FreeRTOS Task: Core 0 — LVGL UI Updater
// ===================================================================
void TaskUI(void* pvParameters) {
    esp_task_wdt_add(NULL);
    for (;;) {
        Display_LVGL::handleLVGL();
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(5)); // 5 ms for responsive LVGL touch
    }
}

// ===================================================================
// FreeRTOS Task: Core 0 — BLE + WebSocket Communication Handler
// ===================================================================
void TaskComm(void* pvParameters) {
    esp_task_wdt_add(NULL);

    for (;;) {
        // 1. Service WebSocket + flush outgoing queue (all BLE notify happens here)
        Communication::handleComm();

        // 2. Handle incoming configuration commands
        while (Communication::availableCommand()) {
            String jsonCmd = Communication::getNewCommand();

            StaticJsonDocument<600> doc;
            DeserializationError error = deserializeJson(doc, jsonCmd);
            if (error) {
                Storage::logEvent("WARN", String("JSON parse error: ") + error.c_str());
                continue;
            }

            const char* method = doc["method"];
            if (!method) continue;

            // -- ABORT: always accepted regardless of state --
            if (strcmp(method, "ABORT") == 0) {
                setAbort(true);
                Storage::logEvent("WARN", "ABORT command received from remote client");
                sendStatusMessage("aborting");
            }
            // -- IDENTIFY --
            else if (strcmp(method, "IDENTIFY") == 0) {
                String idPayload = FirmwareIntegrity::getIdentificationJSON();
                Communication::sendDataPoint(idPayload);
            }
            // -- DIAG --
            else if (strcmp(method, "DIAG") == 0) {
                // AD5941_Driver::readInternalTemperature()/readInternalVref() do raw SPI
                // register writes and detach/reattach the AD5941 IRQ. TaskDAQ (Core 1)
                // "owns" the SPI bus/IRQ exclusively while a measurement is running, so
                // calling those from here (Core 0) mid-scan would race it and can corrupt
                // an in-flight ADC/DFT conversion. Only take a fresh reading when IDLE;
                // otherwise report the last known-good values and mark them stale.
                bool freshReading = (getState() == STATE_IDLE);
                if (freshReading) {
                    lastDiagTempC = AD5941_Driver::readInternalTemperature();
                    lastDiagVrefV = AD5941_Driver::readInternalVref();
                    lastDiagUpdateMs = millis();
                }

                StaticJsonDocument<320> diagDoc;
                diagDoc["type"]          = "diagnostics";
                diagDoc["temp_C"]        = lastDiagTempC;
                diagDoc["vref_V"]        = lastDiagVrefV;
                diagDoc["reading_stale"] = !freshReading;
                diagDoc["reading_age_ms"] = millis() - lastDiagUpdateMs;
                diagDoc["free_heap"]     = ESP.getFreeHeap();
                diagDoc["min_free_heap"] = ESP.getMinFreeHeap();
                diagDoc["uptime_ms"]     = millis();
                diagDoc["stack_daq"]     = FirmwareIntegrity::getTaskStackWatermark(TaskDAQHandle);
                diagDoc["stack_ui"]      = FirmwareIntegrity::getTaskStackWatermark(TaskUIHandle);
                diagDoc["stack_comm"]    = FirmwareIntegrity::getTaskStackWatermark(TaskCommHandle);
                String diagPayload;
                serializeJson(diagDoc, diagPayload);
                Communication::sendDataPoint(diagPayload);
            }
            // -- Measurement start commands: only when IDLE and POST passed --
            else if (getState() == STATE_IDLE && postPassed) {
                String validationError;

                if (strcmp(method, "CV") == 0) {
                    CV_Params p;
                    p.startVoltage   = doc["params"]["start_voltage"] | 0.0f;
                    p.vertexVoltage1 = doc["params"]["vertex_1"]      | 0.8f;
                    p.vertexVoltage2 = doc["params"]["vertex_2"]      | -0.8f;
                    p.scanRate       = doc["params"]["scan_rate"]     | 0.1f;
                    p.cycles         = doc["params"]["cycles"]        | 1;
                    if (!validateCVParams(p, validationError)) {
                        sendStatusMessage("error", validationError.c_str());
                        Storage::logEvent("WARN", "CV param validation failed: " + validationError);
                    } else {
                        if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                            activeCVParams = p;
                            systemState = STATE_RUNNING_CV;
                            xSemaphoreGive(stateMutex);
                        }
                    }
                }
                else if (strcmp(method, "CA") == 0) {
                    CA_Params p;
                    p.stepVoltage    = doc["params"]["step_voltage"] | 0.0f;
                    p.duration       = doc["params"]["duration"]     | 10.0f;
                    p.sampleInterval = doc["params"]["interval"]     | 0.1f;
                    if (!validateCAParams(p, validationError)) {
                        sendStatusMessage("error", validationError.c_str());
                        Storage::logEvent("WARN", "CA param validation failed: " + validationError);
                    } else {
                        if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                            activeCAParams = p;
                            systemState = STATE_RUNNING_CA;
                            xSemaphoreGive(stateMutex);
                        }
                    }
                }
                else if (strcmp(method, "SWV") == 0) {
                    SWV_Params p;
                    p.startVoltage = doc["params"]["start_voltage"] | -0.5f;
                    p.stopVoltage  = doc["params"]["stop_voltage"]  | 0.5f;
                    p.stepHeight   = doc["params"]["step_height"]   | 0.005f;
                    p.amplitude    = doc["params"]["amplitude"]     | 0.025f;
                    p.frequency    = doc["params"]["frequency"]     | 25.0f;
                    if (!validateSWVParams(p, validationError)) {
                        sendStatusMessage("error", validationError.c_str());
                        Storage::logEvent("WARN", "SWV param validation failed: " + validationError);
                    } else {
                        if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                            activeSWVParams = p;
                            systemState = STATE_RUNNING_SWV;
                            xSemaphoreGive(stateMutex);
                        }
                    }
                }
                else if (strcmp(method, "CAL") == 0) {
                    // Operator must have the RCAL reference resistor connected across
                    // WE/RE in place of a real cell before sending this. Relay handling
                    // and safe-state cleanup are identical to a real measurement.
                    if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                        systemState = STATE_RUNNING_CAL;
                        xSemaphoreGive(stateMutex);
                    }
                }
                else if (strcmp(method, "EIS") == 0) {
                    float start = doc["params"]["start_freq"] | 10.0f;
                    float stop  = doc["params"]["stop_freq"]  | 100000.0f;
                    uint32_t steps = doc["params"]["steps"]   | 50;
                    float amp   = doc["params"]["amplitude"]  | 10.0f;
                    float bias  = doc["params"]["bias"]       | 0.0f;
                    if (!validateEISParams(start, stop, steps, amp, bias, validationError)) {
                        sendStatusMessage("error", validationError.c_str());
                        Storage::logEvent("WARN", "EIS param validation failed: " + validationError);
                    } else {
                        if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                            activeEISStart = start;
                            activeEISStop  = stop;
                            activeEISSteps = steps;
                            activeEISAmp   = amp;
                            activeEISBias  = bias;
                            systemState = STATE_RUNNING_EIS;
                            xSemaphoreGive(stateMutex);
                        }
                    }
                }
            }
            else if (getState() == STATE_ERROR) {
                sendStatusMessage("error", "Device in ERROR state. Power cycle required.");
            }
            else if (getState() != STATE_IDLE) {
                sendStatusMessage("busy", "Measurement in progress. Send ABORT first.");
            }
        }

        // 3. Periodic stack watermark monitoring (every 30 s)
        static uint32_t lastStackCheck = 0;
        if (millis() - lastStackCheck > 30000) {
            lastStackCheck = millis();
            uint32_t daqStack  = FirmwareIntegrity::getTaskStackWatermark(TaskDAQHandle);
            uint32_t uiStack   = FirmwareIntegrity::getTaskStackWatermark(TaskUIHandle);
            uint32_t commStack = FirmwareIntegrity::getTaskStackWatermark(TaskCommHandle);
            if (daqStack  < STACK_WARN_THRESHOLD) Storage::logEvent("WARN", "DAQ stack low: "  + String(daqStack)  + " words");
            if (uiStack   < STACK_WARN_THRESHOLD) Storage::logEvent("WARN", "UI stack low: "   + String(uiStack)   + " words");
            if (commStack < STACK_WARN_THRESHOLD) Storage::logEvent("WARN", "Comm stack low: " + String(commStack) + " words");
        }

        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

// ===================================================================
// FreeRTOS Task: Core 1 — High-Priority DAQ Measurement Task
// ===================================================================
void TaskDAQ(void* pvParameters) {
    esp_task_wdt_add(NULL);

    // Initialize AD5941 chip on Core 1 (SPI is used exclusively here at runtime)
    AD5941_Driver::initMCU();
    AD5941_Driver::resetChip();
    AD5941_Driver::configureAFE(); // AFE reference buffers + LP-loop bring-up (see AD5941_Driver.h)
    AD5941_Driver::enterSafeState(); // Start idle: AFE matrix open, WG off, DAC at 0 V bias
    AD5941_Driver::setRelay(false); // Forward-compat no-op (no relay on this board)
    Storage::logEvent("INFO", "AD5941 initialized on Core 1. Cell idle at 0 V bias.");

    for (;;) {
        PotentiostatState currentState = getState();

        if (currentState != STATE_IDLE && currentState != STATE_ERROR
                && currentState != STATE_SAFE_SHUTDOWN) {
            sendStatusMessage("started");
            Display_LVGL::clearChart();

            if (currentState == STATE_RUNNING_CV) {
                Storage::createNewFile("CV");
                Storage::writeCSVHeader("Time(s),Voltage(V),Current(A),DiffCurrent(A)");
                Display_LVGL::showChartScreen("CV");
                executeMeasurementSafe("CV", [&]() {
                    Voltammetry_Methods::runCV(activeCVParams, onVoltammetryData);
                });
            }
            else if (currentState == STATE_RUNNING_CA) {
                Storage::createNewFile("CA");
                Storage::writeCSVHeader("Time(s),Voltage(V),Current(A),DiffCurrent(A)");
                Display_LVGL::showChartScreen("CA");
                executeMeasurementSafe("CA", [&]() {
                    Voltammetry_Methods::runCA(activeCAParams, onVoltammetryData);
                });
            }
            else if (currentState == STATE_RUNNING_SWV) {
                Storage::createNewFile("SWV");
                Storage::writeCSVHeader("Time(s),Voltage(V),Current(A),DiffCurrent(A)");
                Display_LVGL::showChartScreen("SWV");
                executeMeasurementSafe("SWV", [&]() {
                    Voltammetry_Methods::runSWV(activeSWVParams, onVoltammetryData);
                });
            }
            else if (currentState == STATE_RUNNING_EIS) {
                Storage::createNewFile("EIS");
                Storage::writeCSVHeader("Frequency(Hz),RealZ(Ohm),ImagZ(Ohm),Magnitude(Ohm),Phase(deg)");
                Display_LVGL::showChartScreen("EIS");
                executeMeasurementSafe("EIS", [&]() {
                    EIS_Method eis(activeEISStart, activeEISStop, activeEISSteps,
                                   activeEISAmp, activeEISBias);
                    eis.runSweep(onEISData);
                });
            }
            else if (currentState == STATE_RUNNING_CAL) {
                Storage::logEvent("INFO", "Starting RCAL gain/phase calibration");
                String calResult;
                bool calOk = false;
                executeMeasurementSafe("CAL", [&]() {
                    calOk = AD5941_Driver::performCalibration(calResult);
                });
                Storage::logEvent(calOk ? "INFO" : "WARN", String("Calibration: ") + calResult);
                sendStatusMessage(calOk ? "cal_ok" : "cal_error", calResult.c_str());
            }

            Storage::closeActiveFile();
            setState(STATE_IDLE);
            sendStatusMessage("idle");
            Storage::logEvent("INFO", "System returned to IDLE state");
        }

        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

// ===================================================================
// Power-On Self-Test (POST) Sequence
// IEC 61010-1 §14.1, IEC 62304 §5.5.3
// ===================================================================
bool runPowerOnSelfTest() {
    Serial.println("========================================");
    Serial.println(" AnalyteX — POST");
    Serial.println(" Firmware: " FW_VERSION_STRING);
    Serial.println("========================================");

    // --- Step 1: Firmware Integrity (CRC32 vs NVS reference) ---
    Serial.print("[POST] Verifying firmware CRC32... ");
    uint32_t fwCRC;
    bool crcOk = FirmwareIntegrity::verifyFirmwareIntegrity(fwCRC);
    if (crcOk) {
        char crcStr[11];
        snprintf(crcStr, sizeof(crcStr), "0x%08X", fwCRC);
        Serial.println(String("PASS (") + crcStr + ")");
        Storage::logEvent("INFO", String("Firmware CRC32: ") + crcStr);
    } else {
        Serial.println("FAIL — Firmware image corrupted or tampered!");
        Storage::logEvent("CRIT", "Firmware CRC32 verification FAILED");
        return false;
    }

    // --- Step 2: RAM Integrity ---
    Serial.print("[POST] Verifying RAM integrity... ");
    if (FirmwareIntegrity::verifyRAMIntegrity()) {
        Serial.println("PASS");
        Storage::logEvent("INFO", "RAM integrity check passed");
    } else {
        Serial.println("FAIL — RAM corruption detected!");
        Storage::logEvent("CRIT", "RAM integrity check FAILED");
        return false;
    }

    // --- Step 3: AD5941 SPI + Analog Diagnostics ---
    Serial.print("[POST] Initializing AD5941 and running diagnostics... ");
    AD5941_Driver::initMCU();
    AD5941_Driver::resetChip();
    String postMsg;
    bool ad5941Ok = AD5941_Driver::performPOST(postMsg);
    if (ad5941Ok) {
        Serial.println("PASS — " + postMsg);
        Storage::logEvent("INFO", "AD5941 POST: " + postMsg);
    } else {
        Serial.println("FAIL — " + postMsg);
        Storage::logEvent("CRIT", "AD5941 POST FAILED: " + postMsg);
        return false;
    }

    // --- Step 4: Set relay line to safe default ---
    // NOTE: this only drives GPIO8 low; it does NOT verify a relay, because
    // this board has none (see AD5941_Driver.h). Kept as a forward-compat
    // default for a future relay-equipped revision. Not phrased as a PASS/test
    // so the POST log doesn't imply hardware isolation the board can't provide.
    AD5941_Driver::setRelay(false);
    Serial.println("[POST] Relay line GPIO " + String(AD5941_RELAY) + " driven LOW (no relay populated on this board)");
    Storage::logEvent("INFO", "Relay line default set LOW (no relay on this board)");

    // --- Step 5: SD Card ---
    Serial.print("[POST] Checking SD card... ");
    if (Storage::isReady()) {
        Serial.println("PASS — SD card mounted");
        Storage::logEvent("INFO", "SD card mounted successfully");
    } else {
        Serial.println("WARN — SD card not detected (data logging disabled)");
        Storage::logEvent("WARN", "SD card not detected — data logging disabled");
        // Non-critical: continue POST
    }

    // --- Step 6: Heap check ---
    uint32_t freeHeap = ESP.getFreeHeap();
    if (freeHeap < 50000) {
        Serial.println("[POST] WARN — Low free heap: " + String(freeHeap) + " bytes");
        Storage::logEvent("WARN", "Low free heap at POST: " + String(freeHeap) + " bytes");
    }

    // --- Step 7: Device Identification ---
    Serial.println("[POST] Device UID: " + FirmwareIntegrity::getDeviceUID());
    Serial.println("[POST] Partition:  " + FirmwareIntegrity::getPartitionLabel());
    Serial.println("[POST] Free Heap:  " + String(freeHeap) + " bytes");

    Storage::logEvent("INFO", "POST completed — all critical checks PASSED");
    Serial.println("========================================");
    Serial.println(" POST RESULT: ALL CHECKS PASSED");
    Serial.println("========================================");
    return true;
}

// ===================================================================
// setup() — System Initialization
// ===================================================================
void setup() {
    Serial.begin(115200);
    delay(500);

    // --- Create stateMutex before any task uses it ---
    stateMutex = xSemaphoreCreateMutex();
    configASSERT(stateMutex != nullptr);

    // --- Initialize Storage first (needed for syslog) ---
    Storage::init();
    Storage::logEvent("INFO", "===== SYSTEM BOOT =====");
    Storage::logEvent("INFO", "Firmware: " FW_VERSION_STRING);

    // --- Run Power-On Self-Test ---
    bool postResult = false;
    for (int attempt = 1; attempt <= POST_RETRY_COUNT; attempt++) {
        Serial.println("[BOOT] POST attempt " + String(attempt) + " of " + String(POST_RETRY_COUNT));
        postResult = runPowerOnSelfTest();
        if (postResult) break;
        Storage::logEvent("WARN", "POST attempt " + String(attempt) + " failed, retrying...");
        delay(1000);
    }

    if (!postResult) {
        Serial.println("!!! CRITICAL: POST FAILED AFTER ALL RETRIES !!!");
        Serial.println("!!! SYSTEM HALTED IN SAFE STATE !!!");
        Storage::logEvent("CRIT", "POST FAILED after " + String(POST_RETRY_COUNT) + " attempts. SYSTEM HALTED.");
        AD5941_Driver::enterSafeState();
        setState(STATE_ERROR);
        postPassed = false;
        Display_LVGL::init();
        Display_LVGL::showErrorScreen("POST FAILED — Check connections and power cycle");
        while (true) { delay(1000); }
    }

    postPassed = true;

    // Seed the DIAG cache with real readings taken during POST (system is
    // single-threaded here, so there's no race against TaskDAQ yet).
    lastDiagTempC    = AD5941_Driver::readInternalTemperature();
    lastDiagVrefV    = AD5941_Driver::readInternalVref();
    lastDiagUpdateMs = millis();

    // --- Initialize Task Watchdog Timer ---
    esp_task_wdt_init(TWDT_TIMEOUT_SEC, true);
    Storage::logEvent("INFO", "TWDT initialized: " + String(TWDT_TIMEOUT_SEC) + "s timeout");

    // --- Initialize Display ---
    Display_LVGL::init();
    Storage::logEvent("INFO", "Display initialized");

    // --- Initialize Communication ---
    Communication::init();
    Storage::logEvent("INFO", "Communication stack initialized (BLE + WiFi)");

    // --- Create FreeRTOS Tasks ---
    xTaskCreatePinnedToCore(TaskUI,   "UI_Display",       8192,  NULL, 5,  &TaskUIHandle,   0);
    xTaskCreatePinnedToCore(TaskComm, "Comm_Link",        10240, NULL, 10, &TaskCommHandle, 0);
    xTaskCreatePinnedToCore(TaskDAQ,  "DAQ_Potentiostat", 16384, NULL, 24, &TaskDAQHandle,  1);

    Storage::logEvent("INFO", "FreeRTOS tasks created. System operational.");
    Serial.println("[BOOT] System ready. Waiting for commands...");
}

// ===================================================================
// loop() — Empty: FreeRTOS handles all execution
// ===================================================================
void loop() {
    // All work is done in FreeRTOS tasks pinned to specific cores.
    // The Arduino loop task is intentionally idle and not TWDT-registered.
    vTaskDelay(portMAX_DELAY);
}

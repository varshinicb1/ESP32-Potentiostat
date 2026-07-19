#include "Voltammetry_Methods.h"
#include <math.h>

extern "C" {
#include <ad5940.h>
}

// ===================================================================
// Constants
// ===================================================================
#define RTIA_OHM        10000.0f  // 10 kΩ TIA feedback resistor
#define VREF_TIA_MV     1100.0f   // 1.1 V TIA virtual ground reference (mV)
#define ADC_VREF_V      1.82f     // AD5941 ADC reference voltage
#define ADC_FULLSCALE   32767.0f  // 16-bit signed ADC full scale
#define DAC_MIN_MV      200.0f    // LPDAC minimum output (mV)
#define DAC_MAX_MV      2400.0f   // LPDAC maximum output (mV)
#define DAC_SPAN_MV     (DAC_MAX_MV - DAC_MIN_MV) // 2200 mV
#define DAC_12BIT_MAX   4095.0f
#define CV_STEP_MV      2.0f      // CV voltage step size (mV)

// ===================================================================
// setVoltageBias()
// Maps a desired cell voltage to the LPDAC code and applies it.
// Clamps to the valid DAC output range [200 mV, 2400 mV].
// ===================================================================
void Voltammetry_Methods::setVoltageBias(float voltageV) {
    float voltageMV = voltageV * 1000.0f;

    // Virtual ground: VREF_TIA_MV is mid-scale
    // A positive cell voltage (vs. RE) requires DAC below Vref
    float targetDACMV = VREF_TIA_MV - voltageMV;

    // Clamp to valid LPDAC output range
    if (targetDACMV < DAC_MIN_MV) targetDACMV = DAC_MIN_MV;
    if (targetDACMV > DAC_MAX_MV) targetDACMV = DAC_MAX_MV;

    uint32_t dacCode = (uint32_t)(((targetDACMV - DAC_MIN_MV) / DAC_SPAN_MV) * DAC_12BIT_MAX);
    if (dacCode > 4095) dacCode = 4095; // Belt-and-suspenders clamp

    uint32_t val = AD5940_ReadReg(REG_AFE_LPDACDAT0);
    uint8_t data6bit = (val >> 12) & 0x3F;
    AD5940_LPDAC0WriteS(dacCode, data6bit);
}

// ===================================================================
// readCurrentResponse()
// Triggers an ADC conversion and converts the result to current.
// Returns NAN if the conversion times out (hardware/interrupt fault).
// ===================================================================
float Voltammetry_Methods::readCurrentResponse() {
    AD5940_AFECtrlS(AFECTRL_ADCCNV, bTRUE); // Start ADC conversion

    // Poll for conversion-complete interrupt with a bounded timeout
    const uint32_t TIMEOUT_US = 5000; // 5 ms max conversion time
    uint32_t elapsed = 0;
    while (!AD5941_Driver::checkInterrupt() && elapsed < TIMEOUT_US) {
        delayMicroseconds(10);
        elapsed += 10;
    }

    AD5940_AFECtrlS(AFECTRL_ADCCNV, bFALSE); // Stop conversion

    if (elapsed >= TIMEOUT_US) {
        // ADC timed out — this is a hardware fault condition
        AD5941_Driver::clearInterrupt();
        return NAN;
    }
    AD5941_Driver::clearInterrupt();

    int32_t adcRaw = AD5940_ReadReg(REG_AFE_ADCDAT); // Correct register name

    // Convert signed 16-bit ADC reading to voltage (V)
    float adcV  = ((float)adcRaw / ADC_FULLSCALE) * ADC_VREF_V;
    float adcMV = adcV * 1000.0f;

    // TIA current: I = (Vref_tia - Vadc) / Rtia
    float currentAmps = (VREF_TIA_MV - adcMV) / (RTIA_OHM * 1000.0f);
    return currentAmps;
}

// ===================================================================
// runCV() — Cyclic Voltammetry
// Three-segment: start → V1 → V2 → start, repeated for N cycles.
// Uses indexed voltage calculation to avoid floating-point drift.
// Checks abort flag inside every sweep segment.
// ===================================================================
void Voltammetry_Methods::runCV(CV_Params params, void (*dataCallback)(Voltammetry_DataPoint)) {
    const float stepSizeV = CV_STEP_MV / 1000.0f; // 0.002 V

    // Delay between steps derived from scan rate (V/s)
    // stepDelayMs = (stepSize / scanRate) * 1000
    // scanRate > 0 is guaranteed by parameter validation
    float stepDelaySec = stepSizeV / params.scanRate;
    uint32_t stepDelayMs = (uint32_t)(stepDelaySec * 1000.0f);
    if (stepDelayMs < 1) stepDelayMs = 1;

    uint32_t startTime = millis();

    for (uint32_t cycle = 0; cycle < params.cycles; cycle++) {

        // -- Segment 1: startVoltage → vertexVoltage1 --
        {
            float spanV = params.vertexVoltage1 - params.startVoltage;
            int32_t nSteps = (int32_t)(fabsf(spanV) / stepSizeV);
            int8_t  dir    = (spanV >= 0) ? 1 : -1;
            for (int32_t i = 0; i <= nSteps; i++) {
                if (AD5941_Driver::isAbortPending()) return; // Allow external abort
                float v = params.startVoltage + dir * i * stepSizeV;
                setVoltageBias(v);
                delay(stepDelayMs);
                float I = readCurrentResponse();
                if (isnan(I)) continue; // Skip bad samples, don't crash
                Voltammetry_DataPoint dp = {v, I, (float)(millis() - startTime) / 1000.0f, 0.0f};
                if (dataCallback) dataCallback(dp);
            }
        }

        // -- Segment 2: vertexVoltage1 → vertexVoltage2 --
        {
            float spanV = params.vertexVoltage2 - params.vertexVoltage1;
            int32_t nSteps = (int32_t)(fabsf(spanV) / stepSizeV);
            int8_t  dir    = (spanV >= 0) ? 1 : -1;
            for (int32_t i = 0; i <= nSteps; i++) {
                if (AD5941_Driver::isAbortPending()) return;
                float v = params.vertexVoltage1 + dir * i * stepSizeV;
                setVoltageBias(v);
                delay(stepDelayMs);
                float I = readCurrentResponse();
                if (isnan(I)) continue;
                Voltammetry_DataPoint dp = {v, I, (float)(millis() - startTime) / 1000.0f, 0.0f};
                if (dataCallback) dataCallback(dp);
            }
        }

        // -- Segment 3: vertexVoltage2 → startVoltage --
        {
            float spanV = params.startVoltage - params.vertexVoltage2;
            int32_t nSteps = (int32_t)(fabsf(spanV) / stepSizeV);
            int8_t  dir    = (spanV >= 0) ? 1 : -1;
            for (int32_t i = 0; i <= nSteps; i++) {
                if (AD5941_Driver::isAbortPending()) return;
                float v = params.vertexVoltage2 + dir * i * stepSizeV;
                setVoltageBias(v);
                delay(stepDelayMs);
                float I = readCurrentResponse();
                if (isnan(I)) continue;
                Voltammetry_DataPoint dp = {v, I, (float)(millis() - startTime) / 1000.0f, 0.0f};
                if (dataCallback) dataCallback(dp);
            }
        }
    }
}

// ===================================================================
// runCA() — Chronoamperometry
// Applies a fixed voltage and samples current over time.
// ===================================================================
void Voltammetry_Methods::runCA(CA_Params params, void (*dataCallback)(Voltammetry_DataPoint)) {
    setVoltageBias(params.stepVoltage);

    uint32_t startTime   = millis();
    uint32_t durationMs  = (uint32_t)(params.duration * 1000.0f);
    uint32_t intervalMs  = (uint32_t)(params.sampleInterval * 1000.0f);
    if (intervalMs < 1) intervalMs = 1;

    while ((millis() - startTime) < durationMs) {
        if (AD5941_Driver::isAbortPending()) return;
        delay(intervalMs);

        float I = readCurrentResponse();
        if (isnan(I)) continue;

        float elapsed = (float)(millis() - startTime) / 1000.0f;
        Voltammetry_DataPoint dp = {params.stepVoltage, I, elapsed, 0.0f};
        if (dataCallback) dataCallback(dp);
    }
}

// ===================================================================
// runSWV() — Square Wave Voltammetry
// Staircase with alternating forward/backward pulses.
// Net differential current = I_forward - I_backward.
// ===================================================================
void Voltammetry_Methods::runSWV(SWV_Params params, void (*dataCallback)(Voltammetry_DataPoint)) {
    float pulseHalfPeriodMs = (1.0f / params.frequency) * 500.0f;
    uint32_t halfPeriodMs = (uint32_t)pulseHalfPeriodMs;
    if (halfPeriodMs < 1) halfPeriodMs = 1;

    uint32_t startTime = millis();
    bool ascending = (params.stopVoltage > params.startVoltage);

    float spanV  = fabsf(params.stopVoltage - params.startVoltage);
    int32_t nSteps = (int32_t)(spanV / params.stepHeight);
    int8_t  dir    = ascending ? 1 : -1;

    for (int32_t i = 0; i <= nSteps; i++) {
        if (AD5941_Driver::isAbortPending()) return;

        // Current staircase voltage (indexed to avoid cumulative drift)
        float vStair = params.startVoltage + dir * i * params.stepHeight;

        // Forward pulse
        setVoltageBias(vStair + params.amplitude);
        delay(halfPeriodMs);
        float iForward = readCurrentResponse();

        // Backward pulse
        setVoltageBias(vStair - params.amplitude);
        delay(halfPeriodMs);
        float iBackward = readCurrentResponse();

        // Skip if either sample is bad
        if (isnan(iForward) || isnan(iBackward)) continue;

        float iNet = iForward - iBackward;
        float elapsed = (float)(millis() - startTime) / 1000.0f;
        Voltammetry_DataPoint dp = {vStair, iForward, elapsed, iNet};
        if (dataCallback) dataCallback(dp);
    }
}

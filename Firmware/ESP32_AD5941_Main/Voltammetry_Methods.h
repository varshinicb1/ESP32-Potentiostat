#ifndef VOLTAMMETRY_METHODS_H
#define VOLTAMMETRY_METHODS_H

#include <Arduino.h>
#include "AD5941_Driver.h"

// Struct for voltammetry data point (CV, CA, SWV)
struct Voltammetry_DataPoint {
    float voltage;      // V vs Reference (V)
    float current;      // Measured current (Amps)
    float time;         // Relative elapsed time (seconds)
    float diffCurrent;  // For SWV: Net differential current (Amps)
};

// Parameter configs
struct CV_Params {
    float startVoltage;   // V
    float vertexVoltage1; // V
    float vertexVoltage2; // V
    float scanRate;       // V/s
    uint32_t cycles;
};

struct CA_Params {
    float stepVoltage;    // V
    float duration;       // seconds
    float sampleInterval; // seconds
};

struct SWV_Params {
    float startVoltage;   // V
    float stopVoltage;    // V
    float stepHeight;     // V
    float amplitude;      // V
    float frequency;      // Hz
};

class Voltammetry_Methods {
private:
    static float readCurrentResponse();

public:
    // Maps a desired cell voltage (vs. reference) to the LPDAC code and applies it.
    // Public because EIS_Method also needs it to apply the DC bias operating point
    // before layering the HSDAC/WG sine excitation on top (same LPDAC bias path
    // used by CV/CA/SWV).
    static void setVoltageBias(float voltageRef);

    static void runCV(CV_Params params, void (*dataCallback)(Voltammetry_DataPoint));
    static void runCA(CA_Params params, void (*dataCallback)(Voltammetry_DataPoint));
    static void runSWV(SWV_Params params, void (*dataCallback)(Voltammetry_DataPoint));
};

#endif // VOLTAMMETRY_METHODS_H

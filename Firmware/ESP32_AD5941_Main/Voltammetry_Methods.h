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
    // Helper DAC configuration for AD5941 bias
    static void setVoltageBias(float voltageRef);
    static float readCurrentResponse();

public:
    static void runCV(CV_Params params, void (*dataCallback)(Voltammetry_DataPoint));
    static void runCA(CA_Params params, void (*dataCallback)(Voltammetry_DataPoint));
    static void runSWV(SWV_Params params, void (*dataCallback)(Voltammetry_DataPoint));
};

#endif // VOLTAMMETRY_METHODS_H

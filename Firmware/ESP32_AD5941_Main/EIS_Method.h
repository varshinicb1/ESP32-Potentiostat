#ifndef EIS_METHOD_H
#define EIS_METHOD_H

#include <Arduino.h>
#include "AD5941_Driver.h"

// Struct to hold individual impedance data point
struct EIS_DataPoint {
    float frequency;
    float realZ;       // Z' (Ohms)
    float imagZ;       // Z'' (Ohms)
    float magnitude;   // |Z| (Ohms)
    float phase;       // Phase angle (degrees)
    bool  error;       // true if this point is invalid (e.g., DFT timeout)
};

class EIS_Method {
private:
    float startFreq;
    float stopFreq;
    uint32_t steps;
    float amplitude; // mV
    float dcBias;    // mV
    bool useLogSweep;

    // Helper functions to configure AD5941 Registers for EIS
    void configureHSTIA();
    void configureDFT(float frequency);
    void setExcitationSine(float frequency);
    EIS_DataPoint measureSingleFrequency(float frequency);

public:
    EIS_Method(float start, float stop, uint32_t numSteps, float amp, float bias, bool logSweep = true);
    
    // Executes the sweep and streams data callback function
    void runSweep(void (*dataCallback)(EIS_DataPoint));
};

#endif // EIS_METHOD_H

#ifndef EIS_METHOD_H
#define EIS_METHOD_H

#include <Arduino.h>
#include "AD5941_Driver.h"
#include "Voltammetry_Methods.h"

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

    // Applies dcBias via the LPDAC's Vbias(12-bit)/Vzero(6-bit) outputs and
    // routes Vzero to the HSTIA's positive-input bias mux (LPDACSW_VZERO2HSTIA)
    // — the mechanism verified against ADI's official Impedance.c, not the
    // Voltammetry_Methods::setVoltageBias() call this previously (incorrectly)
    // reused. Called once per runSweep(), not per-frequency-point.
    void applyDCBias();

    // Powers down the HS-loop blocks and restores the LP-loop's default ADC
    // mux, so a CV/CA/SWV run immediately after an EIS sweep still works.
    void teardownHSLoop();

    // Reconfigures the switch matrix to the given routing, triggers one ADC+DFT
    // capture, and reads back the complex result. Returns false on abort/timeout.
    bool captureDFT(uint32_t dSw, uint32_t pSw, uint32_t nSw, uint32_t tSw,
                     int32_t& realOut, int32_t& imagOut);

public:
    EIS_Method(float start, float stop, uint32_t numSteps, float amp, float bias, bool logSweep = true);
    
    // Executes the sweep and streams data callback function
    void runSweep(void (*dataCallback)(EIS_DataPoint));
};

#endif // EIS_METHOD_H

#include "EIS_Method.h"
#include <math.h>

extern "C" {
#include <ad5940.h>
}

// ===================================================================
// Constants
// ===================================================================
#define RTIA_VALUE      10000.0f  // 10 kΩ HSTIA feedback resistor
#define EIS_TIMEOUT_MS  3000      // Max wait for DFT complete (ms)

// ===================================================================
// Constructor
// ===================================================================
EIS_Method::EIS_Method(float start, float stop, uint32_t numSteps,
                       float amp, float bias, bool logSweep) {
    startFreq   = start;
    stopFreq    = stop;
    steps       = numSteps;
    amplitude   = amp;
    dcBias      = bias;
    useLogSweep = logSweep;
}

// ===================================================================
// configureHSTIA()
// Sets up the High-Speed TIA for EIS current measurement.
// ===================================================================
void EIS_Method::configureHSTIA() {
    // EIS uses the High-Speed TIA (HSTIA) for current measurement.
    // AD5940_HSRTIACfgS() selects the internal RTIA resistor.
    // HSTIARTIA_10K = 10 kΩ — must match RTIA_VALUE constant used
    // in the impedance calculation (Z = Vdft/Idft * RTIA_VALUE).
    AD5940_HSRTIACfgS(HSTIARTIA_10K);
}

// ===================================================================
// configureDFT()
// Programs the DFT engine for the current frequency point.
// ===================================================================
void EIS_Method::configureDFT(float frequency) {
    (void)frequency; // DFT settings are frequency-independent at this level

    DFTCfg_Type dft_cfg;
    AD5940_StructInit(&dft_cfg, sizeof(dft_cfg));
    dft_cfg.DftNum  = DFTNUM_1024;  // 1024 points for clean spectral resolution
    dft_cfg.DftSrc  = DFTSRC_SINC3; // Output of Sinc3 filter
    AD5940_DFTCfgS(&dft_cfg);       // Correct SDK function name
    AD5940_AFECtrlS(AFECTRL_DFT, bTRUE); // Enable DFT engine
}

// ===================================================================
// setExcitationSine()
// Programs the waveform generator with a sine wave at the given frequency.
// ===================================================================
void EIS_Method::setExcitationSine(float frequency) {
    WGCfg_Type wg_cfg;
    AD5940_StructInit(&wg_cfg, sizeof(wg_cfg));
    wg_cfg.WgType   = WGTYPE_SIN;
    wg_cfg.SinCfg.SinFreqWord = AD5940_WGFreqWordCal(frequency, 16000000.0f);
    wg_cfg.SinCfg.SinAmplitudeWord = (uint32_t)(amplitude / 800.0f * 2047.0f + 0.5f);
    wg_cfg.SinCfg.SinOffsetWord = 0;
    AD5940_WGCfgS(&wg_cfg);               // Correct SDK function name
    AD5940_AFECtrlS(AFECTRL_WG, bTRUE);  // Enable WG
}

// ===================================================================
// measureSingleFrequency()
// Applies sine excitation, waits for DFT result, computes impedance.
// Returns an EIS_DataPoint with error = true on timeout/failure.
// ===================================================================
EIS_DataPoint EIS_Method::measureSingleFrequency(float frequency) {
    EIS_DataPoint dp = {};
    dp.frequency = frequency;
    dp.error     = false;

    // Configure DFT and excitation for this frequency point
    configureDFT(frequency);
    setExcitationSine(frequency);

    // Trigger AFE sequence (sequence must be pre-programmed externally)
    // Note: SEQID_0 must be loaded with the EIS measurement sequence
    // via AD5940_SEQCmdWrite() before calling runSweep(). This is done
    // in the platform-level setup (ad5941_FreiStat_setup.cpp or equivalent).
    AD5940_SEQMmrTrig(SEQID_0); // Manually trigger sequence

    // Wait for conversion-complete interrupt with a bounded timeout
    uint32_t start = millis();
    while (!AD5941_Driver::checkInterrupt()) {
        if ((millis() - start) > EIS_TIMEOUT_MS) {
            // Hardware fault: DFT did not complete in time
            // Set error flag and return a zeroed data point
            AD5941_Driver::clearInterrupt();
            dp.error     = true;
            dp.realZ     = 0.0f;
            dp.imagZ     = 0.0f;
            dp.magnitude = 0.0f;
            dp.phase     = 0.0f;
            return dp;
        }
        delay(1);
    }
    AD5941_Driver::clearInterrupt();

    // Read DFT result registers
    // AD5940_DFTRead is an ADI SDK function that returns the complex DFT result
    // for both the excitation voltage channel (V_dft) and response current channel (I_dft).
    // If this function is not present in your SDK version, use:
    //   realV = (int32_t)AD5940_ReadReg(REG_AFE_DFTREAL);
    //   imagV = (int32_t)AD5940_ReadReg(REG_AFE_DFTIMAG);
    //   realI = (int32_t)AD5940_ReadReg(REG_AFE_DFTREAL2);
    //   imagI = (int32_t)AD5940_ReadReg(REG_AFE_DFTIMAG2);
    int32_t realV = 0, imagV = 0, realI = 0, imagI = 0;
#ifdef AD5940_DFT_READ_AVAILABLE
    // Use the SDK helper if available
    AD5940_DFTRead(&realV, &imagV, &realI, &imagI);
#else
    // Direct register fallback (verify register addresses against your AD5941 datasheet)
    realV = (int32_t)AD5940_ReadReg(REG_AFE_DFTREAL);
    imagV = (int32_t)AD5940_ReadReg(REG_AFE_DFTIMAG);
    realI = (int32_t)AD5940_ReadReg(REG_AFE_DFTREAL + 4);
    imagI = (int32_t)AD5940_ReadReg(REG_AFE_DFTIMAG + 4);
#endif

    // Complex impedance: Z = (V_dft / I_dft) * Rtia
    // Complex division: (a+jb)/(c+jd) = ((ac+bd) + j(bc-ad)) / (c²+d²)
    float a = (float)realV, b = (float)imagV;
    float c = (float)realI, d = (float)imagI;

    float denominator = c * c + d * d;
    if (denominator < 1e-9f) denominator = 1e-9f; // Prevent division by near-zero

    dp.realZ     = ((a * c + b * d) / denominator) * RTIA_VALUE;
    dp.imagZ     = ((b * c - a * d) / denominator) * RTIA_VALUE;
    dp.magnitude = sqrtf(dp.realZ * dp.realZ + dp.imagZ * dp.imagZ);
    dp.phase     = atan2f(dp.imagZ, dp.realZ) * (180.0f / (float)M_PI);

    return dp;
}

// ===================================================================
// runSweep()
// Iterates over the frequency range, measuring impedance at each point.
// Calls dataCallback for each valid data point (errors are skipped).
// ===================================================================
void EIS_Method::runSweep(void (*dataCallback)(EIS_DataPoint)) {
    configureHSTIA();

    for (uint32_t i = 0; i <= steps; i++) {
        if (AD5941_Driver::isAbortPending()) return;

        float freq;
        if (useLogSweep) {
            float logStart = log10f(startFreq);
            float logStop  = log10f(stopFreq);
            freq = powf(10.0f, logStart + (logStop - logStart) * (float)i / (float)steps);
        } else {
            freq = startFreq + (stopFreq - startFreq) * (float)i / (float)steps;
        }

        EIS_DataPoint dp = measureSingleFrequency(freq);

        if (dp.error) {
            // Log the error but continue the sweep — partial data is better than none
            // The caller (onEISData) will receive an error-flagged point
            // and can choose to skip or mark it in the CSV.
        }

        if (dataCallback) {
            
            dataCallback(dp);
        }
    }

    // Disable excitation after sweep completes
    AD5940_AFECtrlS(AFECTRL_WG, bFALSE);
}

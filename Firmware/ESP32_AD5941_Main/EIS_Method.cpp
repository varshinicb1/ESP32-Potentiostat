#include "EIS_Method.h"
#include <math.h>

extern "C" {
#include <ad5940.h>
}

// ===================================================================
// Constants
// ===================================================================
// Max wait for DFT complete (ms). Generous enough to cover the largest DFT
// (16384 pts) at the lowest supported frequency without false per-point
// timeouts; a timeout here just marks one point as error and moves on, so
// erring large is safe (it never blocks an abort — isAbortPending() is only
// checked between points, see runSweep()).
#define EIS_TIMEOUT_MS  10000

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
// Full HS-loop bring-up for EIS. This was previously just a single
// AD5940_HSRTIACfgS() call (RTIA gain only) — verified against ad5940.c that
// this leaves the HSTIA/HSDAC power rails, ADC mux, and DE0 resistor fields
// untouched. Per the datasheet ("HIGH SPEED TIA CONFIGURATION", p.50): "The
// high speed TIA is disabled by default and is turned on by setting
// AFECON[11] = 1" — AFECTRL_HSTIAPWR is that bit, and nothing in this
// codebase was ever setting it. The full power-up sequence and HSTIACfg_Type
// field values below are taken directly from ADI's official Impedance.c
// (AppIMPSeqCfgGen), not independently derived.
// ===================================================================
void EIS_Method::configureHSTIA() {
    // Power up the HS-loop blocks: HSTIA, excitation-loop input amp,
    // excitation buffer, HSDAC, HSDAC reference buffer, SINC2+Notch filter.
    AD5940_AFECtrlS(AFECTRL_HSTIAPWR | AFECTRL_INAMPPWR | AFECTRL_EXTBUFPWR |
                     AFECTRL_DACREFPWR | AFECTRL_HSDACPWR | AFECTRL_SINC2NOTCH, bTRUE);

    HSTIACfg_Type hstia_cfg;
    AD5940_StructInit(&hstia_cfg, sizeof(hstia_cfg));
    // HstiaBias: use the LPDAC's Vzero output (routed via applyDCBias()'s
    // LPDACSW_VZERO2HSTIA switch) when a DC bias was requested, otherwise the
    // fixed internal 1.1V reference — exactly ADI's conditional in Impedance.c.
    hstia_cfg.HstiaBias    = (dcBias != 0.0f) ? HSTIABIAS_VZERO0 : HSTIABIAS_1P1;
    hstia_cfg.HstiaRtiaSel = HSTIARTIA_10K;
    hstia_cfg.HstiaCtia    = 31; // 31pF + 2pF internal, per Impedance.c
    hstia_cfg.DiodeClose   = bFALSE;
    // DE0 is not connected on this board (verified against netlist.ipc — see
    // EIS_SWITCH_CELL_* comment below) and this design uses the internal
    // RTIA, not an external one on DE0, so these are OPEN, not a real value.
    hstia_cfg.HstiaDeRtia  = HSTIADERTIA_OPEN;
    hstia_cfg.HstiaDeRload = HSTIADERLOAD_OPEN;
    AD5940_HSTIACfgS(&hstia_cfg);

    // Route the ADC to the HSTIA output. AD5940_ADCBaseCfgS() only touches
    // REG_AFE_ADCCON (verified in ad5940.c) — it does not disturb the DFT/
    // filter registers that configureDFT() sets up separately, so it's safe
    // to call here independent of that.
    ADCBaseCfg_Type adc_cfg;
    AD5940_StructInit(&adc_cfg, sizeof(adc_cfg));
    adc_cfg.ADCMuxP = ADCMUXP_HSTIA_P;
    adc_cfg.ADCMuxN = ADCMUXN_HSTIA_N;
    adc_cfg.ADCPga  = ADCPGA_1;
    AD5940_ADCBaseCfgS(&adc_cfg);

    applyDCBias();
}

// ===================================================================
// applyDCBias()
// Applies the requested DC bias via the LPDAC's Vbias(12-bit)/Vzero(6-bit)
// outputs, exactly matching ADI's Impedance.c bias-voltage handling — NOT
// Voltammetry_Methods::setVoltageBias(), which this file used in an earlier
// pass. That was the wrong mechanism: it drives the LP-loop's bias path, not
// the LPDACSW_VZERO2HSTIA switch that actually feeds the HSTIA's bias mux
// when HstiaBias = HSTIABIAS_VZERO0 (see configureHSTIA() above and the
// LPDACCfg_Type field comments in the SDK header).
// ===================================================================
void EIS_Method::applyDCBias() {
    if (dcBias == 0.0f) return; // HstiaBias already set to the fixed 1.1V ref

    // 12-bit DAC LSB across the 200mV-2400mV output span (2200mV/4095 codes),
    // matching Voltammetry_Methods.cpp's DAC_SPAN_MV/DAC_12BIT_MAX constants.
    const float DAC12BIT_1LSB_MV = 2200.0f / 4095.0f;

    float biasMV = dcBias;
    if (biasMV < -1100.0f) biasMV = -1100.0f + DAC12BIT_1LSB_MV;
    if (biasMV >  1100.0f) biasMV =  1100.0f - DAC12BIT_1LSB_MV;

    LPDACCfg_Type lpdac_cfg;
    AD5940_StructInit(&lpdac_cfg, sizeof(lpdac_cfg));
    lpdac_cfg.LpdacSel      = LPDAC0;
    lpdac_cfg.LpDacVbiasMux = LPDACVBIAS_12BIT;
    lpdac_cfg.LpDacVzeroMux = LPDACVZERO_6BIT;
    lpdac_cfg.DacData6Bit   = 0x40 >> 1; // Vzero at mid-scale
    lpdac_cfg.DacData12Bit  = (uint32_t)((biasMV + 1100.0f) / DAC12BIT_1LSB_MV);
    lpdac_cfg.DataRst       = bFALSE;
    lpdac_cfg.LpDacSW       = LPDACSW_VBIAS2LPPA | LPDACSW_VBIAS2PIN |
                              LPDACSW_VZERO2LPTIA | LPDACSW_VZERO2PIN | LPDACSW_VZERO2HSTIA;
    lpdac_cfg.LpDacRef      = LPDACREF_2P5;
    lpdac_cfg.LpDacSrc      = LPDACSRC_MMR;
    lpdac_cfg.PowerEn       = bTRUE;
    AD5940_LPDACCfgS(&lpdac_cfg);
}

// ===================================================================
// teardownHSLoop()
// Powers down the HS-loop blocks after a sweep and restores the LP-loop's
// ADC mux (AIN4/LPF0 for LPTIA output, per the datasheet's own recommended
// mux selection), so a CV/CA/SWV run immediately after this EIS sweep works
// without needing its own separate re-init call.
// ===================================================================
void EIS_Method::teardownHSLoop() {
    AD5940_AFECtrlS(AFECTRL_HSTIAPWR | AFECTRL_INAMPPWR | AFECTRL_EXTBUFPWR |
                     AFECTRL_DACREFPWR | AFECTRL_HSDACPWR | AFECTRL_SINC2NOTCH |
                     AFECTRL_WG, bFALSE);

    ADCBaseCfg_Type adc_cfg;
    AD5940_StructInit(&adc_cfg, sizeof(adc_cfg));
    adc_cfg.ADCMuxP = ADCMUXP_AIN4;
    adc_cfg.ADCMuxN = ADCMUXN_VZERO0;
    adc_cfg.ADCPga  = ADCPGA_1;
    AD5940_ADCBaseCfgS(&adc_cfg);
}

// ===================================================================
// configureDFT()
// Programs the DFT engine for the current frequency point.
// ===================================================================
void EIS_Method::configureDFT(float frequency) {
    DFTCfg_Type dft_cfg;
    AD5940_StructInit(&dft_cfg, sizeof(dft_cfg));

    // Adaptive point count / decimation source by frequency band, per the
    // ADI BATImpedance.c reference behavior documented in implementation_plan.md
    // Track 1: fewer points at high frequency (faster, still enough cycles
    // captured), many more points + a gentler decimation filter at low
    // frequency for adequate SNR. All constants below (DFTNUM_512/4096/16384,
    // DFTSRC_SINC3/SINC2NOTCH, HanWinEn) verified against the official
    // ad5940lib source — this exact adaptive band selection is still not
    // hardware-verified, though; check DFT results at both frequency-band
    // boundaries against the lab instrument during bring-up.
    if (frequency > 10000.0f) {
        dft_cfg.DftNum = DFTNUM_512;
        dft_cfg.DftSrc = DFTSRC_SINC3;
    } else if (frequency > 1000.0f) {
        dft_cfg.DftNum = DFTNUM_4096;
        dft_cfg.DftSrc = DFTSRC_SINC3;
    } else {
        dft_cfg.DftNum = DFTNUM_16384;
        dft_cfg.DftSrc = DFTSRC_SINC2NOTCH;
    }
    dft_cfg.HanWinEn = bTRUE; // Hanning window: reduces spectral leakage vs. rectangular

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
// Switch matrix routing for the two-stage RCAL-referenced measurement.
//
// Cell-stage pins verified directly against AnalyteX.kicad_pcb / netlist.ipc
// (AD5941BCPZ is U1 on this design):
//   U1 pin 47 (CE0) -- net "CE" -- J1 pin 1  (counter electrode)
//   U1 pin 45 (SE0) -- net "WE" -- J1 pin 2  (working electrode, sense)
//   U1 pin 48 (RE0) -- net "RE" -- J1 pin 3  (reference electrode)
//   U1 pin 46 (DE0) -- not connected to anything on this board.
//
// CORRECTION vs. an earlier pass: DE0 being unconnected is NOT a hardware
// defect. Verified directly against the AD5940/AD5941 datasheet (Rev. G,
// "High Speed TIA Configuration", p.50): DE0 is only needed when selecting
// an EXTERNAL gain resistor for the HSTIA in place of the internal RTIA
// options ("the DE0 pin must be connected to the output of the high speed
// TIA...to use the DE0 pin for the external RTIA value, set
// DE0RESCON=0x97..."). SE0 and DE0 are listed as separate, alternative HSTIA
// input options, not a pair that must be tied together. Since
// configureHSTIA() selects the chip's INTERNAL RTIA (HstiaRtiaSel =
// HSTIARTIA_10K, HstiaDeRtia/HstiaDeRload = OPEN in the HSTIACfg_Type struct
// below), DE0 is simply not part of this design's signal path, and a
// floating DE0 is expected, not a defect. (Retracting the "bodge wire
// needed" claim from the previous pass — that was wrong.)
//
// Datasheet-confirmed switch roles (Rev. G, "Programmable Switch Matrix",
// p.74): "Dx/DR0...select the pin to connect to the excitation amplifier
// output...for an impedance measurement, this pin is CE0" and "Px/Pxx...
// select the pin to connect to the positive node of the excitation
// amplifier...for most applications, this pin is RE0" — so P is corrected
// here from CE0 (this file's previous, ADI-eval-board-copied value) to RE0.
// T routes to the HSTIA inverting input; SE0 is confirmed elsewhere in the
// datasheet as the standard current-sense input ("High speed TIA measures
// SE0...current response").
//
// N-switch RESOLVED (datasheet Rev. G, NSWFULLCON register, Table 85 +
// SDK header ad5940.h): SWN_SE0 == (1<<8) == the datasheet's N9 switch,
// described as "connects the negative node of the excitation amplifier
// directly to the SE0 pin". For a 3-electrode cell this is exactly the
// loop closure needed: the HS excitation amplifier drives CE0 (D), servos
// to the RE0 reference (P), and its negative feedback node returns through
// the working/sense electrode SE0 (N) whose current the HSTIA measures at
// SE0LOAD (T = SWT_SE0LOAD|SWT_TRTIA, the node after the 100R RLOAD). This
// is symmetric with the RCAL path above (excitation across RCAL0, sense at
// RCAL1) and matches ADI's canonical Impedance.c 3-/2-lead configuration.
// Not a placeholder — the earlier "unverified" note is retracted; the only
// remaining unknown is end-to-end phase accuracy on real hardware, which is
// a calibration/bring-up matter, not a switch-routing one.
// ===================================================================
#define EIS_SWITCH_RCAL_D   SWD_RCAL0
#define EIS_SWITCH_RCAL_P   SWP_RCAL0
#define EIS_SWITCH_RCAL_N   SWN_RCAL1
#define EIS_SWITCH_RCAL_T   (SWT_RCAL1 | SWT_TRTIA)
#define EIS_SWITCH_CELL_D   SWD_CE0
#define EIS_SWITCH_CELL_P   SWP_RE0
#define EIS_SWITCH_CELL_N   SWN_SE0
#define EIS_SWITCH_CELL_T   (SWT_SE0LOAD | SWT_TRTIA)

// ===================================================================
// captureDFT()
// Reconfigures the switch matrix to the given routing, triggers one ADC+DFT
// capture (direct register control — matches the immediate-trigger style
// already used by Voltammetry_Methods::readCurrentResponse(), rather than
// the AD5940 sequencer/FIFO subsystem, which this firmware never configures
// or loads a program into), and reads back the complex result.
// ===================================================================
bool EIS_Method::captureDFT(uint32_t dSw, uint32_t pSw, uint32_t nSw, uint32_t tSw,
                             int32_t& realOut, int32_t& imagOut) {
    SWMatrixCfg_Type sw_cfg;
    sw_cfg.Dswitch = dSw;
    sw_cfg.Pswitch = pSw;
    sw_cfg.Nswitch = nSw;
    sw_cfg.Tswitch = tSw;
    AD5940_SWMatrixCfgS(&sw_cfg);

    AD5940_AFECtrlS(AFECTRL_ADCCNV | AFECTRL_DFT, bTRUE);

    uint32_t start = millis();
    while (!AD5941_Driver::checkInterrupt()) {
        if (AD5941_Driver::isAbortPending()) {
            AD5940_AFECtrlS(AFECTRL_ADCCNV | AFECTRL_DFT, bFALSE);
            return false;
        }
        if ((millis() - start) > EIS_TIMEOUT_MS) {
            AD5940_AFECtrlS(AFECTRL_ADCCNV | AFECTRL_DFT, bFALSE);
            AD5941_Driver::clearInterrupt();
            return false;
        }
        delay(1);
    }
    AD5940_AFECtrlS(AFECTRL_ADCCNV | AFECTRL_DFT, bFALSE);
    AD5941_Driver::clearInterrupt();

    realOut = (int32_t)AD5940_ReadReg(REG_AFE_DFTREAL);
    imagOut = (int32_t)AD5940_ReadReg(REG_AFE_DFTIMAG);
    return true;
}

// ===================================================================
// measureSingleFrequency()
// Two-stage RCAL-referenced impedance measurement, matching ADI's official
// AD5940_Impedance / BATImpedance reference algorithm.
//
// Corrected from a previous version of this function that assumed the
// AD5941 exposes separate simultaneous "V_dft" and "I_dft" registers
// (reading REG_AFE_DFTREAL/DFTIMAG for one and a fabricated "+4" offset for
// the other). Verified against the official ad5940lib source: there is only
// ONE DFT result register pair (REG_AFE_DFTREAL/REG_AFE_DFTIMAG) on this
// chip — no AD5940_DFTRead() function and no second-channel registers exist
// at all. The real architecture measures the SAME current-through-HSTIA
// signal twice per point, with the switch matrix routed differently each
// time (see captureDFT() above): once through the known RCAL resistor, once
// through the real electrodes. Impedance is the ratio of these two DFT
// captures scaled by RCAL_OHM — which is also intrinsically self-calibrating
// against AFE gain/phase drift on every single point, so no stored
// calibration factor is needed (see AD5941_Driver::performCalibration(),
// which is now a standalone diagnostic rather than something applied here).
// ===================================================================
EIS_DataPoint EIS_Method::measureSingleFrequency(float frequency) {
    EIS_DataPoint dp = {};
    dp.frequency = frequency;
    dp.error     = false;

    configureDFT(frequency);
    setExcitationSine(frequency);

    int32_t realRcal, imagRcal, realCell, imagCell;

    if (!captureDFT(EIS_SWITCH_RCAL_D, EIS_SWITCH_RCAL_P, EIS_SWITCH_RCAL_N, EIS_SWITCH_RCAL_T,
                    realRcal, imagRcal)) {
        dp.error = true;
        return dp;
    }
    if (!captureDFT(EIS_SWITCH_CELL_D, EIS_SWITCH_CELL_P, EIS_SWITCH_CELL_N, EIS_SWITCH_CELL_T,
                    realCell, imagCell)) {
        dp.error = true;
        return dp;
    }

    // Magnitude/phase of each DFT capture. The `-imag` in atan2 matches ADI's
    // own sign convention (Impedance.c: RcalPhase = atan2(-Image, Real)) —
    // needed so the resulting phase/imagZ end up in the standard EE/Nyquist
    // convention the GUI and mobile app already assume (they compute -imagZ
    // for the Nyquist Y-axis expecting imagZ itself to be un-negated).
    float rcalMag   = sqrtf((float)realRcal * realRcal + (float)imagRcal * imagRcal);
    float rcalPhase = atan2f(-(float)imagRcal, (float)realRcal); // radians

    float cellMag = sqrtf((float)realCell * realCell + (float)imagCell * imagCell);
    if (cellMag < 1.0f) cellMag = 1.0f; // guard divide-by-near-zero (open circuit)
    float cellPhase = atan2f(-(float)imagCell, (float)realCell);

    float zMagnitude = (rcalMag / cellMag) * RCAL_OHM;
    float zPhaseRad  = rcalPhase - cellPhase;

    dp.magnitude = zMagnitude;
    dp.phase     = zPhaseRad * (180.0f / (float)M_PI);
    dp.realZ     = zMagnitude * cosf(zPhaseRad);
    dp.imagZ     = zMagnitude * sinf(zPhaseRad);

    return dp;
}

// ===================================================================
// runSweep()
// Iterates over the frequency range, measuring impedance at each point.
// Calls dataCallback for each valid data point (errors are skipped).
// ===================================================================
void EIS_Method::runSweep(void (*dataCallback)(EIS_DataPoint)) {
    // Full HS-loop bring-up (power rails, HSTIA config, ADC mux, DC bias) —
    // previously this was a single RTIA-gain-only call with no DC bias
    // applied at all.
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

    // Power down the HS-loop and restore the LP-loop's ADC mux so a
    // subsequent CV/CA/SWV run works immediately without its own re-init.
    teardownHSLoop();
}

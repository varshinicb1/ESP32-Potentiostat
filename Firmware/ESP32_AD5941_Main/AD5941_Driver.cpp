#include "AD5941_Driver.h"
#include "EIS_Method.h"
#include "SharedSPIBus.h"
#include <math.h>

extern "C" {
#include <ad5940.h>
}

// ===================================================================
// Static member definitions
// ===================================================================
SPIClass*     AD5941_Driver::spi              = nullptr;
bool          AD5941_Driver::spiInitialized   = false;
volatile bool AD5941_Driver::interruptOccurred = false;
volatile bool AD5941_Driver::abortPending      = false;

float AD5941_Driver::calGain     = 1.0f;
float AD5941_Driver::calPhaseRad = 0.0f;
float AD5941_Driver::calTempC    = NAN;
bool  AD5941_Driver::calValid    = false;

// ===================================================================
// ISR — IRAM_ATTR ensures execution from IRAM even during flash access
// ===================================================================
void IRAM_ATTR AD5941_Driver::isrHandler() {
    interruptOccurred = true;
}

// ===================================================================
// initSPI()
// Guarded: only creates the SPIClass object once.
// On POST retries, this is safe to call multiple times.
// ===================================================================
void AD5941_Driver::initSPI() {
    if (!spiInitialized) {
        spi = new SPIClass(FSPI);
        spi->begin(AD5941_SCK, AD5941_MISO, AD5941_MOSI, AD5941_CS);
        spiInitialized = true;
    }
}

// ===================================================================
// initMCU()
// ===================================================================
void AD5941_Driver::initMCU() {
    // Idempotent — safe to call again from TaskDAQ; must exist before any
    // AD5941 CS toggle (see setCS()) or Display_LVGL/Storage SPI access.
    SharedSPIBus::init();

    pinMode(AD5941_CS,    OUTPUT);
    pinMode(AD5941_RESET, OUTPUT);
    pinMode(AD5941_RELAY, OUTPUT);
    pinMode(AD5941_INT,   INPUT_PULLUP);

    // Safe defaults
    digitalWrite(AD5941_CS,    HIGH);
    digitalWrite(AD5941_RESET, HIGH);
    digitalWrite(AD5941_RELAY, LOW);

    initSPI();

    // Attach falling-edge interrupt for AD5941 GP0_INT (conversion complete)
    attachInterrupt(digitalPinToInterrupt(AD5941_INT), isrHandler, FALLING);
}

// ===================================================================
// resetChip()
// ===================================================================
void AD5941_Driver::resetChip() {
    digitalWrite(AD5941_RESET, LOW);
    delay(10);
    digitalWrite(AD5941_RESET, HIGH);
    delay(20);
}

// ===================================================================
// configureAFE()
// One-time AFE bring-up, previously entirely missing from this firmware
// (see the warning block at the top of AD5941_Driver.h). Covers the
// reference buffers and the LP-loop (LPDAC+LPTIA) used by CV/CA/SWV.
// Field values and switch combinations are taken directly from ADI's
// official AD5940_Amperometric.c / AD5940_LpLoop.c examples and the
// datasheet's "Low Power Potentiostat" section (CE0=excitation output,
// RE0=feedback sense, SE0=current sense) — not independently derived.
// Call once after initMCU()/resetChip(), before any measurement.
// ===================================================================
void AD5941_Driver::configureAFE() {
    // Reference buffers. Lp* enable the LP-loop (CV/CA/SWV); Hp* enable the
    // HS-loop (EIS) — both are enabled here since either loop can run later
    // and AD5940_REFCfgS() only touches shared reference registers, not
    // anything loop-specific.
    AFERefCfg_Type ref_cfg;
    AD5940_StructInit(&ref_cfg, sizeof(ref_cfg));
    ref_cfg.LpBandgapEn = bTRUE;
    ref_cfg.LpRefBufEn  = bTRUE;
    ref_cfg.HpBandgapEn = bTRUE;
    ref_cfg.Hp1V8BuffEn = bTRUE;
    ref_cfg.Hp1V1BuffEn = bTRUE;
    AD5940_REFCfgS(&ref_cfg);

    // LP-loop: LPDAC drives the bias point, LPTIA senses working-electrode
    // current. RTIA=10K matches Voltammetry_Methods.cpp's RTIA_OHM constant.
    // LpDacSW includes LPDACSW_VZERO2HSTIA in addition to the LP-loop's own
    // switches so EIS_Method::applyDCBias() (a separate, per-sweep call) can
    // route the same LPDAC Vzero output to the HSTIA bias mux later without
    // needing to reconfigure this base LP-loop setup.
    LPLoopCfg_Type lp_cfg;
    AD5940_StructInit(&lp_cfg, sizeof(lp_cfg));
    lp_cfg.LpDacCfg.LpdacSel      = LPDAC0;
    lp_cfg.LpDacCfg.DacData12Bit  = 0x800; // overwritten per-measurement by Voltammetry_Methods::setVoltageBias()
    lp_cfg.LpDacCfg.DacData6Bit   = 32;
    lp_cfg.LpDacCfg.DataRst       = bFALSE;
    lp_cfg.LpDacCfg.LpDacSW       = LPDACSW_VBIAS2LPPA | LPDACSW_VBIAS2PIN |
                                    LPDACSW_VZERO2LPTIA | LPDACSW_VZERO2PIN |
                                    LPDACSW_VZERO2HSTIA;
    lp_cfg.LpDacCfg.LpDacRef      = LPDACREF_2P5;
    lp_cfg.LpDacCfg.LpDacSrc      = LPDACSRC_MMR;
    lp_cfg.LpDacCfg.LpDacVbiasMux = LPDACVBIAS_12BIT;
    lp_cfg.LpDacCfg.LpDacVzeroMux = LPDACVZERO_6BIT;
    lp_cfg.LpDacCfg.PowerEn       = bTRUE;

    lp_cfg.LpAmpCfg.LpAmpSel    = LPAMP0;
    lp_cfg.LpAmpCfg.LpAmpPwrMod = LPAMPPWR_NORM;
    lp_cfg.LpAmpCfg.LpPaPwrEn   = bTRUE;
    lp_cfg.LpAmpCfg.LpTiaPwrEn  = bTRUE;
    lp_cfg.LpAmpCfg.LpTiaRtia   = LPTIARTIA_10K;
    // Switch combination taken directly from ADI's AD5940_Amperometric.c
    // (the internal-RTIA-selected branch).
    //
    // RESOLVED discrepancy: the datasheet's own Table 21 ("Recommended
    // Switch Settings", Amperometric Mode row) lists LPTIASW0 = 0x302C,
    // i.e. bits {2,3,5,12,13} — one bit different from this (bit 3 instead
    // of bit 4). The datasheet's LPTIASW0 bit table only gives generic
    // "SWn switch control" descriptions with no text mapping each SWn to
    // the physical node it connects (that detail is only in Figure 22, an
    // image, not extractable from the text conversion). Decision: keep the
    // value from Amperometric.c — shipped, presumably hardware-tested ADI
    // example code is stronger evidence than a documentation table cell,
    // and Table 21's row may describe a slightly different sub-mode than
    // the specific "RTIA selected, diode path closed" branch this is
    // copied from. If bring-up shows anomalous LPTIA behavior, this is the
    // first thing to re-check against Figure 22 directly.
    lp_cfg.LpAmpCfg.LpTiaSW = LPTIASW(2) | LPTIASW(4) | LPTIASW(5) |
                              LPTIASW(12) | LPTIASW(13);
    AD5940_LPLoopCfgS(&lp_cfg);

    // Route the ADC to the LPTIA's filtered output. The datasheet explicitly
    // recommends this exact mux selection when using the low power TIA:
    // "Analog Devices recommends that the LPTIA0_P_LPF0 mux option be
    // selected as the ADC input when using the low power TIA."
    // AD5940_ADCBaseCfgS() only touches REG_AFE_ADCCON (verified in
    // ad5940.c), so this is safe to call independent of any DFT/filter
    // config EIS_Method sets up separately.
    ADCBaseCfg_Type adc_cfg;
    AD5940_StructInit(&adc_cfg, sizeof(adc_cfg));
    adc_cfg.ADCMuxP = ADCMUXP_AIN4;
    adc_cfg.ADCMuxN = ADCMUXN_VZERO0;
    adc_cfg.ADCPga  = ADCPGA_1;
    AD5940_ADCBaseCfgS(&adc_cfg);
}

// ===================================================================
// Pin Controls
// ===================================================================
// Every AD5941 register access (AD5940_ReadReg/WriteReg, and everything
// built on them) goes through the SDK's own CsClr()/...transfer.../CsSet()
// pattern — see the AD5940_CsClr/AD5940_CsSet C bindings below. Tying the
// shared-bus lock to CS assert/deassert here means every register access is
// automatically protected without touching each individual transfer
// function, and the critical section matches the real electrical boundary:
// the AD5941 only expects exclusive use of the shared MOSI/SCLK/MISO lines
// while ITS OWN CS is asserted low. See SharedSPIBus.h for why this exists.
void AD5941_Driver::setCS(bool high) {
    if (!high) {
        // CS going low: about to start a transaction. Block (don't skip) —
        // a dropped/interleaved AD5941 register access corrupts whatever
        // measurement is in progress, unlike a skipped LVGL frame.
        SharedSPIBus::lockBlocking();
    }
    digitalWrite(AD5941_CS, high ? HIGH : LOW);
    if (high) {
        // CS back high: transaction (or transaction sequence) complete.
        SharedSPIBus::unlock();
    }
}

void AD5941_Driver::setReset(bool high) {
    digitalWrite(AD5941_RESET, high ? HIGH : LOW);
}

void AD5941_Driver::setRelay(bool connected) {
    digitalWrite(AD5941_RELAY, connected ? HIGH : LOW);
}

// ===================================================================
// enterSafeState()
// Called on emergency stop or POST failure. Instantly disconnects electrodes.
// ===================================================================
void AD5941_Driver::enterSafeState() {
    // 1. Open physical isolation relay immediately
    setRelay(false);

    // 2. Disable Waveform Generator
    AD5940_AFECtrlS(AFECTRL_WG, bFALSE);

    // 3. Open all switches in AFE switch matrix (isolate CE, WE, RE)
    AD5940_WriteReg(REG_AFE_SWCON, 0x00010000);

    // 4. Return the DAC to the neutral (0V cell bias) operating point.
    // Previously this wrote LPDAC 12-bit code 0 while keeping whatever 6-bit
    // sub-code happened to be in the register — code 0 is the DAC_MIN_MV rail
    // (200 mV) in Voltammetry_Methods::setVoltageBias's own mapping, not the
    // VREF_TIA_MV (1100 mV) "virtual ground" used as zero-bias everywhere
    // else. The relay is already open at this point (step 1, above) so this
    // was never reaching the electrodes, but leave the chip state genuinely
    // neutral rather than parked at a rail value, for defense-in-depth.
    Voltammetry_Methods::setVoltageBias(0.0f);
}

// ===================================================================
// readInternalVref()
// Temporarily switches ADC mux to Bandgap Vref channel, reads, and restores.
// Disables the interrupt during the channel switch to avoid ISR corruption.
// ===================================================================
float AD5941_Driver::readInternalVref() {
    // Temporarily disable AD5941 interrupt during ADC mux change
    detachInterrupt(digitalPinToInterrupt(AD5941_INT));

    uint32_t oldMux = AD5940_ReadReg(REG_AFE_ADCCON);
    AD5940_WriteReg(REG_AFE_ADCCON, 0x0000001B); // Bandgap Vref channel
    AD5940_AFECtrlS(AFECTRL_ADCCNV, bTRUE);      // Start ADC conversion
    delay(2);
    int32_t raw = AD5940_ReadReg(REG_AFE_ADCDAT); // Read result (correct register name)
    AD5940_AFECtrlS(AFECTRL_ADCCNV, bFALSE);     // Stop conversion
    AD5940_WriteReg(REG_AFE_ADCCON, oldMux);

    // Restore interrupt
    attachInterrupt(digitalPinToInterrupt(AD5941_INT), isrHandler, FALLING);

    return ((float)raw / 32767.0f) * 1.82f;
}

// ===================================================================
// readInternalTemperature()
// ===================================================================
float AD5941_Driver::readInternalTemperature() {
    detachInterrupt(digitalPinToInterrupt(AD5941_INT));

    uint32_t oldMux = AD5940_ReadReg(REG_AFE_ADCCON);
    AD5940_WriteReg(REG_AFE_ADCCON, 0x0000001A); // Temp sensor channel
    AD5940_AFECtrlS(AFECTRL_ADCCNV, bTRUE);      // Start ADC conversion
    delay(2);
    int32_t raw = AD5940_ReadReg(REG_AFE_ADCDAT); // Read result
    AD5940_AFECtrlS(AFECTRL_ADCCNV, bFALSE);     // Stop conversion
    AD5940_WriteReg(REG_AFE_ADCCON, oldMux);

    attachInterrupt(digitalPinToInterrupt(AD5941_INT), isrHandler, FALLING);

    float volt   = ((float)raw / 32767.0f) * 1.82f;
    float voltMV = volt * 1000.0f;
    return (voltMV - 813.0f) / 2.7f + 25.0f;
}

// ===================================================================
// performPOST()
// 1. SPI scratch register write/read
// 2. Vref in-band check [1.70V, 1.95V]
// 3. Die temperature in-band check [-40°C, 85°C]
// ===================================================================
bool AD5941_Driver::performPOST(String& errorMsg) {
    // 1. SPI communication test — write/read REG_AFE_REPEATADCCNV (safe R/W register).
    //    We restore the original value afterwards.
    uint32_t origVal  = AD5940_ReadReg(REG_AFE_REPEATADCCNV);
    uint32_t testValue = 0x000001A5; // Arbitrary pattern, fits 16-bit register
    AD5940_WriteReg(REG_AFE_REPEATADCCNV, testValue);
    uint32_t readValue = AD5940_ReadReg(REG_AFE_REPEATADCCNV);
    AD5940_WriteReg(REG_AFE_REPEATADCCNV, origVal); // Restore
    if (readValue != testValue) {
        errorMsg = "SPI register mismatch. Expected 0x" + String(testValue, HEX) + ", got 0x" + String(readValue, HEX);
        return false;
    }

    // 2. Vref validation
    float vref = readInternalVref();
    if (vref < 1.70f || vref > 1.95f) {
        errorMsg = "Vref out of bounds: " + String(vref, 3) + " V (expected 1.70-1.95 V)";
        return false;
    }

    // 3. Temperature validation
    float temp = readInternalTemperature();
    if (temp < -40.0f || temp > 85.0f) {
        errorMsg = "Die temp out of bounds: " + String(temp, 1) + " °C (expected -40 to 85 °C)";
        return false;
    }

    errorMsg = "POST PASS. Temp: " + String(temp, 1) + "°C, Vref: " + String(vref, 3) + " V";
    return true;
}

// ===================================================================
// performCalibration()
// IEC 61010-1 §5.4 — accuracy maintenance / end-to-end sanity check.
//
// REQUIRES the operator to connect a precision reference resistor of value
// RCAL_OHM directly across the WE/RE (and CE, tied to RE/WE as appropriate
// for a 2-terminal reference load) electrode terminals in place of a real
// cell before calling this. Manual, operator-triggered step (CAL command) —
// not run automatically, since the firmware has no way to know a reference
// load is connected instead of a sample.
//
// NOTE: EIS_Method::measureSingleFrequency() now computes every impedance
// point as a ratio against the AD5941's *internal* RCAL0/RCAL1 resistor
// (see that function's comment), so it is intrinsically self-calibrating —
// there's no separate stored gain/phase factor left to "apply" here anymore.
// What this function actually checks: with a known EXTERNAL resistor
// connected as the "cell", does the ratiometric measurement read back close
// to that resistor's true value? A result far from 1.0x gain / 0 deg phase
// means either the internal RCAL0/1's true resistance doesn't match what
// AD5941_Driver.h assumes, or the switch-matrix routing (see
// EIS_SWITCH_CELL_* in EIS_Method.cpp) isn't reaching the electrodes
// correctly — this is a genuine end-to-end validation of the whole EIS
// signal path, useful before trusting a comparison against a lab
// instrument, even though it's no longer "corrective."
//
// NOTE: This only exercises the HSTIA/EIS signal path. CV/CA/SWV use a
// separate (LPTIA) current-measurement path with no equivalent self-check —
// their absolute current readings remain unverified against a reference.
// ===================================================================
#define CAL_FREQUENCY_HZ    1000.0f  // Reference frequency for gain/phase cal
#define CAL_AMPLITUDE_MV    50.0f    // Excitation amplitude used during cal

// Scratch storage for the single calibration data point. EIS_Method::runSweep
// only accepts a plain function pointer (no captured lambda state), so the
// callback below stashes its result here.
static EIS_DataPoint s_calRawPoint;
static void captureCalPoint(EIS_DataPoint dp) { s_calRawPoint = dp; }

bool AD5941_Driver::performCalibration(String& errorMsg) {
    s_calRawPoint = EIS_DataPoint{};
    s_calRawPoint.error = true;

    // steps=1 with start==stop runs two identical-frequency measurements
    // (EIS_Method::runSweep divides by `steps`, so steps=0 would be a
    // divide-by-zero); the second overwrites the first in s_calRawPoint,
    // giving a small amount of settling/averaging for free.
    EIS_Method calMeasurement(CAL_FREQUENCY_HZ, CAL_FREQUENCY_HZ, 1,
                              CAL_AMPLITUDE_MV, 0.0f, false);
    calMeasurement.runSweep(captureCalPoint);

    // Sanity-bound the raw reading against the expected order of magnitude.
    // Catches "no resistor connected" (near-open, huge/garbage magnitude) and
    // "wrong/shorted load" (near-zero magnitude) rather than anchoring a
    // nonsense gain correction.
    if (s_calRawPoint.error ||
        s_calRawPoint.magnitude < (RCAL_OHM * 0.2f) ||
        s_calRawPoint.magnitude > (RCAL_OHM * 5.0f)) {
        errorMsg = "Calibration measurement failed or implausible ("
                   + String(s_calRawPoint.magnitude, 1) + " ohm raw). "
                   "Confirm the RCAL reference resistor is connected across WE/RE and retry.";
        return false;
    }

    // Informational only now — not applied to future measurements (see the
    // function comment above). "calGain"/"calPhaseRad" here just describe
    // how far this end-to-end check deviated from ideal (1.0x / 0 deg).
    calGain     = RCAL_OHM / s_calRawPoint.magnitude;
    calPhaseRad = s_calRawPoint.phase * (float)M_PI / 180.0f;
    calTempC    = readInternalTemperature();
    calValid    = true;

    char buf[128];
    snprintf(buf, sizeof(buf),
             "PASS. External resistor read back as %.1f ohm @ %.1f deg "
             "(ideal: %.1f ohm @ 0 deg). Deviation: x%.4f gain, %.3f deg phase. Ref temp %.1fC",
             s_calRawPoint.magnitude, s_calRawPoint.phase, RCAL_OHM, calGain, s_calRawPoint.phase, calTempC);
    errorMsg = String(buf);
    return true;
}

float AD5941_Driver::getCalGain()     { return calValid ? calGain : 1.0f; }
float AD5941_Driver::getCalPhaseRad() { return calValid ? calPhaseRad : 0.0f; }
bool  AD5941_Driver::isCalibrationValid() { return calValid; }

bool AD5941_Driver::needsRecalibration() {
    if (!calValid) return true;
    float currentTemp = readInternalTemperature();
    return fabsf(currentTemp - calTempC) > CAL_TEMP_HYSTERESIS_C;
}

// ===================================================================
// SPI Transfer functions (called by ADI SDK C bindings below)
// ===================================================================
void AD5941_Driver::writeBytes(uint8_t* pTxBuffer, uint32_t size) {
    spi->beginTransaction(SPISettings(16000000, MSBFIRST, SPI_MODE0));
    spi->transfer(pTxBuffer, size);
    spi->endTransaction();
}

void AD5941_Driver::readBytes(uint8_t* pRxBuffer, uint32_t size) {
    spi->beginTransaction(SPISettings(16000000, MSBFIRST, SPI_MODE0));
    memset(pRxBuffer, 0xFF, size);
    spi->transfer(pRxBuffer, size);
    spi->endTransaction();
}

void AD5941_Driver::writeReadBytes(uint8_t* pTxBuffer, uint8_t* pRxBuffer, uint32_t size) {
    spi->beginTransaction(SPISettings(16000000, MSBFIRST, SPI_MODE0));
    memcpy(pRxBuffer, pTxBuffer, size);
    spi->transfer(pRxBuffer, size);
    spi->endTransaction();
}

// ===================================================================
// Timing & Interrupt helpers
// ===================================================================
uint32_t AD5941_Driver::getMCUTick() { return millis(); }
void     AD5941_Driver::delayMs(uint32_t ms) { delay(ms); }
bool     AD5941_Driver::checkInterrupt()  { return interruptOccurred; }
void     AD5941_Driver::clearInterrupt()  { interruptOccurred = false; }

// ===================================================================
// Abort flag helpers
// ===================================================================
void AD5941_Driver::setAbortFlag(bool val) { abortPending = val; }
bool AD5941_Driver::isAbortPending()       { return abortPending; }

// ===================================================================
// C Bindings required by ADI AD5940/5941 SDK
// ===================================================================
extern "C" {

void AD5940_ReadWriteNBytes(unsigned char* pSendBuffer, unsigned char* pRecvBuff, unsigned long length) {
    AD5941_Driver::writeReadBytes(pSendBuffer, pRecvBuff, (uint32_t)length);
}

void AD5940_DelayMs(unsigned int msec) {
    AD5941_Driver::delayMs(msec);
}

void AD5940_CsClr(void) { AD5941_Driver::setCS(false); }
void AD5940_CsSet(void) { AD5941_Driver::setCS(true);  }
void AD5940_RstClr(void){ AD5941_Driver::setReset(false); }
void AD5940_RstSet(void){ AD5941_Driver::setReset(true);  }

unsigned int AD5940_GetMCUTick(void) {
    return AD5941_Driver::getMCUTick();
}

// Interrupt controller: handled directly in initMCU() via attachInterrupt
uint32_t AD5940_INTC_Init(void) {
    return 0;
}

} // extern "C"

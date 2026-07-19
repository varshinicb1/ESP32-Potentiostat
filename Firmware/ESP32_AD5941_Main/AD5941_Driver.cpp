#include "AD5941_Driver.h"

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
// Pin Controls
// ===================================================================
void AD5941_Driver::setCS(bool high) {
    digitalWrite(AD5941_CS, high ? HIGH : LOW);
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

    // 4. Clamp DAC outputs to 0V (mid-scale for 2-complement DAC)
    uint32_t val = AD5940_ReadReg(REG_AFE_LPDACDAT0);
    uint8_t data6bit = (val >> 12) & 0x3F;
    AD5940_LPDAC0WriteS(0, data6bit);
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

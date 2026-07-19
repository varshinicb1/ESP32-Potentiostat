#include "Communication.h"
#include <BLE2902.h>

// ===================================================================
// Static member definitions
// ===================================================================
BLEServer*          Communication::pServer            = nullptr;
BLECharacteristic*  Communication::pTxCharacteristic  = nullptr;
BLECharacteristic*  Communication::pRxCharacteristic  = nullptr;
volatile bool       Communication::deviceConnected    = false;
volatile bool       Communication::wsClientConnected  = false;
WebSocketsServer    Communication::webSocket(81);
QueueHandle_t       Communication::rxCommandQueue     = nullptr;
QueueHandle_t       Communication::txDataQueue        = nullptr;
SemaphoreHandle_t   Communication::bleMutex           = nullptr;

// ===================================================================
// Internal helper — push raw JSON into rxCommandQueue from any callback.
// Called from BLE callback (BLE stack task) or WS event (background thread).
// Uses BaseType_t xHigherPriorityTaskWoken for ISR safety, but since these
// are not true ISR contexts we use the non-ISR queue API with a 0 timeout.
// ===================================================================
void Communication::enqueueCommand(const char* json, size_t len) {
    if (rxCommandQueue == nullptr) return;

    // Allocate a fixed buffer on the stack. We send a copy of the pointer
    // — actually we copy by value into a char array item in the queue.
    char buf[COMM_MAX_PAYLOAD_LEN];
    size_t copyLen = (len < COMM_MAX_PAYLOAD_LEN - 1) ? len : COMM_MAX_PAYLOAD_LEN - 1;
    memcpy(buf, json, copyLen);
    buf[copyLen] = '\0';

    // Non-blocking send — drop the command if the queue is full (overload protection)
    xQueueSendToBack(rxCommandQueue, buf, 0);
}

// ===================================================================
// BLE Server Callbacks
// ===================================================================
void Communication::MyServerCallbacks::onConnect(BLEServer* pServer) {
    deviceConnected = true;
}

void Communication::MyServerCallbacks::onDisconnect(BLEServer* pServer) {
    deviceConnected = false;
    // Restart advertising so new clients can connect
    pServer->startAdvertising();
}

// ===================================================================
// BLE RX Characteristic Callback — called from BLE stack task (Core 0)
// ===================================================================
void Communication::MyCallbacks::onWrite(BLECharacteristic* pCharacteristic) {
    std::string rxValue = pCharacteristic->getValue();
    if (rxValue.length() > 0) {
        enqueueCommand(rxValue.c_str(), rxValue.length());
    }
}

// ===================================================================
// WebSocket Event — called from WebSocketsServer library (same task as handleComm)
// ===================================================================
void Communication::webSocketEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
    switch (type) {
        case WStype_DISCONNECTED:
            wsClientConnected = false;
            break;
        case WStype_CONNECTED:
            wsClientConnected = true;
            break;
        case WStype_TEXT:
            enqueueCommand((char*)payload, length);
            break;
        case WStype_BIN:
            // Binary commands not yet supported
            break;
        default:
            break;
    }
}

// ===================================================================
// Communication::init()
// ===================================================================
void Communication::init() {
    // --- Create FreeRTOS primitives ---
    // Queue of char[COMM_MAX_PAYLOAD_LEN] items for incoming commands
    rxCommandQueue = xQueueCreate(COMM_CMD_QUEUE_DEPTH, COMM_MAX_PAYLOAD_LEN);
    // Queue of char[COMM_MAX_PAYLOAD_LEN] items for outgoing data
    txDataQueue    = xQueueCreate(COMM_TX_QUEUE_DEPTH, COMM_MAX_PAYLOAD_LEN);
    bleMutex       = xSemaphoreCreateMutex();

    configASSERT(rxCommandQueue != nullptr);
    configASSERT(txDataQueue    != nullptr);
    configASSERT(bleMutex       != nullptr);

    // --- BLE Stack ---
    BLEDevice::init("VidyuthLabs_Potentiostat");

    // Request a larger MTU to handle JSON payloads > 23 bytes.
    // The client must also request MTU negotiation; this sets our preference.
    BLEDevice::setMTU(512);

    pServer = BLEDevice::createServer();
    pServer->setCallbacks(new MyServerCallbacks());

    BLEService* pService = pServer->createService(SERVICE_UUID);

    // TX Characteristic: ESP32 → App (Notify)
    pTxCharacteristic = pService->createCharacteristic(
        CHARACTERISTIC_TX_UUID,
        BLECharacteristic::PROPERTY_NOTIFY
    );
    pTxCharacteristic->addDescriptor(new BLE2902());

    // RX Characteristic: App → ESP32 (Write)
    pRxCharacteristic = pService->createCharacteristic(
        CHARACTERISTIC_RX_UUID,
        BLECharacteristic::PROPERTY_WRITE
    );
    pRxCharacteristic->setCallbacks(new MyCallbacks());

    pService->start();
    pServer->getAdvertising()->start();

    // --- WiFi Access Point ---
    // TODO (production): derive password from chip UID stored in NVS.
    // For now, use a strong default — must be changed per-device before shipping.
    char apPassword[32];
    uint64_t chipId = ESP.getEfuseMac();
    snprintf(apPassword, sizeof(apPassword), "VL-%04X%08X",
             (uint16_t)(chipId >> 32), (uint32_t)chipId);
    WiFi.softAP("VidyuthLabs_AP", apPassword);

    // --- WebSocket Server on port 81 ---
    webSocket.begin();
    webSocket.onEvent(webSocketEvent);
}

// ===================================================================
// Communication::handleComm()
// Called from TaskComm loop on Core 0. Handles all BLE/WS work here,
// keeping BLE notify calls on a single task/core.
// ===================================================================
void Communication::handleComm() {
    // Service the WebSocket library (non-blocking)
    webSocket.loop();

    // Drain the outgoing data queue and transmit
    flushOutgoingQueue();
}

// ===================================================================
// flushOutgoingQueue() — internal, called only from TaskComm (Core 0)
// ===================================================================
void Communication::flushOutgoingQueue() {
    char buf[COMM_MAX_PAYLOAD_LEN];

    // Process up to 16 items per call to avoid starving other work
    for (int i = 0; i < 16; i++) {
        if (xQueueReceive(txDataQueue, buf, 0) != pdTRUE) {
            break; // Queue empty
        }

        // BLE notify — only safe from the task that owns the BLE characteristic
        if (deviceConnected) {
            if (xSemaphoreTake(bleMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                pTxCharacteristic->setValue((uint8_t*)buf, strlen(buf));
                pTxCharacteristic->notify();
                xSemaphoreGive(bleMutex);
            }
        }

        // WebSocket broadcast (thread-safe in WebSocketsServer when called here)
        webSocket.broadcastTXT(buf);
    }
}

// ===================================================================
// Public API
// ===================================================================
bool Communication::isDeviceConnected() {
    return deviceConnected || wsClientConnected;
}

bool Communication::availableCommand() {
    if (rxCommandQueue == nullptr) return false;
    return (uxQueueMessagesWaiting(rxCommandQueue) > 0);
}

String Communication::getNewCommand() {
    if (rxCommandQueue == nullptr) return "";
    char buf[COMM_MAX_PAYLOAD_LEN];
    if (xQueueReceive(rxCommandQueue, buf, 0) == pdTRUE) {
        return String(buf);
    }
    return "";
}

// Thread-safe: enqueue payload for transmission.
// May be called from TaskDAQ (Core 1) or any other context.
void Communication::sendDataPoint(const String& jsonPayload) {
    if (txDataQueue == nullptr) return;

    char buf[COMM_MAX_PAYLOAD_LEN];
    size_t len = jsonPayload.length();
    if (len >= COMM_MAX_PAYLOAD_LEN) len = COMM_MAX_PAYLOAD_LEN - 1;
    memcpy(buf, jsonPayload.c_str(), len);
    buf[len] = '\0';

    // Non-blocking: if queue is full, drop the sample rather than block TaskDAQ
    xQueueSendToBack(txDataQueue, buf, 0);
}

void Communication::sendBinaryData(uint8_t* buffer, size_t length) {
    // Binary data sent directly over WebSocket (not queued — only call from TaskComm)
    webSocket.broadcastBIN(buffer, length);
}

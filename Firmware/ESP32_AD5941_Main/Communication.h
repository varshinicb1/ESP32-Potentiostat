#ifndef COMMUNICATION_H
#define COMMUNICATION_H

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>
#include <WiFi.h>
#include <WebSocketsServer.h>
#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

// BLE UUID definitions
#define SERVICE_UUID           "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHARACTERISTIC_RX_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"
#define CHARACTERISTIC_TX_UUID "d063d8d6-4447-4952-b883-7be1a80d5d4d"

// Maximum JSON payload size (bytes) — must fit in BLE MTU after negotiation
#define COMM_MAX_PAYLOAD_LEN    512
// Maximum items in the incoming command queue
#define COMM_CMD_QUEUE_DEPTH    8
// Maximum items in the outgoing data queue (each is COMM_MAX_PAYLOAD_LEN bytes)
#define COMM_TX_QUEUE_DEPTH     32

class Communication {
private:
    static BLEServer* pServer;
    static BLECharacteristic* pTxCharacteristic;
    static BLECharacteristic* pRxCharacteristic;
    static volatile bool deviceConnected;
    static volatile bool wsClientConnected;
    static WebSocketsServer webSocket;

    // Thread-safe incoming command queue (filled by BLE/WS callbacks, drained by TaskComm)
    static QueueHandle_t rxCommandQueue;

    // Thread-safe outgoing data queue (filled by any task, drained by handleComm on TaskComm)
    static QueueHandle_t txDataQueue;

    // Mutex protecting BLE notify calls — only TaskComm touches BLE characteristics
    static SemaphoreHandle_t bleMutex;

    // BLE callbacks
    class MyServerCallbacks : public BLEServerCallbacks {
        void onConnect(BLEServer* pServer) override;
        void onDisconnect(BLEServer* pServer) override;
    };
    class MyCallbacks : public BLECharacteristicCallbacks {
        void onWrite(BLECharacteristic* pCharacteristic) override;
    };

    static void webSocketEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length);

    // Internal: push a raw string into rxCommandQueue (safe from ISR/callback context)
    static void enqueueCommand(const char* json, size_t len);

    // Internal: drain txDataQueue and transmit over BLE + WS. Must run on TaskComm.
    static void flushOutgoingQueue();

public:
    static void init();
    static void handleComm();   // Run in TaskComm loop — drains both queues

    static bool isDeviceConnected();

    // Returns true if a new command JSON is available
    static bool availableCommand();
    // Pops the next command JSON string (caller owns the buffer — caller must free or copy)
    static String getNewCommand();

    // Thread-safe: may be called from any task / any core.
    // Enqueues payload for transmission; actual send happens in handleComm().
    static void sendDataPoint(const String& jsonPayload);
    static void sendBinaryData(uint8_t* buffer, size_t length);
};

#endif // COMMUNICATION_H

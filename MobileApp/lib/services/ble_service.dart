import 'dart:async';
import 'dart:convert';
import 'package:flutter/foundation.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';

class BLEService extends ChangeNotifier {
  // Must match firmware Communication.h UUID definitions exactly
  static const String serviceUuid = "4fafc201-1fb5-459e-8fcc-c5c9c331914b";
  static const String rxUuid      = "beb5483e-36e1-4688-b7f5-ea07361b26a8";
  static const String txUuid      = "d063d8d6-4447-4952-b883-7be1a80d5d4d";

  BluetoothDevice?        connectedDevice;
  BluetoothCharacteristic? txCharacteristic; // ESP32 → App (notify)
  BluetoothCharacteristic? rxCharacteristic; // App → ESP32 (write)

  bool            isScanning  = false;
  bool            isConnected = false;
  List<ScanResult> scanResults = [];

  // Broadcast stream so multiple listeners (ChartPage, HomePage) can subscribe
  final StreamController<Map<String, dynamic>> _dataStreamController =
      StreamController<Map<String, dynamic>>.broadcast();
  Stream<Map<String, dynamic>> get dataStream => _dataStreamController.stream;

  StreamSubscription?                 _scanSubscription;
  StreamSubscription?                 _connectionStateSubscription;
  StreamSubscription?                 _notifySubscription;
  StreamSubscription<List<ScanResult>>? _scanResultsSubscription;

  // ================================================================
  // startScan()
  // Cancels any existing scan before starting a new one.
  // ================================================================
  void startScan() {
    // Cancel any previous scan subscriptions
    _scanSubscription?.cancel();
    _scanResultsSubscription?.cancel();
    scanResults.clear();
    isScanning = true;
    notifyListeners();

    _scanResultsSubscription = FlutterBluePlus.scanResults.listen((results) {
      // Filter to show only AnalyteX devices (kept the legacy "VidyuthLabs"/
      // "FreiStat" name fragments too so already-flashed field units with the
      // old BLE advertised name still show up in the scanner).
      scanResults = results
          .where((r) =>
              r.device.platformName.contains("AnalyteX") ||
              r.device.platformName.contains("FreiStat") ||
              r.device.platformName.contains("VidyuthLabs"))
          .toList();
      notifyListeners();
    });

    FlutterBluePlus.startScan(timeout: const Duration(seconds: 6));

    Future.delayed(const Duration(seconds: 6), () {
      isScanning = false;
      notifyListeners();
    });
  }

  // ================================================================
  // connectToDevice()
  // Guards against double-connect. Disconnects existing device first.
  // Negotiates MTU=512 after connecting.
  // Listens to connectionState to update isConnected automatically.
  // ================================================================
  Future<void> connectToDevice(BluetoothDevice device) async {
    // Disconnect existing device cleanly before connecting a new one
    if (isConnected) {
      await disconnect();
    }

    try {
      await device.connect(timeout: const Duration(seconds: 10));
    } catch (e) {
      debugPrint("BLEService: connect error: $e");
      return;
    }

    connectedDevice = device;
    isConnected     = true;
    notifyListeners();

    // Listen to connection state changes — auto-update isConnected on drop
    _connectionStateSubscription = device.connectionState.listen((state) {
      final connected = (state == BluetoothConnectionState.connected);
      if (isConnected != connected) {
        isConnected = connected;
        if (!connected) {
          // Clean up characteristics on disconnect
          txCharacteristic = null;
          rxCharacteristic = null;
          _notifySubscription?.cancel();
        }
        notifyListeners();
      }
    });

    // Request larger MTU to handle JSON payloads > 23 bytes
    try {
      await device.requestMtu(512);
    } catch (e) {
      debugPrint("BLEService: MTU negotiation failed: $e");
      // Non-fatal; proceed with default MTU
    }

    // Discover services and bind characteristics
    List<BluetoothService> services = await device.discoverServices();
    for (var service in services) {
      if (service.uuid.toString().toLowerCase() == serviceUuid) {
        for (var char in service.characteristics) {
          final cuuid = char.uuid.toString().toLowerCase();
          if (cuuid == txUuid) {
            txCharacteristic = char;
            await txCharacteristic!.setNotifyValue(true);
            _notifySubscription = txCharacteristic!.onValueReceived.listen(
              (value) {
                final rawJson = utf8.decode(value);
                try {
                  final data = jsonDecode(rawJson) as Map<String, dynamic>;
                  _dataStreamController.add(data);
                } catch (_) {
                  // Ignore malformed / incomplete packets silently
                }
              },
              onError: (e) => debugPrint("BLEService: notify error: $e"),
            );
          } else if (cuuid == rxUuid) {
            rxCharacteristic = char;
          }
        }
      }
    }
  }

  // ================================================================
  // sendCommand()
  // Serialises a command map to JSON and writes to the RX characteristic.
  // ================================================================
  Future<void> sendCommand(Map<String, dynamic> command) async {
    if (rxCharacteristic == null) {
      debugPrint("BLEService: sendCommand called but rxCharacteristic is null");
      return;
    }
    if (!isConnected) {
      debugPrint("BLEService: sendCommand called while disconnected");
      return;
    }
    try {
      final jsonStr = jsonEncode(command);
      final bytes   = utf8.encode(jsonStr);
      await rxCharacteristic!.write(bytes, withoutResponse: false);
    } catch (e) {
      debugPrint("BLEService: sendCommand error: $e");
    }
  }

  // ================================================================
  // disconnect()
  // ================================================================
  Future<void> disconnect() async {
    _notifySubscription?.cancel();
    _connectionStateSubscription?.cancel();
    try {
      await connectedDevice?.disconnect();
    } catch (_) {}
    isConnected      = false;
    connectedDevice  = null;
    txCharacteristic = null;
    rxCharacteristic = null;
    notifyListeners();
  }

  // ================================================================
  // dispose() — close the stream controller to prevent leaks
  // ================================================================
  @override
  void dispose() {
    _scanSubscription?.cancel();
    _scanResultsSubscription?.cancel();
    _notifySubscription?.cancel();
    _connectionStateSubscription?.cancel();
    _dataStreamController.close();
    super.dispose();
  }
}

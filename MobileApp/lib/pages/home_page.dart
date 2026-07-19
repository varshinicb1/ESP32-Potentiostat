import 'package:flutter/material.dart';
import 'package:provider/provider.dart';
import '../services/ble_service.dart';
import 'chart_page.dart';

class HomePage extends StatefulWidget {
  const HomePage({super.key});

  @override
  State<HomePage> createState() => _HomePageState();
}

class _HomePageState extends State<HomePage> {
  // ── CV controllers ─────────────────────────────────────────────
  final _cvStart    = TextEditingController(text: "-0.5");
  final _cvV1       = TextEditingController(text: "0.8");
  final _cvV2       = TextEditingController(text: "-0.8");
  final _cvScanRate = TextEditingController(text: "0.1");
  final _cvCycles   = TextEditingController(text: "2");

  // ── CA controllers ─────────────────────────────────────────────
  final _caStep = TextEditingController(text: "0.5");
  final _caDur  = TextEditingController(text: "10");
  final _caInt  = TextEditingController(text: "0.1");

  // ── SWV controllers ────────────────────────────────────────────
  final _swvStart    = TextEditingController(text: "-0.5");
  final _swvStop     = TextEditingController(text: "0.5");
  final _swvStep     = TextEditingController(text: "0.005");
  final _swvAmp      = TextEditingController(text: "0.025");
  final _swvFreq     = TextEditingController(text: "25");

  // ── EIS controllers ────────────────────────────────────────────
  final _eisStart = TextEditingController(text: "10");
  final _eisStop  = TextEditingController(text: "100000");
  final _eisSteps = TextEditingController(text: "50");
  final _eisAmp   = TextEditingController(text: "10");
  final _eisBias  = TextEditingController(text: "0");

  String selectedMethod = "CV";

  // ── Dispose all controllers to prevent memory leaks ────────────
  @override
  void dispose() {
    _cvStart.dispose();    _cvV1.dispose();    _cvV2.dispose();
    _cvScanRate.dispose(); _cvCycles.dispose();
    _caStep.dispose();     _caDur.dispose();   _caInt.dispose();
    _swvStart.dispose();   _swvStop.dispose(); _swvStep.dispose();
    _swvAmp.dispose();     _swvFreq.dispose();
    _eisStart.dispose();   _eisStop.dispose(); _eisSteps.dispose();
    _eisAmp.dispose();     _eisBias.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final ble = Provider.of<BLEService>(context);

    return Scaffold(
      appBar: AppBar(
        title: const Text("VidyuthLabs Potentiostat"),
        actions: [
          // Connection status chip
          Padding(
            padding: const EdgeInsets.symmetric(vertical: 12, horizontal: 8),
            child: Chip(
              avatar: Icon(
                ble.isConnected
                    ? Icons.bluetooth_connected
                    : Icons.bluetooth_disabled,
                size: 16,
                color: ble.isConnected ? Colors.greenAccent : Colors.red,
              ),
              label: Text(ble.isConnected ? "Connected" : "Disconnected",
                  style: const TextStyle(fontSize: 12)),
              backgroundColor: const Color(0xFF2A2A2A),
            ),
          ),
          IconButton(
            icon: const Icon(Icons.search),
            tooltip: "Scan for devices",
            onPressed: () => _showDeviceScanner(context, ble),
          ),
          if (ble.isConnected)
            IconButton(
              icon: const Icon(Icons.bluetooth_disabled),
              tooltip: "Disconnect",
              onPressed: () => ble.disconnect(),
            ),
        ],
      ),
      body: SingleChildScrollView(
        padding: const EdgeInsets.all(16.0),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: [
            // ── Technique selector ──────────────────────────────
            DropdownButtonFormField<String>(
              value: selectedMethod,
              decoration: const InputDecoration(
                labelText: "Electrochemical Method",
                prefixIcon: Icon(Icons.science),
              ),
              items: const [
                DropdownMenuItem(value: "CV",  child: Text("Cyclic Voltammetry (CV)")),
                DropdownMenuItem(value: "CA",  child: Text("Chronoamperometry (CA)")),
                DropdownMenuItem(value: "SWV", child: Text("Square Wave Voltammetry (SWV)")),
                DropdownMenuItem(value: "EIS", child: Text("Electrochemical Impedance (EIS)")),
              ],
              onChanged: (val) => setState(() => selectedMethod = val!),
            ),
            const SizedBox(height: 16),

            // ── Dynamic parameter form ──────────────────────────
            AnimatedSwitcher(
              duration: const Duration(milliseconds: 200),
              child: KeyedSubtree(
                key: ValueKey(selectedMethod),
                child: _buildForm(),
              ),
            ),
            const SizedBox(height: 24),

            // ── Start button ────────────────────────────────────
            ElevatedButton.icon(
              icon: const Icon(Icons.play_arrow),
              label: const Text("Start Measurement",
                  style: TextStyle(fontSize: 16, fontWeight: FontWeight.bold)),
              style: ElevatedButton.styleFrom(
                minimumSize: const Size.fromHeight(52),
              ),
              onPressed: ble.isConnected ? () => _startMeasurement(ble) : null,
            ),

            if (!ble.isConnected)
              const Padding(
                padding: EdgeInsets.only(top: 8),
                child: Text(
                  "Connect to a device using the scan button above.",
                  textAlign: TextAlign.center,
                  style: TextStyle(color: Colors.grey),
                ),
              ),
          ],
        ),
      ),
    );
  }

  // ──────────────────────────────────────────────────────────────
  Widget _buildForm() {
    switch (selectedMethod) {
      case "CV":  return _buildCVForm();
      case "CA":  return _buildCAForm();
      case "SWV": return _buildSWVForm();
      case "EIS": return _buildEISForm();
      default:    return const SizedBox.shrink();
    }
  }

  Widget _buildCVForm() => _formCard("Cyclic Voltammetry Parameters", [
    _field(_cvStart,    "Start Voltage (V)",    "-2.0 to 2.0"),
    _field(_cvV1,       "Vertex Voltage 1 (V)", "-2.0 to 2.0"),
    _field(_cvV2,       "Vertex Voltage 2 (V)", "-2.0 to 2.0"),
    _field(_cvScanRate, "Scan Rate (V/s)",      "0.001 to 10"),
    _field(_cvCycles,   "Cycles",               "1 to 100"),
  ]);

  Widget _buildCAForm() => _formCard("Chronoamperometry Parameters", [
    _field(_caStep, "Step Voltage (V)",     "-2.0 to 2.0"),
    _field(_caDur,  "Duration (s)",         "0.1 to 3600"),
    _field(_caInt,  "Sample Interval (s)",  "0.001 to 60"),
  ]);

  Widget _buildSWVForm() => _formCard("Square Wave Voltammetry Parameters", [
    _field(_swvStart, "Start Voltage (V)",  "-2.0 to 2.0"),
    _field(_swvStop,  "Stop Voltage (V)",   "-2.0 to 2.0"),
    _field(_swvStep,  "Step Height (V)",    "0.001 to 0.1"),
    _field(_swvAmp,   "Amplitude (V)",      "0.001 to 0.5"),
    _field(_swvFreq,  "Frequency (Hz)",     "1 to 1000"),
  ]);

  Widget _buildEISForm() => _formCard("Impedance Spectroscopy Parameters", [
    _field(_eisStart, "Start Frequency (Hz)",  "0.1 to 1M"),
    _field(_eisStop,  "Stop Frequency (Hz)",   "> Start"),
    _field(_eisSteps, "Steps",                 "1 to 1000"),
    _field(_eisAmp,   "AC Amplitude (mV)",     "1 to 200"),
    _field(_eisBias,  "DC Bias (mV)",          "-1000 to 1000"),
  ]);

  Widget _formCard(String title, List<Widget> fields) => Card(
    child: Padding(
      padding: const EdgeInsets.all(16),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Text(title,
              style: const TextStyle(fontWeight: FontWeight.bold, fontSize: 14)),
          const SizedBox(height: 12),
          ...fields.map((f) => Padding(
            padding: const EdgeInsets.only(bottom: 10),
            child: f,
          )),
        ],
      ),
    ),
  );

  Widget _field(TextEditingController ctrl, String label, String hint) =>
      TextField(
        controller: ctrl,
        keyboardType: const TextInputType.numberWithOptions(
            signed: true, decimal: true),
        decoration: InputDecoration(
          labelText: label,
          hintText: hint,
          hintStyle: const TextStyle(fontSize: 12, color: Colors.grey),
        ),
      );

  // ──────────────────────────────────────────────────────────────
  // _startMeasurement() — validates all fields before sending
  // ──────────────────────────────────────────────────────────────
  void _startMeasurement(BLEService ble) {
    try {
      final cmd = _buildCommand();
      ble.sendCommand(cmd);
      Navigator.push(
        context,
        MaterialPageRoute(
          builder: (_) => ChartPage(techniqueName: selectedMethod),
        ),
      );
    } on FormatException catch (e) {
      _showError("Invalid input: ${e.message}");
    } on RangeError catch (e) {
      _showError("Value out of range: ${e.message}");
    } catch (e) {
      _showError("Failed to start: $e");
    }
  }

  Map<String, dynamic> _buildCommand() {
    switch (selectedMethod) {
      case "CV":
        return {
          "method": "CV",
          "params": {
            "start_voltage": _parseDouble(_cvStart,    "Start Voltage",   -2.0, 2.0),
            "vertex_1":      _parseDouble(_cvV1,       "Vertex 1",        -2.0, 2.0),
            "vertex_2":      _parseDouble(_cvV2,       "Vertex 2",        -2.0, 2.0),
            "scan_rate":     _parseDouble(_cvScanRate, "Scan Rate",       0.001, 10.0),
            "cycles":        _parseInt   (_cvCycles,   "Cycles",          1, 100),
          }
        };
      case "CA":
        return {
          "method": "CA",
          "params": {
            "step_voltage": _parseDouble(_caStep, "Step Voltage", -2.0, 2.0),
            "duration":     _parseDouble(_caDur,  "Duration",     0.1, 3600.0),
            "interval":     _parseDouble(_caInt,  "Interval",     0.001, 60.0),
          }
        };
      case "SWV":
        return {
          "method": "SWV",
          "params": {
            "start_voltage": _parseDouble(_swvStart, "Start Voltage", -2.0, 2.0),
            "stop_voltage":  _parseDouble(_swvStop,  "Stop Voltage",  -2.0, 2.0),
            "step_height":   _parseDouble(_swvStep,  "Step Height",   0.001, 0.1),
            "amplitude":     _parseDouble(_swvAmp,   "Amplitude",     0.001, 0.5),
            "frequency":     _parseDouble(_swvFreq,  "Frequency",     1.0, 1000.0),
          }
        };
      case "EIS":
        final startFreq = _parseDouble(_eisStart, "Start Frequency", 0.1, 1e6);
        final stopFreq  = _parseDouble(_eisStop,  "Stop Frequency",  startFreq + 0.1, 1e6);
        return {
          "method": "EIS",
          "params": {
            "start_freq": startFreq,
            "stop_freq":  stopFreq,
            "steps":      _parseInt   (_eisSteps, "Steps",       1, 1000),
            "amplitude":  _parseDouble(_eisAmp,   "Amplitude",   1.0, 200.0),
            "bias":       _parseDouble(_eisBias,  "Bias",       -1000.0, 1000.0),
          }
        };
      default:
        throw FormatException("Unknown method: $selectedMethod");
    }
  }

  double _parseDouble(TextEditingController ctrl, String name,
      double min, double max) {
    final raw = ctrl.text.trim();
    if (raw.isEmpty) throw FormatException("$name cannot be empty");
    final val = double.tryParse(raw);
    if (val == null) throw FormatException("$name must be a number (got '$raw')");
    if (val < min || val > max) {
      throw RangeError.range(val.toInt(), min.toInt(), max.toInt(),
          name, "$name must be between $min and $max");
    }
    return val;
  }

  int _parseInt(TextEditingController ctrl, String name, int min, int max) {
    final raw = ctrl.text.trim();
    if (raw.isEmpty) throw FormatException("$name cannot be empty");
    final val = int.tryParse(raw);
    if (val == null) throw FormatException("$name must be an integer (got '$raw')");
    if (val < min || val > max) {
      throw RangeError.range(val, min, max, name,
          "$name must be between $min and $max");
    }
    return val;
  }

  void _showError(String msg) {
    ScaffoldMessenger.of(context).showSnackBar(
      SnackBar(
        content: Text(msg),
        backgroundColor: Colors.red[700],
        duration: const Duration(seconds: 4),
      ),
    );
  }

  // ──────────────────────────────────────────────────────────────
  // Device scanner bottom sheet
  // ──────────────────────────────────────────────────────────────
  void _showDeviceScanner(BuildContext context, BLEService ble) {
    ble.startScan();
    showModalBottomSheet(
      context: context,
      backgroundColor: const Color(0xFF1E1E1E),
      shape: const RoundedRectangleBorder(
          borderRadius: BorderRadius.vertical(top: Radius.circular(16))),
      builder: (ctx) => Consumer<BLEService>(
        builder: (ctx, ble, _) => Column(
          children: [
            const SizedBox(height: 12),
            Container(width: 40, height: 4,
                decoration: BoxDecoration(
                    color: Colors.grey[600],
                    borderRadius: BorderRadius.circular(2))),
            const SizedBox(height: 12),
            const Text("Scan & Connect",
                style: TextStyle(fontSize: 18, fontWeight: FontWeight.bold)),
            if (ble.isScanning)
              const Padding(
                padding: EdgeInsets.symmetric(horizontal: 16, vertical: 4),
                child: LinearProgressIndicator(),
              ),
            Expanded(
              child: ble.scanResults.isEmpty
                  ? const Center(
                      child: Text("No devices found.\nMake sure firmware is running.",
                          textAlign: TextAlign.center,
                          style: TextStyle(color: Colors.grey)))
                  : ListView.separated(
                      itemCount: ble.scanResults.length,
                      separatorBuilder: (_, __) => const Divider(height: 1),
                      itemBuilder: (_, i) {
                        final r = ble.scanResults[i];
                        return ListTile(
                          leading: const Icon(Icons.bluetooth,
                              color: Colors.blueAccent),
                          title: Text(r.device.platformName.isNotEmpty
                              ? r.device.platformName
                              : "Unknown Device"),
                          subtitle: Text(r.device.remoteId.toString(),
                              style: const TextStyle(fontSize: 12)),
                          trailing: Text("${r.rssi} dBm",
                              style: TextStyle(color: Colors.grey[400])),
                          onTap: () async {
                            Navigator.pop(ctx);
                            await ble.connectToDevice(r.device);
                          },
                        );
                      },
                    ),
            ),
          ],
        ),
      ),
    );
  }
}

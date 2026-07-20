import 'package:flutter/material.dart';
import 'package:provider/provider.dart';
import '../services/ble_service.dart';
import '../models/material_profile.dart';
import '../main.dart' show AnalyteXColors;
import 'chart_page.dart';

class HomePage extends StatefulWidget {
  const HomePage({super.key});

  @override
  State<HomePage> createState() => _HomePageState();
}

class _HomePageState extends State<HomePage> {
  // ── Material profile picker (materials-library-and-applications.md §6) ──
  // Default UX: pick a profile ("Lead & Cadmium (water)") instead of manually
  // configuring a scan. "Advanced" mode falls back to the original raw
  // parameter forms below, for the research/lab-instrument audience.
  List<MaterialProfile> _profiles = [];
  MaterialProfile? _selectedProfile;
  bool _advancedMode = false;
  bool _profilesLoading = true;

  @override
  void initState() {
    super.initState();
    MaterialProfile.loadBundled().then((profiles) {
      if (!mounted) return;
      setState(() {
        _profiles = profiles;
        _selectedProfile = profiles.isNotEmpty ? profiles.first : null;
        _profilesLoading = false;
      });
    }).catchError((e) {
      // Bundled asset failed to load — fall back to advanced/manual mode
      // rather than leaving the user stuck with a picker that has nothing
      // to pick from.
      if (!mounted) return;
      setState(() {
        _profilesLoading = false;
        _advancedMode = true;
      });
    });
  }

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
    final canStart = ble.isConnected && (_advancedMode || _selectedProfile != null);

    return Scaffold(
      appBar: AppBar(
        titleSpacing: 20,
        title: Row(
          mainAxisSize: MainAxisSize.min,
          children: [
            Container(
              width: 30,
              height: 30,
              decoration: BoxDecoration(
                shape: BoxShape.circle,
                gradient: const LinearGradient(
                  colors: [AnalyteXColors.teal, AnalyteXColors.tealDim],
                  begin: Alignment.topLeft,
                  end: Alignment.bottomRight,
                ),
              ),
              alignment: Alignment.center,
              child: const Icon(Icons.bolt, size: 18, color: AnalyteXColors.ink),
            ),
            const SizedBox(width: 10),
            const Text("AnalyteX",
                style: TextStyle(fontWeight: FontWeight.w800, letterSpacing: 0.5)),
          ],
        ),
        actions: [
          IconButton(
            icon: const Icon(Icons.search),
            tooltip: "Scan for devices",
            onPressed: () => _showDeviceScanner(context, ble),
          ),
          const SizedBox(width: 4),
        ],
      ),
      body: SafeArea(
        child: SingleChildScrollView(
          padding: const EdgeInsets.fromLTRB(16, 12, 16, 24),
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.stretch,
            children: [
              _connectionBanner(ble),
              const SizedBox(height: 20),

              // ── Mode toggle: material profile (default) vs. manual/advanced ──
              Row(
                children: [
                  Expanded(
                    child: Text(
                      _advancedMode ? "ADVANCED · MANUAL PARAMETERS" : "SELECT A TEST",
                      style: const TextStyle(
                        fontWeight: FontWeight.w700,
                        fontSize: 12,
                        letterSpacing: 1.0,
                        color: AnalyteXColors.textMuted,
                      ),
                    ),
                  ),
                  TextButton.icon(
                    icon: Icon(_advancedMode ? Icons.list_alt : Icons.tune, size: 16),
                    label: Text(_advancedMode ? "Use Profile" : "Advanced"),
                    onPressed: () => setState(() => _advancedMode = !_advancedMode),
                  ),
                ],
              ),
              const SizedBox(height: 10),

              if (!_advancedMode) ...[
                // ── Material profile picker ─────────────────────────
                // Default UX for the actual target users (food-safety
                // inspector, water-utility technician) — pick what you're
                // testing for, not how to configure a voltammetric sweep.
                // See materials-library-and-applications.md §6.
                if (_profilesLoading)
                  const Padding(
                    padding: EdgeInsets.symmetric(vertical: 32),
                    child: Center(child: CircularProgressIndicator(color: AnalyteXColors.teal)),
                  )
                else if (_profiles.isEmpty)
                  Card(
                    child: Padding(
                      padding: const EdgeInsets.all(16),
                      child: Row(
                        children: const [
                          Icon(Icons.info_outline, color: AnalyteXColors.textMuted),
                          SizedBox(width: 12),
                          Expanded(
                            child: Text(
                              "No material profiles available. Use Advanced mode to configure a scan manually.",
                              style: TextStyle(color: AnalyteXColors.textMuted, fontSize: 13),
                            ),
                          ),
                        ],
                      ),
                    ),
                  )
                else
                  _profileCard(),
              ] else ...[
                // ── Technique selector (manual/advanced) ────────────
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
              ],
              const SizedBox(height: 24),

              // ── Start button ────────────────────────────────────
              ElevatedButton.icon(
                icon: const Icon(Icons.play_arrow_rounded, size: 22),
                label: const Text("Start Measurement",
                    style: TextStyle(fontSize: 16, fontWeight: FontWeight.bold)),
                style: ElevatedButton.styleFrom(
                  minimumSize: const Size.fromHeight(54),
                ),
                onPressed: canStart ? () => _startMeasurement(ble) : null,
              ),
              if (!canStart) ...[
                const SizedBox(height: 10),
                Row(
                  mainAxisAlignment: MainAxisAlignment.center,
                  children: [
                    Icon(
                      ble.isConnected ? Icons.checklist : Icons.bluetooth_searching,
                      size: 15,
                      color: AnalyteXColors.textMuted,
                    ),
                    const SizedBox(width: 6),
                    Text(
                      ble.isConnected
                          ? "Select a test above to continue"
                          : "Connect a device to begin",
                      style: const TextStyle(color: AnalyteXColors.textMuted, fontSize: 13),
                    ),
                  ],
                ),
              ],

              const SizedBox(height: 32),
              _howItWorks(),
              const SizedBox(height: 28),
              Center(
                child: Text(
                  "VidyuthLabs Technologies Pvt Ltd",
                  style: TextStyle(
                    fontSize: 11,
                    letterSpacing: 0.5,
                    color: AnalyteXColors.textMuted.withValues(alpha: 0.6),
                  ),
                ),
              ),
            ],
          ),
        ),
      ),
    );
  }

  // ── Connection status banner ──────────────────────────────────
  // Replaces the old corner-chip: for a device sold to non-engineers,
  // "am I connected" needs to be the loudest thing on screen, not a
  // 12px label tucked into the AppBar.
  Widget _connectionBanner(BLEService ble) {
    final connected = ble.isConnected;
    return Container(
      padding: const EdgeInsets.all(14),
      decoration: BoxDecoration(
        color: connected
            ? AnalyteXColors.success.withValues(alpha: 0.12)
            : AnalyteXColors.surface,
        borderRadius: BorderRadius.circular(14),
        border: Border.all(
          color: connected
              ? AnalyteXColors.success.withValues(alpha: 0.4)
              : AnalyteXColors.outline,
        ),
      ),
      child: Row(
        children: [
          Container(
            width: 38,
            height: 38,
            decoration: BoxDecoration(
              shape: BoxShape.circle,
              color: connected
                  ? AnalyteXColors.success.withValues(alpha: 0.18)
                  : AnalyteXColors.surfaceRaised,
            ),
            alignment: Alignment.center,
            child: Icon(
              connected ? Icons.bluetooth_connected : Icons.bluetooth_disabled,
              size: 19,
              color: connected ? AnalyteXColors.success : AnalyteXColors.textMuted,
            ),
          ),
          const SizedBox(width: 12),
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Text(
                  connected ? "Device connected" : "No device connected",
                  style: const TextStyle(fontWeight: FontWeight.w700, fontSize: 14),
                ),
                Text(
                  connected
                      ? (ble.connectedDevice?.platformName.isNotEmpty == true
                          ? ble.connectedDevice!.platformName
                          : "AnalyteX")
                      : "Tap Connect to scan for nearby units",
                  style: const TextStyle(fontSize: 12, color: AnalyteXColors.textMuted),
                ),
              ],
            ),
          ),
          if (connected)
            TextButton(
              onPressed: () => ble.disconnect(),
              child: const Text("Disconnect"),
            )
          else
            ElevatedButton(
              style: ElevatedButton.styleFrom(
                minimumSize: const Size(0, 36),
                padding: const EdgeInsets.symmetric(horizontal: 16),
              ),
              onPressed: () => _showDeviceScanner(context, ble),
              child: const Text("Connect"),
            ),
        ],
      ),
    );
  }

  // ── Material profile picker card ──────────────────────────────
  Widget _profileCard() {
    return Column(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        DropdownButtonFormField<MaterialProfile>(
          value: _selectedProfile,
          isExpanded: true,
          decoration: const InputDecoration(
            labelText: "Test for",
            prefixIcon: Icon(Icons.science_outlined),
          ),
          items: _profiles
              .map((p) => DropdownMenuItem(
                    value: p,
                    child: Text(p.name, overflow: TextOverflow.ellipsis),
                  ))
              .toList(),
          onChanged: (p) => setState(() => _selectedProfile = p),
        ),
        if (_selectedProfile != null)
          Card(
            margin: const EdgeInsets.only(top: 10),
            child: Padding(
              padding: const EdgeInsets.all(14),
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Row(
                    children: [
                      Expanded(
                        child: Text(_selectedProfile!.material,
                            style: const TextStyle(fontWeight: FontWeight.w700, fontSize: 13)),
                      ),
                      Container(
                        padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 3),
                        decoration: BoxDecoration(
                          color: AnalyteXColors.teal.withValues(alpha: 0.15),
                          borderRadius: BorderRadius.circular(20),
                        ),
                        child: Text(
                          _selectedProfile!.method,
                          style: const TextStyle(
                            fontSize: 11,
                            fontWeight: FontWeight.w700,
                            color: AnalyteXColors.teal,
                          ),
                        ),
                      ),
                    ],
                  ),
                  const SizedBox(height: 6),
                  Text("Electrolyte: ${_selectedProfile!.electrolyte}",
                      style: const TextStyle(fontSize: 12, color: AnalyteXColors.textMuted)),
                  if (_selectedProfile!.notes.isNotEmpty) ...[
                    const SizedBox(height: 10),
                    Container(
                      padding: const EdgeInsets.all(10),
                      decoration: BoxDecoration(
                        color: AnalyteXColors.amber.withValues(alpha: 0.10),
                        borderRadius: BorderRadius.circular(10),
                        border: Border.all(color: AnalyteXColors.amber.withValues(alpha: 0.3)),
                      ),
                      child: Row(
                        crossAxisAlignment: CrossAxisAlignment.start,
                        children: [
                          const Icon(Icons.bookmark_outline, size: 15, color: AnalyteXColors.amber),
                          const SizedBox(width: 8),
                          Expanded(
                            child: Text(_selectedProfile!.notes,
                                style: const TextStyle(fontSize: 11.5, color: AnalyteXColors.amber, height: 1.3)),
                          ),
                        ],
                      ),
                    ),
                  ],
                ],
              ),
            ),
          ),
      ],
    );
  }

  // ── "How it works" strip ──────────────────────────────────────
  // Fills what used to be dead space below the fold with something a
  // first-time operator actually benefits from seeing.
  Widget _howItWorks() {
    final steps = [
      (Icons.bluetooth_searching, "Connect", "Pair with your AnalyteX device"),
      (Icons.science_outlined, "Pick a test", "Choose the material or analyte"),
      (Icons.show_chart, "Measure", "Run the scan and view live results"),
    ];
    return Row(
      children: [
        for (var i = 0; i < steps.length; i++) ...[
          Expanded(
            child: Column(
              children: [
                Container(
                  width: 44,
                  height: 44,
                  decoration: const BoxDecoration(
                    shape: BoxShape.circle,
                    color: AnalyteXColors.surfaceRaised,
                  ),
                  alignment: Alignment.center,
                  child: Icon(steps[i].$1, size: 20, color: AnalyteXColors.teal),
                ),
                const SizedBox(height: 8),
                Text(steps[i].$2,
                    style: const TextStyle(fontWeight: FontWeight.w700, fontSize: 12.5)),
                const SizedBox(height: 2),
                Text(steps[i].$3,
                    textAlign: TextAlign.center,
                    style: const TextStyle(fontSize: 10.5, color: AnalyteXColors.textMuted)),
              ],
            ),
          ),
          if (i != steps.length - 1)
            Padding(
              padding: const EdgeInsets.only(bottom: 32),
              child: Icon(Icons.chevron_right, size: 16, color: AnalyteXColors.outline),
            ),
        ],
      ],
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
    _field(_caInt,  "Sample Interval (s)",  "0.01 to 60"),
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
      final Map<String, dynamic> cmd;
      final String techniqueName;
      if (!_advancedMode && _selectedProfile != null) {
        cmd = _selectedProfile!.toCommand();
        techniqueName = _selectedProfile!.method;
      } else {
        cmd = _buildCommand();
        techniqueName = selectedMethod;
      }
      ble.sendCommand(cmd);
      Navigator.push(
        context,
        MaterialPageRoute(
          builder: (_) => ChartPage(techniqueName: techniqueName),
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
            "interval":     _parseDouble(_caInt,  "Interval",     0.01, 60.0),
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
        backgroundColor: AnalyteXColors.danger,
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
      backgroundColor: AnalyteXColors.surface,
      shape: const RoundedRectangleBorder(
          borderRadius: BorderRadius.vertical(top: Radius.circular(20))),
      builder: (ctx) => SizedBox(
        height: MediaQuery.of(ctx).size.height * 0.6,
        child: Consumer<BLEService>(
          builder: (ctx, ble, _) => Column(
            children: [
              const SizedBox(height: 12),
              Container(width: 40, height: 4,
                  decoration: BoxDecoration(
                      color: AnalyteXColors.outline,
                      borderRadius: BorderRadius.circular(2))),
              const SizedBox(height: 14),
              const Text("Scan & Connect",
                  style: TextStyle(fontSize: 17, fontWeight: FontWeight.w700)),
              if (ble.isScanning)
                const Padding(
                  padding: EdgeInsets.symmetric(horizontal: 16, vertical: 10),
                  child: LinearProgressIndicator(color: AnalyteXColors.teal),
                )
              else
                const SizedBox(height: 10),
              Expanded(
                child: ble.scanResults.isEmpty
                    ? const Center(
                        child: Padding(
                          padding: EdgeInsets.all(24),
                          child: Text("No devices found.\nMake sure firmware is running.",
                              textAlign: TextAlign.center,
                              style: TextStyle(color: AnalyteXColors.textMuted)),
                        ),
                      )
                    : ListView.separated(
                        padding: const EdgeInsets.symmetric(horizontal: 8),
                        itemCount: ble.scanResults.length,
                        separatorBuilder: (_, __) => const Divider(height: 1, color: AnalyteXColors.outline),
                        itemBuilder: (_, i) {
                          final r = ble.scanResults[i];
                          return ListTile(
                            leading: const Icon(Icons.bluetooth,
                                color: AnalyteXColors.teal),
                            title: Text(r.device.platformName.isNotEmpty
                                ? r.device.platformName
                                : "Unknown Device"),
                            subtitle: Text(r.device.remoteId.toString(),
                                style: const TextStyle(fontSize: 12)),
                            trailing: Text("${r.rssi} dBm",
                                style: const TextStyle(color: AnalyteXColors.textMuted)),
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
      ),
    );
  }
}

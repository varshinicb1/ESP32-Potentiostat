import 'package:flutter/material.dart';
import 'package:provider/provider.dart';
import '../services/ble_service.dart';
import '../models/material_profile.dart';
import '../main.dart' show AX, GraticuleBackground, SignalTrace;
import 'chart_page.dart';

// Small shared bits for the instrument look.
const _eyebrow = TextStyle(
  fontFamily: 'PlexMono',
  fontSize: 11,
  fontWeight: FontWeight.w500,
  letterSpacing: 2.0,
  color: AX.textMid,
);

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
      body: GraticuleBackground(
        child: SafeArea(
          child: CustomScrollView(
            slivers: [
              SliverToBoxAdapter(child: _hero(ble)),
              SliverToBoxAdapter(
                child: Padding(
                  padding: const EdgeInsets.fromLTRB(18, 4, 18, 28),
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.stretch,
                    children: [
                      _connectionStrip(ble),
                      const SizedBox(height: 26),

                      // ── Mode toggle ──────────────────────────────
                      Row(
                        children: [
                          Text(
                            _advancedMode ? "MANUAL PARAMETERS" : "SELECT A TEST",
                            style: _eyebrow,
                          ),
                          const Spacer(),
                          TextButton.icon(
                            icon: Icon(_advancedMode ? Icons.list_alt : Icons.tune, size: 15),
                            label: Text(_advancedMode ? "PROFILES" : "ADVANCED"),
                            onPressed: () => setState(() => _advancedMode = !_advancedMode),
                          ),
                        ],
                      ),
                      const SizedBox(height: 12),

                      if (!_advancedMode) ...[
                        if (_profilesLoading)
                          const Padding(
                            padding: EdgeInsets.symmetric(vertical: 32),
                            child: Center(child: CircularProgressIndicator(color: AX.signal)),
                          )
                        else if (_profiles.isEmpty)
                          Card(
                            child: Padding(
                              padding: const EdgeInsets.all(16),
                              child: Row(
                                children: const [
                                  Icon(Icons.info_outline, color: AX.textMid),
                                  SizedBox(width: 12),
                                  Expanded(
                                    child: Text(
                                      "No material profiles available. Use Advanced mode to configure a scan manually.",
                                      style: TextStyle(color: AX.textMid, fontSize: 13),
                                    ),
                                  ),
                                ],
                              ),
                            ),
                          )
                        else
                          _profileCard(),
                      ] else ...[
                        DropdownButtonFormField<String>(
                          value: selectedMethod,
                          decoration: const InputDecoration(
                            labelText: "METHOD",
                            prefixIcon: Icon(Icons.science_outlined, color: AX.textMid),
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
                        AnimatedSwitcher(
                          duration: const Duration(milliseconds: 200),
                          child: KeyedSubtree(
                            key: ValueKey(selectedMethod),
                            child: _buildForm(),
                          ),
                        ),
                      ],
                      const SizedBox(height: 26),

                      // ── Start (arm) button ───────────────────────
                      _startButton(ble, canStart),
                      if (!canStart) ...[
                        const SizedBox(height: 12),
                        Center(
                          child: Text(
                            ble.isConnected
                                ? "▸ SELECT A TEST TO CONTINUE"
                                : "▸ CONNECT A DEVICE TO BEGIN",
                            style: const TextStyle(
                              fontFamily: 'PlexMono',
                              fontSize: 11,
                              letterSpacing: 1.5,
                              color: AX.textLo,
                            ),
                          ),
                        ),
                      ],

                      const SizedBox(height: 34),
                      _workflowStrip(),
                      const SizedBox(height: 26),
                      const _FooterMark(),
                    ],
                  ),
                ),
              ),
            ],
          ),
        ),
      ),
    );
  }

  // ── Hero: wordmark + live signal trace + scan action ──────────
  Widget _hero(BLEService ble) {
    return Container(
      padding: const EdgeInsets.fromLTRB(18, 14, 18, 18),
      decoration: const BoxDecoration(
        border: Border(bottom: BorderSide(color: AX.hairline)),
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Row(
            children: [
              // Instrument mark: a bracketed "AX" in mono, like a device label
              Container(
                padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 4),
                decoration: BoxDecoration(
                  border: Border.all(color: AX.signal, width: 1.5),
                  borderRadius: BorderRadius.circular(3),
                ),
                child: const Text(
                  "AX",
                  style: TextStyle(
                    fontFamily: 'PlexMono',
                    fontWeight: FontWeight.w600,
                    fontSize: 15,
                    height: 1.0,
                    color: AX.signal,
                  ),
                ),
              ),
              const SizedBox(width: 12),
              Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                mainAxisSize: MainAxisSize.min,
                children: const [
                  Text("AnalyteX",
                      style: TextStyle(fontSize: 20, fontWeight: FontWeight.w700, letterSpacing: -0.3, height: 1.05)),
                  Text("ELECTROCHEMICAL ANALYZER",
                      style: TextStyle(fontFamily: 'PlexMono', fontSize: 9.5, letterSpacing: 1.8, color: AX.textLo)),
                ],
              ),
              const Spacer(),
              IconButton(
                onPressed: () => _showDeviceScanner(context, ble),
                icon: const Icon(Icons.wifi_tethering, color: AX.textMid),
                tooltip: "Scan for devices",
              ),
            ],
          ),
          const SizedBox(height: 12),
          // The signature: a faint live trace, like an idle scope screen
          Opacity(opacity: 0.9, child: SignalTrace(height: 30, seed: 11)),
        ],
      ),
    );
  }

  // ── Big arm/start button with a mono command label ────────────
  Widget _startButton(BLEService ble, bool canStart) {
    return SizedBox(
      height: 56,
      child: ElevatedButton(
        onPressed: canStart ? () => _startMeasurement(ble) : null,
        child: Row(
          mainAxisAlignment: MainAxisAlignment.center,
          children: const [
            Icon(Icons.play_arrow_rounded, size: 22),
            SizedBox(width: 8),
            Text("START MEASUREMENT",
                style: TextStyle(fontSize: 15, fontWeight: FontWeight.w700, letterSpacing: 0.5)),
          ],
        ),
      ),
    );
  }

  // ── Connection status strip — reads like an instrument link readout ──
  Widget _connectionStrip(BLEService ble) {
    final connected = ble.isConnected;
    final accent = connected ? AX.signal : AX.textLo;
    final name = connected
        ? (ble.connectedDevice?.platformName.isNotEmpty == true
            ? ble.connectedDevice!.platformName
            : "AnalyteX")
        : "NO LINK";
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 14, vertical: 12),
      decoration: BoxDecoration(
        color: AX.panel,
        borderRadius: BorderRadius.circular(4),
        border: Border.all(color: connected ? AX.signal.withValues(alpha: 0.45) : AX.hairline),
      ),
      child: Row(
        children: [
          // pulsing/steady status dot
          Container(
            width: 9,
            height: 9,
            decoration: BoxDecoration(
              shape: BoxShape.circle,
              color: accent,
              boxShadow: connected
                  ? [BoxShadow(color: AX.signal.withValues(alpha: 0.6), blurRadius: 8, spreadRadius: 1)]
                  : null,
            ),
          ),
          const SizedBox(width: 12),
          Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            mainAxisSize: MainAxisSize.min,
            children: [
              Text("LINK",
                  style: _eyebrow.copyWith(color: accent, fontSize: 10, letterSpacing: 2.5)),
              const SizedBox(height: 2),
              Text(name,
                  style: TextStyle(
                    fontFamily: 'PlexMono',
                    fontSize: 14,
                    fontWeight: FontWeight.w500,
                    color: connected ? AX.textHi : AX.textMid,
                  )),
            ],
          ),
          const Spacer(),
          if (connected)
            TextButton(onPressed: () => ble.disconnect(), child: const Text("DISCONNECT"))
          else
            ElevatedButton(
              style: ElevatedButton.styleFrom(
                minimumSize: const Size(0, 38),
                padding: const EdgeInsets.symmetric(horizontal: 18),
              ),
              onPressed: () => _showDeviceScanner(context, ble),
              child: const Text("CONNECT", style: TextStyle(letterSpacing: 0.5)),
            ),
        ],
      ),
    );
  }

  // ── Material profile picker + technical spec card ─────────────
  Widget _profileCard() {
    return Column(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        DropdownButtonFormField<MaterialProfile>(
          value: _selectedProfile,
          isExpanded: true,
          decoration: const InputDecoration(
            labelText: "TEST FOR",
            prefixIcon: Icon(Icons.biotech_outlined, color: AX.textMid),
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
          Container(
            margin: const EdgeInsets.only(top: 10),
            decoration: BoxDecoration(
              color: AX.panel,
              borderRadius: BorderRadius.circular(4),
              border: Border.all(color: AX.hairline),
            ),
            child: Column(
              children: [
                // method badge header
                Container(
                  padding: const EdgeInsets.fromLTRB(14, 12, 12, 12),
                  decoration: const BoxDecoration(
                    border: Border(bottom: BorderSide(color: AX.hairline)),
                  ),
                  child: Row(
                    children: [
                      Expanded(
                        child: Text(_selectedProfile!.material,
                            style: const TextStyle(fontWeight: FontWeight.w600, fontSize: 14)),
                      ),
                      Container(
                        padding: const EdgeInsets.symmetric(horizontal: 9, vertical: 4),
                        decoration: BoxDecoration(
                          color: AX.signal.withValues(alpha: 0.14),
                          borderRadius: BorderRadius.circular(3),
                          border: Border.all(color: AX.signal.withValues(alpha: 0.5)),
                        ),
                        child: Text(_selectedProfile!.method,
                            style: const TextStyle(
                              fontFamily: 'PlexMono',
                              fontSize: 11,
                              fontWeight: FontWeight.w600,
                              letterSpacing: 1,
                              color: AX.signal,
                            )),
                      ),
                    ],
                  ),
                ),
                // spec rows (mono key/value like a datasheet)
                Padding(
                  padding: const EdgeInsets.fromLTRB(14, 10, 14, 12),
                  child: Column(
                    children: [
                      _specRow("ELECTROLYTE", _selectedProfile!.electrolyte),
                      if (_selectedProfile!.notes.isNotEmpty) ...[
                        const SizedBox(height: 12),
                        Container(
                          padding: const EdgeInsets.all(11),
                          decoration: BoxDecoration(
                            color: AX.amber.withValues(alpha: 0.08),
                            borderRadius: BorderRadius.circular(3),
                            border: Border(left: BorderSide(color: AX.amber, width: 2)),
                          ),
                          child: Row(
                            crossAxisAlignment: CrossAxisAlignment.start,
                            children: [
                              const Icon(Icons.menu_book_outlined, size: 14, color: AX.amber),
                              const SizedBox(width: 8),
                              Expanded(
                                child: Text(_selectedProfile!.notes,
                                    style: const TextStyle(
                                        fontSize: 11.5, color: AX.amber, height: 1.35)),
                              ),
                            ],
                          ),
                        ),
                      ],
                    ],
                  ),
                ),
              ],
            ),
          ),
      ],
    );
  }

  Widget _specRow(String k, String v) {
    return Row(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        SizedBox(
          width: 92,
          child: Text(k, style: _eyebrow.copyWith(fontSize: 10, letterSpacing: 1.2)),
        ),
        Expanded(
          child: Text(v, style: const TextStyle(fontSize: 12.5, color: AX.textHi, height: 1.3)),
        ),
      ],
    );
  }

  // ── Workflow strip: CONNECT › SELECT › MEASURE, technical style ──
  Widget _workflowStrip() {
    final steps = [
      ("01", "CONNECT", "Pair the device"),
      ("02", "SELECT", "Choose the analyte"),
      ("03", "MEASURE", "Run & read live"),
    ];
    return Row(
      children: [
        for (var i = 0; i < steps.length; i++) ...[
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Text(steps[i].$1,
                    style: const TextStyle(
                      fontFamily: 'PlexMono',
                      fontSize: 12,
                      fontWeight: FontWeight.w600,
                      color: AX.signal,
                    )),
                const SizedBox(height: 6),
                Container(height: 1, color: AX.hairline),
                const SizedBox(height: 8),
                Text(steps[i].$2,
                    style: const TextStyle(
                        fontFamily: 'PlexMono', fontSize: 11.5, fontWeight: FontWeight.w600, letterSpacing: 0.8)),
                const SizedBox(height: 2),
                Text(steps[i].$3, style: const TextStyle(fontSize: 10.5, color: AX.textLo, height: 1.3)),
              ],
            ),
          ),
          if (i != steps.length - 1) const SizedBox(width: 14),
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

  Widget _formCard(String title, List<Widget> fields) => Container(
    decoration: BoxDecoration(
      color: AX.panel,
      borderRadius: BorderRadius.circular(4),
      border: Border.all(color: AX.hairline),
    ),
    child: Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        Container(
          width: double.infinity,
          padding: const EdgeInsets.fromLTRB(14, 11, 14, 11),
          decoration: const BoxDecoration(
            border: Border(bottom: BorderSide(color: AX.hairline)),
          ),
          child: Text(title.toUpperCase(),
              style: _eyebrow.copyWith(color: AX.textHi, letterSpacing: 1.2)),
        ),
        Padding(
          padding: const EdgeInsets.fromLTRB(14, 14, 14, 6),
          child: Column(
            children: fields.map((f) => Padding(
              padding: const EdgeInsets.only(bottom: 12),
              child: f,
            )).toList(),
          ),
        ),
      ],
    ),
  );

  Widget _field(TextEditingController ctrl, String label, String hint) =>
      TextField(
        controller: ctrl,
        style: const TextStyle(fontFamily: 'PlexMono', fontSize: 15, color: AX.textHi),
        keyboardType: const TextInputType.numberWithOptions(
            signed: true, decimal: true),
        decoration: InputDecoration(
          labelText: label.toUpperCase(),
          isDense: true,
          hintText: hint,
          hintStyle: const TextStyle(fontFamily: 'PlexMono', fontSize: 12, color: AX.textLo),
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
        backgroundColor: AX.danger,
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
      backgroundColor: AX.surface,
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
                      color: AX.outline,
                      borderRadius: BorderRadius.circular(2))),
              const SizedBox(height: 16),
              Row(
                mainAxisAlignment: MainAxisAlignment.center,
                children: const [
                  Icon(Icons.wifi_tethering, size: 16, color: AX.signal),
                  SizedBox(width: 8),
                  Text("NEARBY DEVICES",
                      style: TextStyle(
                        fontFamily: 'PlexMono',
                        fontSize: 13,
                        fontWeight: FontWeight.w600,
                        letterSpacing: 1.5,
                      )),
                ],
              ),
              if (ble.isScanning)
                const Padding(
                  padding: EdgeInsets.symmetric(horizontal: 16, vertical: 12),
                  child: LinearProgressIndicator(color: AX.signal, minHeight: 2),
                )
              else
                const SizedBox(height: 14),
              Expanded(
                child: ble.scanResults.isEmpty
                    ? const Center(
                        child: Padding(
                          padding: EdgeInsets.all(24),
                          child: Text("No devices found.\nMake sure firmware is running.",
                              textAlign: TextAlign.center,
                              style: TextStyle(color: AX.textMuted)),
                        ),
                      )
                    : ListView.separated(
                        padding: const EdgeInsets.symmetric(horizontal: 8),
                        itemCount: ble.scanResults.length,
                        separatorBuilder: (_, __) => const Divider(height: 1, color: AX.outline),
                        itemBuilder: (_, i) {
                          final r = ble.scanResults[i];
                          return ListTile(
                            leading: const Icon(Icons.bluetooth,
                                color: AX.teal),
                            title: Text(r.device.platformName.isNotEmpty
                                ? r.device.platformName
                                : "Unknown Device"),
                            subtitle: Text(r.device.remoteId.toString(),
                                style: const TextStyle(fontFamily: 'PlexMono', fontSize: 11, color: AX.textLo)),
                            trailing: Text("${r.rssi} dBm",
                                style: const TextStyle(fontFamily: 'PlexMono', fontSize: 12, color: AX.signal)),
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

// ── Footer brand mark ─────────────────────────────────────────
class _FooterMark extends StatelessWidget {
  const _FooterMark();

  @override
  Widget build(BuildContext context) {
    return Column(
      children: [
        Container(height: 1, color: AX.hairline),
        const SizedBox(height: 14),
        Text(
          "VIDYUTHLABS TECHNOLOGIES PVT LTD",
          style: TextStyle(
            fontFamily: 'PlexMono',
            fontSize: 9.5,
            letterSpacing: 1.5,
            color: AX.textLo.withValues(alpha: 0.8),
          ),
        ),
      ],
    );
  }
}

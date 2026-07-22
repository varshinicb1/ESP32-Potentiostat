import 'dart:async';
import 'package:flutter/material.dart';
import 'package:provider/provider.dart';
import 'package:fl_chart/fl_chart.dart';
import '../services/ble_service.dart';
import '../services/csv_service.dart';
import '../main.dart' show AX, GraticuleBackground;

class ChartPage extends StatefulWidget {
  final String techniqueName;
  const ChartPage({super.key, required this.techniqueName});

  @override
  State<ChartPage> createState() => _ChartPageState();
}

class _ChartPageState extends State<ChartPage> {
  // Maximum data points to keep in memory (prevents OOM on long experiments)
  static const int _maxPoints = 5000;

  List<Map<String, dynamic>> rawData   = [];
  List<FlSpot>               plotSpots = [];
  StreamSubscription?        _dataSubscription;
  bool                       _measurementDone = false;

  @override
  void initState() {
    super.initState();
    final ble = Provider.of<BLEService>(context, listen: false);

    _dataSubscription = ble.dataStream.listen((data) {
      final type   = data["type"]   as String?;
      final status = data["status"] as String?;

      if (type == "data" || type == "eis_data") {
        setState(() {
          rawData.add(data);

          FlSpot? spot;
          if (widget.techniqueName == "CV") {
            // I (µA) vs V (V)
            spot = FlSpot(
              (data["voltage"] as num?)?.toDouble() ?? 0.0,
              (data["current"] as num?)?.toDouble() ?? 0.0,
            );
          } else if (widget.techniqueName == "CA") {
            // I (µA) vs t (s)
            spot = FlSpot(
              (data["time"] as num?)?.toDouble() ?? 0.0,
              (data["current"] as num?)?.toDouble() ?? 0.0,
            );
          } else if (widget.techniqueName == "SWV") {
            // ΔI (µA) vs V (V)
            spot = FlSpot(
              (data["voltage"]     as num?)?.toDouble() ?? 0.0,
              (data["diffCurrent"] as num?)?.toDouble() ?? 0.0,
            );
          } else if (widget.techniqueName == "EIS") {
            // Nyquist: -Z'' vs Z'
            final realZ = (data["realZ"] as num?)?.toDouble() ?? 0.0;
            final imagZ = (data["imagZ"] as num?)?.toDouble() ?? 0.0;
            spot = FlSpot(realZ, -imagZ);
          }

          if (spot != null) {
            plotSpots.add(spot);
            // Downsample: keep only the last _maxPoints to prevent OOM
            if (plotSpots.length > _maxPoints) {
              // Remove from the front (oldest)
              plotSpots.removeRange(0, plotSpots.length - _maxPoints);
            }
          }
        });
      } else if (status == "idle") {
        // Measurement finished
        setState(() => _measurementDone = true);
        _dataSubscription?.cancel();
        if (mounted) {
          ScaffoldMessenger.of(context).showSnackBar(
            SnackBar(
              content: Row(children: const [
                Icon(Icons.check_circle, color: Colors.white),
                SizedBox(width: 8),
                Text("Measurement completed successfully"),
              ]),
              backgroundColor: AX.success,
              duration: const Duration(seconds: 3),
            ),
          );
        }
      } else if (status == "error") {
        final detail = data["detail"] as String? ?? "Unknown error";
        if (mounted) {
          ScaffoldMessenger.of(context).showSnackBar(
            SnackBar(
              content: Text("Device error: $detail"),
              backgroundColor: AX.danger,
              duration: const Duration(seconds: 5),
            ),
          );
        }
      } else if (status == "aborting") {
        setState(() => _measurementDone = true);
        _dataSubscription?.cancel();
      }
    });
  }

  @override
  void dispose() {
    _dataSubscription?.cancel();
    super.dispose();
  }

  // ──────────────────────────────────────────────────────────────
  // Axis labels based on technique
  // ──────────────────────────────────────────────────────────────
  String get _xAxisLabel {
    switch (widget.techniqueName) {
      case "CV":  return "Voltage (V)";
      case "CA":  return "Time (s)";
      case "SWV": return "Voltage (V)";
      case "EIS": return "Z' (Ω)";
      default:    return "X";
    }
  }

  String get _yAxisLabel {
    switch (widget.techniqueName) {
      case "CV":  return "Current (A)";
      case "CA":  return "Current (A)";
      case "SWV": return "ΔI (A)";
      case "EIS": return "-Z'' (Ω)";
      default:    return "Y";
    }
  }

  // ──────────────────────────────────────────────────────────────
  void _exportData() async {
    try {
      final filename =
          "${widget.techniqueName}_${DateTime.now().millisecondsSinceEpoch}";
      if (widget.techniqueName == "EIS") {
        await CSVService.exportEIS(filename, rawData);
      } else {
        await CSVService.exportVoltammetry(filename, rawData);
      }
    } catch (e) {
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(content: Text("Export failed: $e"),
              backgroundColor: AX.danger),
        );
      }
    }
  }

  void _sendAbort() {
    final ble = Provider.of<BLEService>(context, listen: false);
    // Firmware handles "ABORT" command — NOT "STOP"
    ble.sendCommand({"method": "ABORT"});
  }

  // ──────────────────────────────────────────────────────────────
  @override
  Widget build(BuildContext context) {
    final spots = plotSpots.isEmpty ? [const FlSpot(0, 0)] : plotSpots;
    final running = !_measurementDone;
    final accent = running ? AX.signal : AX.ok;

    return Scaffold(
      body: GraticuleBackground(
        child: SafeArea(
          child: Padding(
            padding: const EdgeInsets.fromLTRB(16, 8, 16, 16),
            child: Column(
              children: [
                // ── Header: back / technique / status ──────────────
                Row(
                  children: [
                    IconButton(
                      onPressed: () => Navigator.pop(context),
                      icon: const Icon(Icons.arrow_back, color: AX.textMid),
                      padding: EdgeInsets.zero,
                      constraints: const BoxConstraints(),
                    ),
                    const SizedBox(width: 12),
                    Column(
                      crossAxisAlignment: CrossAxisAlignment.start,
                      children: [
                        Text(widget.techniqueName,
                            style: const TextStyle(
                                fontFamily: 'PlexMono',
                                fontSize: 18,
                                fontWeight: FontWeight.w600,
                                letterSpacing: 1)),
                        const Text("LIVE ACQUISITION",
                            style: TextStyle(
                                fontFamily: 'PlexMono',
                                fontSize: 9.5,
                                letterSpacing: 1.8,
                                color: AX.textLo)),
                      ],
                    ),
                    const Spacer(),
                    _statusPill(accent),
                  ],
                ),
                const SizedBox(height: 14),

                // ── Readout bar ────────────────────────────────────
                Container(
                  padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 8),
                  decoration: BoxDecoration(
                    color: AX.panel,
                    borderRadius: const BorderRadius.vertical(top: Radius.circular(4)),
                    border: Border.all(color: AX.hairline),
                  ),
                  child: Row(
                    children: [
                      _readout("PTS", "${rawData.length}"),
                      const SizedBox(width: 20),
                      _readout("Y", _yAxisLabel),
                      const SizedBox(width: 20),
                      _readout("X", _xAxisLabel),
                      const Spacer(),
                      if (plotSpots.length >= _maxPoints)
                        const Text("◂ LAST 5000",
                            style: TextStyle(
                                fontFamily: 'PlexMono', fontSize: 10, color: AX.amber)),
                    ],
                  ),
                ),

                // ── Scope screen ───────────────────────────────────
                Expanded(
                  child: Container(
                    padding: const EdgeInsets.fromLTRB(6, 14, 14, 6),
                    decoration: BoxDecoration(
                      color: const Color(0xFF0A100C),
                      borderRadius: const BorderRadius.vertical(bottom: Radius.circular(4)),
                      border: Border.all(color: AX.hairline),
                    ),
                    child: LineChart(
                      LineChartData(
                        lineBarsData: [
                          LineChartBarData(
                            spots: spots,
                            isCurved: false,
                            barWidth: 1.6,
                            color: AX.signal,
                            dotData: const FlDotData(show: false),
                            belowBarData: BarAreaData(
                              show: true,
                              gradient: LinearGradient(
                                begin: Alignment.topCenter,
                                end: Alignment.bottomCenter,
                                colors: [
                                  AX.signal.withValues(alpha: 0.18),
                                  AX.signal.withValues(alpha: 0.0),
                                ],
                              ),
                            ),
                          ),
                        ],
                        titlesData: FlTitlesData(
                          topTitles: const AxisTitles(sideTitles: SideTitles(showTitles: false)),
                          rightTitles: const AxisTitles(sideTitles: SideTitles(showTitles: false)),
                          leftTitles: const AxisTitles(
                            sideTitles: SideTitles(
                                showTitles: true, reservedSize: 42,
                                getTitlesWidget: _monoAxis),
                          ),
                          bottomTitles: const AxisTitles(
                            sideTitles: SideTitles(
                                showTitles: true, reservedSize: 26,
                                getTitlesWidget: _monoAxis),
                          ),
                        ),
                        gridData: FlGridData(
                          show: true,
                          getDrawingHorizontalLine: (v) =>
                              const FlLine(color: Color(0xFF16241C), strokeWidth: 1),
                          getDrawingVerticalLine: (v) =>
                              const FlLine(color: Color(0xFF16241C), strokeWidth: 1),
                        ),
                        borderData: FlBorderData(
                          show: true,
                          border: Border.all(color: AX.hairline),
                        ),
                      ),
                    ),
                  ),
                ),
                const SizedBox(height: 16),

                // ── Actions ────────────────────────────────────────
                Row(
                  children: [
                    Expanded(
                      child: OutlinedButton.icon(
                        icon: const Icon(Icons.stop, size: 18),
                        label: const Text("ABORT", style: TextStyle(letterSpacing: 1)),
                        style: OutlinedButton.styleFrom(
                          foregroundColor: AX.danger,
                          side: BorderSide(color: AX.danger.withValues(alpha: 0.6)),
                          minimumSize: const Size.fromHeight(50),
                          shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(4)),
                          textStyle: const TextStyle(fontFamily: 'PlexSans', fontWeight: FontWeight.w700),
                        ),
                        onPressed: _measurementDone ? null : () {
                          _sendAbort();
                          Navigator.pop(context);
                        },
                      ),
                    ),
                    const SizedBox(width: 12),
                    Expanded(
                      child: ElevatedButton.icon(
                        icon: const Icon(Icons.download, size: 18),
                        label: const Text("EXPORT CSV", style: TextStyle(letterSpacing: 0.5)),
                        style: ElevatedButton.styleFrom(minimumSize: const Size.fromHeight(50)),
                        onPressed: rawData.isNotEmpty ? _exportData : null,
                      ),
                    ),
                  ],
                ),
              ],
            ),
          ),
        ),
      ),
    );
  }

  Widget _statusPill(Color accent) {
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 11, vertical: 6),
      decoration: BoxDecoration(
        color: accent.withValues(alpha: 0.12),
        borderRadius: BorderRadius.circular(3),
        border: Border.all(color: accent.withValues(alpha: 0.5)),
      ),
      child: Row(
        mainAxisSize: MainAxisSize.min,
        children: [
          Container(
            width: 7, height: 7,
            decoration: BoxDecoration(
              shape: BoxShape.circle,
              color: accent,
              boxShadow: [BoxShadow(color: accent.withValues(alpha: 0.6), blurRadius: 6)],
            ),
          ),
          const SizedBox(width: 7),
          Text(_measurementDone ? "DONE" : "REC",
              style: TextStyle(
                  fontFamily: 'PlexMono',
                  fontSize: 11,
                  fontWeight: FontWeight.w600,
                  letterSpacing: 1,
                  color: accent)),
        ],
      ),
    );
  }

  Widget _readout(String k, String v) {
    return Row(
      mainAxisSize: MainAxisSize.min,
      children: [
        Text("$k ",
            style: const TextStyle(fontFamily: 'PlexMono', fontSize: 10, color: AX.textLo, letterSpacing: 1)),
        Text(v,
            style: const TextStyle(fontFamily: 'PlexMono', fontSize: 12.5, color: AX.signal, fontWeight: FontWeight.w500)),
      ],
    );
  }

  static Widget _monoAxis(double value, TitleMeta meta) {
    if (value == meta.min || value == meta.max) return const SizedBox.shrink();
    return Text(
      value.abs() >= 1000 ? "${(value / 1000).toStringAsFixed(0)}k" : value.toStringAsFixed(1),
      style: const TextStyle(fontFamily: 'PlexMono', fontSize: 9, color: AX.textLo),
    );
  }
}

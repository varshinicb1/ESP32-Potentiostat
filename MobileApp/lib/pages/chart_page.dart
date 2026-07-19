import 'dart:async';
import 'package:flutter/material.dart';
import 'package:provider/provider.dart';
import 'package:fl_chart/fl_chart.dart';
import '../services/ble_service.dart';
import '../services/csv_service.dart';

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
              backgroundColor: Colors.green[700],
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
              backgroundColor: Colors.red[700],
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
              backgroundColor: Colors.red[700]),
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

    return Scaffold(
      appBar: AppBar(
        title: Text("${widget.techniqueName} — Live Plot"),
        actions: [
          if (rawData.isNotEmpty)
            IconButton(
              icon: const Icon(Icons.share),
              tooltip: "Export CSV",
              onPressed: _exportData,
            ),
        ],
      ),
      body: Padding(
        padding: const EdgeInsets.all(16),
        child: Column(
          children: [
            // Status row
            Row(
              mainAxisAlignment: MainAxisAlignment.spaceBetween,
              children: [
                Text("Points: ${rawData.length}",
                    style: const TextStyle(fontSize: 14)),
                if (plotSpots.length >= _maxPoints)
                  const Text("⚠ Showing last 5000 pts",
                      style: TextStyle(fontSize: 12, color: Colors.orange)),
                Chip(
                  label: Text(_measurementDone ? "Completed" : "Running",
                      style: const TextStyle(fontSize: 12)),
                  backgroundColor:
                      _measurementDone ? Colors.green[800] : Colors.blue[800],
                ),
              ],
            ),
            const SizedBox(height: 12),

            // Chart
            Expanded(
              child: Card(
                elevation: 4,
                child: Padding(
                  padding: const EdgeInsets.fromLTRB(8, 16, 16, 8),
                  child: LineChart(
                    LineChartData(
                      lineBarsData: [
                        LineChartBarData(
                          spots: spots,
                          isCurved: false,
                          barWidth: 1.5,
                          color: Theme.of(context).primaryColor,
                          dotData: const FlDotData(show: false),
                        ),
                      ],
                      titlesData: FlTitlesData(
                        topTitles: const AxisTitles(
                            sideTitles: SideTitles(showTitles: false)),
                        rightTitles: const AxisTitles(
                            sideTitles: SideTitles(showTitles: false)),
                        leftTitles: AxisTitles(
                          axisNameWidget: RotatedBox(
                            quarterTurns: 3,
                            child: Text(_yAxisLabel,
                                style: const TextStyle(fontSize: 11)),
                          ),
                          sideTitles: const SideTitles(
                              showTitles: true, reservedSize: 44),
                        ),
                        bottomTitles: AxisTitles(
                          axisNameWidget: Text(_xAxisLabel,
                              style: const TextStyle(fontSize: 11)),
                          sideTitles: const SideTitles(
                              showTitles: true, reservedSize: 28),
                        ),
                      ),
                      gridData: const FlGridData(show: true),
                      borderData: FlBorderData(show: true),
                    ),
                  ),
                ),
              ),
            ),
            const SizedBox(height: 16),

            // Action buttons
            Row(
              children: [
                Expanded(
                  child: ElevatedButton.icon(
                    icon: const Icon(Icons.stop),
                    label: const Text("Abort"),
                    style: ElevatedButton.styleFrom(
                      backgroundColor: Colors.red[700],
                      foregroundColor: Colors.white,
                      minimumSize: const Size.fromHeight(48),
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
                    icon: const Icon(Icons.download),
                    label: const Text("Export CSV"),
                    style: ElevatedButton.styleFrom(
                      minimumSize: const Size.fromHeight(48),
                    ),
                    onPressed: rawData.isNotEmpty ? _exportData : null,
                  ),
                ),
              ],
            ),
          ],
        ),
      ),
    );
  }
}

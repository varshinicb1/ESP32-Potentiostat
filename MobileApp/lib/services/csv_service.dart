import 'dart:io';
import 'package:path_provider/path_provider.dart';
import 'package:share_plus/share_plus.dart';

class CSVService {
  static Future<void> exportVoltammetry(String filename, List<Map<String, dynamic>> dataPoints) async {
    StringBuffer csv = StringBuffer();
    csv.writeln("Time (s),Voltage (V),Current (A),DiffCurrent (A)");
    
    for (var dp in dataPoints) {
      double time = dp["time"] ?? 0.0;
      double voltage = dp["voltage"] ?? 0.0;
      double current = dp["current"] ?? 0.0;
      double diff = dp["diffCurrent"] ?? 0.0;
      csv.writeln("$time,$voltage,$current,$diff");
    }

    await _shareCSVFile(filename, csv.toString());
  }

  static Future<void> exportEIS(String filename, List<Map<String, dynamic>> dataPoints) async {
    StringBuffer csv = StringBuffer();
    csv.writeln("Frequency (Hz),Real Z (Ohm),Imag Z (Ohm),Magnitude (Ohm),Phase (deg)");
    
    for (var dp in dataPoints) {
      double freq = dp["frequency"] ?? 0.0;
      double real = dp["realZ"] ?? 0.0;
      double imag = dp["imagZ"] ?? 0.0;
      double mag = dp["magnitude"] ?? 0.0;
      double phase = dp["phase"] ?? 0.0;
      csv.writeln("$freq,$real,$imag,$mag,$phase");
    }

    await _shareCSVFile(filename, csv.toString());
  }

  static Future<void> _shareCSVFile(String filename, String content) async {
    final directory = await getTemporaryDirectory();
    final file = File('${directory.path}/$filename.csv');
    await file.writeAsString(content);

    // Trigger system share panel
    await Share.shareXFiles([XFile(file.path)], text: 'Exported Potentiostat Data');
  }
}

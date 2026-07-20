import 'dart:convert';
import 'package:flutter/services.dart' show rootBundle;

// ================================================================
// MaterialProfile
//
// Mirrors the JSON schema in materials-library-and-applications.md §6 —
// a material coating + target analyte + AD5941 method + scan parameters,
// bundled as one selectable unit. This is what lets an operator pick
// "Lead & Cadmium (water)" instead of manually configuring an SWV sweep.
//
// New materials/analytes = new entries in assets/material_profiles.json,
// not new app code or firmware changes.
// ================================================================
class MaterialProfile {
  final String id;
  final String name;
  final String material;
  final List<String> targetAnalytes;
  final String method; // "CV" | "CA" | "SWV" | "EIS"
  final String electrolyte;
  final String notes;
  final Map<String, dynamic> params;

  MaterialProfile({
    required this.id,
    required this.name,
    required this.material,
    required this.targetAnalytes,
    required this.method,
    required this.electrolyte,
    required this.notes,
    required this.params,
  });

  factory MaterialProfile.fromJson(Map<String, dynamic> json) {
    return MaterialProfile(
      id: json['id'] as String,
      name: json['name'] as String,
      material: json['material'] as String,
      targetAnalytes: (json['target_analytes'] as List).cast<String>(),
      method: json['method'] as String,
      electrolyte: json['electrolyte'] as String,
      notes: json['notes'] as String? ?? '',
      params: Map<String, dynamic>.from(json['params'] as Map),
    );
  }

  /// Builds the exact {"method": ..., "params": {...}} command this profile
  /// sends to the device — same wire format as the manual/advanced forms.
  Map<String, dynamic> toCommand() => {
        'method': method,
        'params': params,
      };

  /// Loads all bundled profiles from assets/material_profiles.json.
  /// A remote-fetched/updatable profile list (per the OTA design doc) is the
  /// natural next step — new materials become a data update, not an
  /// app-store release — but ship with a bundled asset first.
  static Future<List<MaterialProfile>> loadBundled() async {
    final raw = await rootBundle.loadString('assets/material_profiles.json');
    final decoded = jsonDecode(raw) as Map<String, dynamic>;
    final list = (decoded['profiles'] as List)
        .map((e) => MaterialProfile.fromJson(e as Map<String, dynamic>))
        .toList();
    return list;
  }
}

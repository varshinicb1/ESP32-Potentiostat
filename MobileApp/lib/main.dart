import 'package:flutter/material.dart';
import 'package:provider/provider.dart';
import 'services/ble_service.dart';
import 'pages/home_page.dart';

void main() {
  runApp(
    MultiProvider(
      providers: [
        ChangeNotifierProvider(create: (_) => BLEService()),
      ],
      child: const PotentiostatApp(),
    ),
  );
}

// ================================================================
// AnalyteX brand palette
//
// Previously this theme was untouched Flutter-tutorial defaults
// (Colors.blueAccent, #121212 background, no type scale) — generic in a way
// that undermines a product meant to be sold as precision lab instrumentation
// worldwide. Deliberately distinct from the generic "developer dashboard
// blue" look: a cool electric teal (precision/analytical, not "just another
// app") on a near-black ink background, with a warm amber reserved
// specifically for literature/calibration callouts so it reads as
// "important reference data," not decoration.
// ================================================================
class AnalyteXColors {
  static const teal = Color(0xFF14E0C4); // primary — precision/analytical accent
  static const tealDim = Color(0xFF0E9C88); // pressed/secondary variant of teal
  static const ink = Color(0xFF0A0F12); // app background — near-black, blue-tinted
  static const surface = Color(0xFF131B1F); // cards
  static const surfaceRaised = Color(0xFF1A252B); // inputs, raised elements
  static const outline = Color(0xFF23343B); // hairline borders
  static const textPrimary = Color(0xFFEAF2F1);
  static const textMuted = Color(0xFF7E9498);
  static const amber = Color(0xFFF2B84B); // literature/reference callouts only
  static const danger = Color(0xFFE05B5B);
  static const success = Color(0xFF3ECF8E);
}

class PotentiostatApp extends StatelessWidget {
  const PotentiostatApp({super.key});

  @override
  Widget build(BuildContext context) {
    final c = AnalyteXColors.teal;
    return MaterialApp(
      title: 'AnalyteX',
      debugShowCheckedModeBanner: false,
      themeMode: ThemeMode.dark,
      darkTheme: ThemeData(
        brightness: Brightness.dark,
        useMaterial3: true,
        scaffoldBackgroundColor: AnalyteXColors.ink,
        colorScheme: const ColorScheme.dark(
          primary: AnalyteXColors.teal,
          secondary: AnalyteXColors.amber,
          surface: AnalyteXColors.surface,
          error: AnalyteXColors.danger,
        ),
        cardTheme: CardThemeData(
          color: AnalyteXColors.surface,
          elevation: 0,
          shape: RoundedRectangleBorder(
            borderRadius: BorderRadius.circular(14),
            side: const BorderSide(color: AnalyteXColors.outline),
          ),
        ),
        appBarTheme: const AppBarTheme(
          backgroundColor: AnalyteXColors.ink,
          surfaceTintColor: Colors.transparent,
          elevation: 0,
        ),
        textTheme: const TextTheme(
          titleLarge: TextStyle(
            fontWeight: FontWeight.w800,
            letterSpacing: 0.5,
            color: AnalyteXColors.textPrimary,
          ),
          titleMedium: TextStyle(
            fontWeight: FontWeight.w700,
            color: AnalyteXColors.textPrimary,
          ),
          bodyMedium: TextStyle(color: AnalyteXColors.textPrimary),
          bodySmall: TextStyle(color: AnalyteXColors.textMuted),
        ),
        elevatedButtonTheme: ElevatedButtonThemeData(
          style: ElevatedButton.styleFrom(
            backgroundColor: c,
            foregroundColor: AnalyteXColors.ink,
            disabledBackgroundColor: AnalyteXColors.surfaceRaised,
            disabledForegroundColor: AnalyteXColors.textMuted,
            elevation: 0,
            shape: RoundedRectangleBorder(
              borderRadius: BorderRadius.circular(12),
            ),
          ),
        ),
        textButtonTheme: TextButtonThemeData(
          style: TextButton.styleFrom(foregroundColor: c),
        ),
        inputDecorationTheme: InputDecorationTheme(
          filled: true,
          fillColor: AnalyteXColors.surfaceRaised,
          border: OutlineInputBorder(
            borderRadius: BorderRadius.circular(12),
            borderSide: const BorderSide(color: AnalyteXColors.outline),
          ),
          enabledBorder: OutlineInputBorder(
            borderRadius: BorderRadius.circular(12),
            borderSide: const BorderSide(color: AnalyteXColors.outline),
          ),
          focusedBorder: OutlineInputBorder(
            borderRadius: BorderRadius.circular(12),
            borderSide: BorderSide(color: c, width: 2),
          ),
          labelStyle: const TextStyle(color: AnalyteXColors.textMuted),
        ),
        dividerColor: AnalyteXColors.outline,
      ),
      home: const HomePage(),
    );
  }
}

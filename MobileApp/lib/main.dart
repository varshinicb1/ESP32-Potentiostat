import 'dart:math' as math;
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

// ============================================================================
// AnalyteX design system — "field scientific instrument"
//
// The app is meant to feel like the screen of a precision analytical device,
// not a generic dark-mode dashboard. Three deliberate moves carry that:
//   1. A faint GRATICULE (graph-paper grid) behind every screen — the visual
//      language of an oscilloscope / a voltammogram, which is literally what
//      this device draws.
//   2. IBM Plex typography: Plex Sans for UI, Plex Mono for every DATA value
//      and technical label, so numbers read like an instrument readout.
//   3. One confident SIGNAL-GREEN accent (a live-trace colour) used sparingly
//      against deep graphite, with amber reserved for reference/literature.
// ============================================================================
class AX {
  // Surfaces — deep graphite, faintly green-tinted
  static const ink = Color(0xFF080B0A); // scaffold
  static const panel = Color(0xFF10160F); // cards / raised surfaces
  static const panelHi = Color(0xFF171F16); // inputs, chips
  static const hairline = Color(0xFF243026); // 1px rules / borders
  static const grid = Color(0xFF15201A); // graticule minor lines

  // Signal — the live-trace green (primary accent; use sparingly)
  static const signal = Color(0xFF8CEB57);
  static const signalDim = Color(0xFF4E7A34);

  // Reference / warning — amber (literature callouts, cautions)
  static const amber = Color(0xFFF0B429);
  static const danger = Color(0xFFFF5C5C);
  static const ok = Color(0xFF7BE0A4);

  // Text
  static const textHi = Color(0xFFE9F1EA);
  static const textMid = Color(0xFF9BB0A2);
  static const textLo = Color(0xFF5D7166);

  // ---- Back-compat aliases (older screens referenced these names) ----
  static const teal = signal;
  static const tealDim = signalDim;
  static const surface = panel;
  static const surfaceRaised = panelHi;
  static const outline = hairline;
  static const textPrimary = textHi;
  static const textMuted = textMid;
  static const success = ok;
}

/// Old name kept so `import '../main.dart' show AnalyteXColors;` still resolves.
typedef AnalyteXColors = AX;

class PotentiostatApp extends StatelessWidget {
  const PotentiostatApp({super.key});

  @override
  Widget build(BuildContext context) {
    const eyebrow = TextStyle(
      fontFamily: 'PlexMono',
      fontSize: 11,
      fontWeight: FontWeight.w500,
      letterSpacing: 2.0,
      color: AX.textMid,
    );
    return MaterialApp(
      title: 'AnalyteX',
      debugShowCheckedModeBanner: false,
      themeMode: ThemeMode.dark,
      darkTheme: ThemeData(
        brightness: Brightness.dark,
        useMaterial3: true,
        fontFamily: 'PlexSans',
        scaffoldBackgroundColor: AX.ink,
        colorScheme: const ColorScheme.dark(
          primary: AX.signal,
          onPrimary: AX.ink,
          secondary: AX.amber,
          surface: AX.panel,
          error: AX.danger,
        ),
        cardTheme: CardThemeData(
          color: AX.panel,
          elevation: 0,
          shape: RoundedRectangleBorder(
            borderRadius: BorderRadius.circular(4), // instruments have hard corners
            side: const BorderSide(color: AX.hairline),
          ),
        ),
        appBarTheme: const AppBarTheme(
          backgroundColor: Colors.transparent,
          surfaceTintColor: Colors.transparent,
          elevation: 0,
          centerTitle: false,
        ),
        textTheme: const TextTheme(
          titleLarge: TextStyle(fontWeight: FontWeight.w700, letterSpacing: -0.2, color: AX.textHi),
          titleMedium: TextStyle(fontWeight: FontWeight.w600, color: AX.textHi),
          bodyMedium: TextStyle(color: AX.textHi, height: 1.35),
          bodySmall: TextStyle(color: AX.textMid, height: 1.35),
          labelSmall: eyebrow,
        ),
        elevatedButtonTheme: ElevatedButtonThemeData(
          style: ElevatedButton.styleFrom(
            backgroundColor: AX.signal,
            foregroundColor: AX.ink,
            disabledBackgroundColor: AX.panelHi,
            disabledForegroundColor: AX.textLo,
            elevation: 0,
            textStyle: const TextStyle(
              fontFamily: 'PlexSans',
              fontWeight: FontWeight.w700,
              letterSpacing: 0.3,
            ),
            shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(4)),
          ),
        ),
        textButtonTheme: TextButtonThemeData(
          style: TextButton.styleFrom(
            foregroundColor: AX.signal,
            textStyle: const TextStyle(fontFamily: 'PlexMono', fontWeight: FontWeight.w500, letterSpacing: 0.5),
          ),
        ),
        inputDecorationTheme: InputDecorationTheme(
          filled: true,
          fillColor: AX.panelHi,
          border: OutlineInputBorder(
            borderRadius: BorderRadius.circular(4),
            borderSide: const BorderSide(color: AX.hairline),
          ),
          enabledBorder: OutlineInputBorder(
            borderRadius: BorderRadius.circular(4),
            borderSide: const BorderSide(color: AX.hairline),
          ),
          focusedBorder: OutlineInputBorder(
            borderRadius: BorderRadius.circular(4),
            borderSide: const BorderSide(color: AX.signal, width: 1.5),
          ),
          labelStyle: const TextStyle(color: AX.textMid, fontFamily: 'PlexMono', fontSize: 13),
        ),
        dividerColor: AX.hairline,
      ),
      home: const HomePage(),
    );
  }
}

// ============================================================================
// GraticuleBackground — the signature motif. A faint graph-paper grid painted
// behind screen content so the whole app reads like an instrument display.
// Wrap a screen body in this.
// ============================================================================
class GraticuleBackground extends StatelessWidget {
  final Widget child;
  const GraticuleBackground({super.key, required this.child});

  @override
  Widget build(BuildContext context) {
    return DecoratedBox(
      decoration: const BoxDecoration(
        gradient: RadialGradient(
          center: Alignment(0, -0.6),
          radius: 1.3,
          colors: [Color(0xFF0E1512), AX.ink],
        ),
      ),
      child: CustomPaint(
        painter: _GraticulePainter(),
        child: child,
      ),
    );
  }
}

class _GraticulePainter extends CustomPainter {
  static const double step = 26; // minor grid spacing

  @override
  void paint(Canvas canvas, Size size) {
    final minor = Paint()
      ..color = AX.grid
      ..strokeWidth = 1;
    final major = Paint()
      ..color = const Color(0xFF1B2A22)
      ..strokeWidth = 1;

    int i = 0;
    for (double x = 0; x <= size.width; x += step, i++) {
      canvas.drawLine(Offset(x, 0), Offset(x, size.height), i % 4 == 0 ? major : minor);
    }
    i = 0;
    for (double y = 0; y <= size.height; y += step, i++) {
      canvas.drawLine(Offset(0, y), Offset(size.width, y), i % 4 == 0 ? major : minor);
    }
  }

  @override
  bool shouldRepaint(covariant CustomPainter oldDelegate) => false;
}

// ============================================================================
// SignalTrace — a small live-looking waveform in signal-green. The one
// memorable flourish; used in the header so the app feels "on" like an
// instrument, even before a measurement runs.
// ============================================================================
class SignalTrace extends StatelessWidget {
  final double height;
  final int seed;
  const SignalTrace({super.key, this.height = 34, this.seed = 7});

  @override
  Widget build(BuildContext context) {
    return SizedBox(
      height: height,
      width: double.infinity,
      child: CustomPaint(painter: _TracePainter(seed)),
    );
  }
}

class _TracePainter extends CustomPainter {
  final int seed;
  _TracePainter(this.seed);

  @override
  void paint(Canvas canvas, Size size) {
    final rnd = math.Random(seed);
    final path = Path();
    const n = 120;
    for (int k = 0; k <= n; k++) {
      final t = k / n;
      final x = t * size.width;
      // a shaped "voltammogram-ish" bump plus a little noise
      final bump = math.exp(-math.pow((t - 0.55) * 4.2, 2).toDouble()) * 0.8;
      final noise = (rnd.nextDouble() - 0.5) * 0.06;
      final y = size.height * (0.72 - bump * 0.5 + noise);
      if (k == 0) {
        path.moveTo(x, y);
      } else {
        path.lineTo(x, y);
      }
    }
    // glow underlay + crisp line
    canvas.drawPath(
      path,
      Paint()
        ..color = AX.signal.withValues(alpha: 0.25)
        ..style = PaintingStyle.stroke
        ..strokeWidth = 4
        ..maskFilter = const MaskFilter.blur(BlurStyle.normal, 4),
    );
    canvas.drawPath(
      path,
      Paint()
        ..color = AX.signal
        ..style = PaintingStyle.stroke
        ..strokeWidth = 1.6
        ..strokeJoin = StrokeJoin.round,
    );
  }

  @override
  bool shouldRepaint(covariant CustomPainter oldDelegate) => false;
}

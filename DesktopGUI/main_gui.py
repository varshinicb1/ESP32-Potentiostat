"""
FreiStat Potentiostat Desktop Suite
VidyuthLabs — Desktop Control Panel v0.2.0

Supports:
  - USB Serial connection (JSON lines over UART)
  - WiFi WebSocket connection (ws://<device-ip>:81)
  - Techniques: CV, CA, SWV, EIS
  - Live pyqtgraph plotting
  - CSV export with complete data columns

Bugs fixed vs v0.1:
  - list.add() → list.append()  (critical crash on every data point)
  - Added SWV method
  - Input validation with user-friendly error dialogs
  - Safe serial read loop (threading.Event stop flag)
  - Fixed bare except clauses → logged exceptions
  - Fixed CSV export (all columns, correct time axis for CA)
  - Fixed WebSocket connection state tracking
  - Fixed CA plotting to use time axis
"""

import sys
import json
import logging
import threading
import csv
import os
import time

from PyQt6.QtWidgets import (
    QApplication, QMainWindow, QWidget, QVBoxLayout, QHBoxLayout,
    QGridLayout, QLabel, QLineEdit, QPushButton, QComboBox,
    QTabWidget, QFileDialog, QMessageBox, QGroupBox, QStatusBar,
    QScrollArea,
)
from PyQt6.QtCore import pyqtSignal, QObject, Qt
from PyQt6.QtGui import QFont
import pyqtgraph as pg
import serial
import serial.tools.list_ports
import websocket

# Configure logging to stderr so errors are visible during development
logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(name)s: %(message)s",
    stream=sys.stderr,
)
logger = logging.getLogger("PotentiostatGUI")


# ──────────────────────────────────────────────────────────────────
# Cross-thread signal bridge (Qt signals are the correct way to pass
# data from daemon threads into the GUI thread)
# ──────────────────────────────────────────────────────────────────
class CommSignals(QObject):
    data_received  = pyqtSignal(dict)
    status_changed = pyqtSignal(str)


# ──────────────────────────────────────────────────────────────────
# Main Application Window
# ──────────────────────────────────────────────────────────────────
class PotentiostatGUI(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("VidyuthLabs Potentiostat Desktop Suite  v0.2.0")
        self.resize(1100, 750)
        self._apply_theme()

        self.signals = CommSignals()
        self.signals.data_received.connect(self._handle_incoming_data)
        self.signals.status_changed.connect(self._update_status)

        self.serial_conn    = None
        self.ws_conn        = None
        self._serial_stop   = threading.Event()   # Clean stop for serial thread
        self.running_thread = None

        # List of measurement data dicts (populated by _handle_incoming_data)
        self.rawData: list[dict] = []

        self._setup_ui()

    # ──────────────────────────────────────────────────────────────
    # Theme
    # ──────────────────────────────────────────────────────────────
    def _apply_theme(self):
        self.setStyleSheet("""
            QMainWindow, QWidget {
                background-color: #121212;
                color: #E0E0E0;
                font-family: 'Segoe UI', 'Inter', Arial, sans-serif;
                font-size: 13px;
            }
            QGroupBox {
                border: 1px solid #2C2C2C;
                border-radius: 8px;
                margin-top: 14px;
                padding: 12px 10px 10px 10px;
                font-weight: bold;
                color: #90CAF9;
            }
            QGroupBox::title {
                subcontrol-origin: margin;
                left: 10px;
                padding: 0 4px;
            }
            QLineEdit, QComboBox {
                background-color: #1E1E1E;
                border: 1px solid #3A3A3A;
                border-radius: 5px;
                padding: 5px 8px;
                color: #E0E0E0;
                selection-background-color: #1565C0;
            }
            QLineEdit:focus, QComboBox:focus {
                border-color: #1976D2;
            }
            QPushButton {
                background-color: #1565C0;
                border: none;
                border-radius: 5px;
                padding: 8px 16px;
                color: #FFFFFF;
                font-weight: bold;
            }
            QPushButton:hover    { background-color: #1976D2; }
            QPushButton:pressed  { background-color: #0D47A1; }
            QPushButton:disabled { background-color: #2A2A2A; color: #555555; }
            QPushButton#stopBtn  { background-color: #B71C1C; }
            QPushButton#stopBtn:hover { background-color: #C62828; }
            QTabWidget::pane {
                border: 1px solid #2C2C2C;
                background-color: #181818;
                border-radius: 4px;
            }
            QTabBar::tab {
                background: #1E1E1E;
                border: 1px solid #2C2C2C;
                padding: 7px 14px;
                color: #AAAAAA;
            }
            QTabBar::tab:selected   { background: #1565C0; color: #FFFFFF; }
            QTabBar::tab:hover      { background: #263238; color: #E0E0E0; }
            QLabel#statusLabel {
                color: #AAAAAA;
                font-size: 12px;
                padding: 2px 4px;
            }
        """)

    # ──────────────────────────────────────────────────────────────
    # UI Setup
    # ──────────────────────────────────────────────────────────────
    def _setup_ui(self):
        main_widget = QWidget()
        self.setCentralWidget(main_widget)
        main_layout = QHBoxLayout(main_widget)
        main_layout.setContentsMargins(12, 12, 12, 12)
        main_layout.setSpacing(12)

        # ── Left Panel (config) ──────────────────────────────────
        left_widget = QWidget()
        left_widget.setFixedWidth(320)
        left_panel  = QVBoxLayout(left_widget)
        left_panel.setContentsMargins(0, 0, 0, 0)
        main_layout.addWidget(left_widget)

        # Connection box
        conn_box    = QGroupBox("Connection")
        conn_layout = QGridLayout(conn_box)

        conn_layout.addWidget(QLabel("Interface:"), 0, 0)
        self.interface_select = QComboBox()
        self.interface_select.addItems(["USB Serial", "WiFi WebSocket"])
        self.interface_select.currentIndexChanged.connect(self._toggle_interface_inputs)
        conn_layout.addWidget(self.interface_select, 0, 1)

        self.port_label  = QLabel("COM Port:")
        self.port_select = QComboBox()
        self.port_select.setMinimumWidth(140)
        self._refresh_ports()
        conn_layout.addWidget(self.port_label, 1, 0)
        conn_layout.addWidget(self.port_select, 1, 1)

        refresh_btn = QPushButton("↺")
        refresh_btn.setFixedWidth(32)
        refresh_btn.setToolTip("Refresh COM ports")
        refresh_btn.clicked.connect(self._refresh_ports)
        conn_layout.addWidget(refresh_btn, 1, 2)

        self.ip_label = QLabel("Device IP:")
        self.ip_input = QLineEdit("192.168.4.1")
        self.ip_label.hide()
        self.ip_input.hide()
        conn_layout.addWidget(self.ip_label, 2, 0)
        conn_layout.addWidget(self.ip_input, 2, 1, 1, 2)

        self.connect_btn = QPushButton("Connect")
        self.connect_btn.clicked.connect(self._toggle_connection)
        conn_layout.addWidget(self.connect_btn, 3, 0, 1, 3)
        left_panel.addWidget(conn_box)

        # Experiment box
        exp_box    = QGroupBox("Experiment Configuration")
        exp_layout = QVBoxLayout(exp_box)

        self.method_select = QComboBox()
        self.method_select.addItems([
            "Cyclic Voltammetry (CV)",
            "Chronoamperometry (CA)",
            "Square Wave Voltammetry (SWV)",
            "Impedance Spectroscopy (EIS)",
        ])
        self.method_select.currentIndexChanged.connect(self._on_method_changed)
        exp_layout.addWidget(self.method_select)

        self.param_tabs = QTabWidget()

        # ── CV Tab ──
        cv_tab  = QWidget()
        cv_grid = QGridLayout(cv_tab)
        self.cv_start    = self._make_field(cv_grid, "Start Voltage (V):",  0, "-0.5")
        self.cv_v1       = self._make_field(cv_grid, "Vertex 1 (V):",       1,  "0.8")
        self.cv_v2       = self._make_field(cv_grid, "Vertex 2 (V):",       2, "-0.8")
        self.cv_scanrate = self._make_field(cv_grid, "Scan Rate (V/s):",    3,  "0.1")
        self.cv_cycles   = self._make_field(cv_grid, "Cycles:",             4,    "2")
        self.param_tabs.addTab(cv_tab, "CV")

        # ── CA Tab ──
        ca_tab  = QWidget()
        ca_grid = QGridLayout(ca_tab)
        self.ca_step = self._make_field(ca_grid, "Step Voltage (V):", 0, "0.5")
        self.ca_dur  = self._make_field(ca_grid, "Duration (s):",     1,  "10")
        self.ca_int  = self._make_field(ca_grid, "Interval (s):",     2, "0.1")
        self.param_tabs.addTab(ca_tab, "CA")

        # ── SWV Tab ──
        swv_tab  = QWidget()
        swv_grid = QGridLayout(swv_tab)
        self.swv_start = self._make_field(swv_grid, "Start Voltage (V):", 0, "-0.5")
        self.swv_stop  = self._make_field(swv_grid, "Stop Voltage (V):",  1,  "0.5")
        self.swv_step  = self._make_field(swv_grid, "Step Height (V):",   2, "0.005")
        self.swv_amp   = self._make_field(swv_grid, "Amplitude (V):",     3, "0.025")
        self.swv_freq  = self._make_field(swv_grid, "Frequency (Hz):",    4,   "25")
        self.param_tabs.addTab(swv_tab, "SWV")

        # ── EIS Tab ──
        eis_tab  = QWidget()
        eis_grid = QGridLayout(eis_tab)
        self.eis_start = self._make_field(eis_grid, "Start Freq (Hz):", 0,    "10")
        self.eis_stop  = self._make_field(eis_grid, "Stop Freq (Hz):",  1, "100000")
        self.eis_steps = self._make_field(eis_grid, "Steps:",           2,    "50")
        self.eis_amp   = self._make_field(eis_grid, "Amplitude (mV):",  3,    "10")
        self.eis_bias  = self._make_field(eis_grid, "DC Bias (mV):",    4,     "0")
        self.param_tabs.addTab(eis_tab, "EIS")

        exp_layout.addWidget(self.param_tabs)
        left_panel.addWidget(exp_box)

        # Action buttons
        btn_layout = QVBoxLayout()

        self.start_btn = QPushButton("▶  Start Experiment")
        self.start_btn.setEnabled(False)
        self.start_btn.clicked.connect(self._start_experiment)
        btn_layout.addWidget(self.start_btn)

        self.stop_btn = QPushButton("■  Abort")
        self.stop_btn.setObjectName("stopBtn")
        self.stop_btn.setEnabled(False)
        self.stop_btn.clicked.connect(self._send_abort)
        btn_layout.addWidget(self.stop_btn)

        self.export_btn = QPushButton("⬇  Export CSV")
        self.export_btn.setEnabled(False)
        self.export_btn.clicked.connect(self._export_csv)
        btn_layout.addWidget(self.export_btn)

        left_panel.addLayout(btn_layout)
        left_panel.addStretch()

        # Status label
        self.status_label = QLabel("Disconnected")
        self.status_label.setObjectName("statusLabel")
        self.status_label.setWordWrap(True)
        left_panel.addWidget(self.status_label)

        # ── Right Panel (plot) ───────────────────────────────────
        right_panel = QVBoxLayout()
        right_panel.setContentsMargins(0, 0, 0, 0)
        main_layout.addLayout(right_panel, 1)

        # Data point counter
        self.point_count_label = QLabel("Points: 0")
        self.point_count_label.setAlignment(Qt.AlignmentFlag.AlignRight)
        right_panel.addWidget(self.point_count_label)

        self.plot_widget = pg.PlotWidget()
        self.plot_widget.setBackground("#181818")
        self.plot_widget.showGrid(x=True, y=True, alpha=0.25)
        self.plot_widget.getAxis("left").setTextPen("#AAAAAA")
        self.plot_widget.getAxis("bottom").setTextPen("#AAAAAA")
        self.curve = self.plot_widget.plot(pen=pg.mkPen("#1E88E5", width=2))
        right_panel.addWidget(self.plot_widget)

    # ──────────────────────────────────────────────────────────────
    # Helper to add a labelled field row to a grid layout
    # ──────────────────────────────────────────────────────────────
    @staticmethod
    def _make_field(grid: QGridLayout, label: str, row: int,
                    default: str = "") -> QLineEdit:
        lbl = QLabel(label)
        lbl.setAlignment(Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter)
        grid.addWidget(lbl, row, 0)
        field = QLineEdit(default)
        field.setFixedHeight(30)
        grid.addWidget(field, row, 1)
        return field

    # ──────────────────────────────────────────────────────────────
    # Connection management
    # ──────────────────────────────────────────────────────────────
    def _refresh_ports(self):
        self.port_select.clear()
        for port in serial.tools.list_ports.comports():
            self.port_select.addItem(port.device)

    def _toggle_interface_inputs(self, idx: int):
        serial_mode = (idx == 0)
        self.port_label.setVisible(serial_mode)
        self.port_select.setVisible(serial_mode)
        self.ip_label.setVisible(not serial_mode)
        self.ip_input.setVisible(not serial_mode)

    def _toggle_connection(self):
        if self.connect_btn.text().startswith("Connect"):
            self._do_connect()
        else:
            self._disconnect_all()

    def _do_connect(self):
        if self.interface_select.currentIndex() == 0:
            # ── Serial ──
            port = self.port_select.currentText()
            if not port:
                QMessageBox.warning(self, "No Port", "Select a COM port first.")
                return
            try:
                self.serial_conn = serial.Serial(port, 115200, timeout=1)
                self.connect_btn.setText("Disconnect")
                self.start_btn.setEnabled(True)
                self._update_status(f"Connected to {port}")
                self._serial_stop.clear()
                self.running_thread = threading.Thread(
                    target=self._read_serial_loop, daemon=True)
                self.running_thread.start()
            except serial.SerialException as e:
                logger.error("Serial connect error: %s", e)
                QMessageBox.critical(self, "Connection Error",
                                     f"Could not open {port}:\n{e}")
        else:
            # ── WebSocket ──
            ip = self.ip_input.text().strip()
            if not ip:
                QMessageBox.warning(self, "No IP", "Enter the device IP address.")
                return
            try:
                url = f"ws://{ip}:81"
                self.ws_conn = websocket.WebSocketApp(
                    url,
                    on_message=self._on_ws_message,
                    on_error=self._on_ws_error,
                    on_close=self._on_ws_close,
                    on_open=self._on_ws_open,
                )
                self.running_thread = threading.Thread(
                    target=self.ws_conn.run_forever, daemon=True)
                self.running_thread.start()
                # Status will be set by on_ws_open callback
                self._update_status(f"Connecting to {url}…")
            except Exception as e:
                logger.error("WebSocket connect error: %s", e)
                QMessageBox.critical(self, "Connection Error",
                                     f"Could not connect:\n{e}")

    def _disconnect_all(self):
        # Signal serial thread to stop cleanly
        self._serial_stop.set()
        if self.serial_conn:
            try:
                self.serial_conn.close()
            except Exception:
                pass
            self.serial_conn = None
        if self.ws_conn:
            try:
                self.ws_conn.close()
            except Exception:
                pass
            self.ws_conn = None
        self.connect_btn.setText("Connect")
        self.start_btn.setEnabled(False)
        self.stop_btn.setEnabled(False)
        self._update_status("Disconnected")

    # ──────────────────────────────────────────────────────────────
    # WebSocket callbacks (called from WebSocketApp background thread)
    # ──────────────────────────────────────────────────────────────
    def _on_ws_open(self, ws):
        self.signals.status_changed.emit(
            f"Connected to ws://{self.ip_input.text().strip()}:81")
        # Enable Start button from GUI thread via signal
        self.connect_btn.setText("Disconnect")
        # Note: GUI widget updates must happen on main thread; use a signal
        self.signals.status_changed.emit("__WS_OPEN__")

    def _on_ws_message(self, ws, msg: str):
        try:
            data = json.loads(msg)
            self.signals.data_received.emit(data)
        except json.JSONDecodeError as e:
            logger.warning("WebSocket JSON parse error: %s | msg: %s", e, msg[:80])

    def _on_ws_error(self, ws, err):
        logger.error("WebSocket error: %s", err)
        self.signals.status_changed.emit(f"WS Error: {err}")

    def _on_ws_close(self, ws, code, reason):
        logger.info("WebSocket closed: code=%s reason=%s", code, reason)
        self.signals.status_changed.emit("WS Closed")

    # ──────────────────────────────────────────────────────────────
    # Serial read loop — runs in daemon thread
    # Uses threading.Event for clean shutdown (no AttributeError race)
    # ──────────────────────────────────────────────────────────────
    def _read_serial_loop(self):
        logger.info("Serial read loop started")
        while not self._serial_stop.is_set():
            try:
                conn = self.serial_conn
                if conn is None or not conn.is_open:
                    break
                if conn.in_waiting > 0:
                    line = conn.readline().decode("utf-8", errors="replace").strip()
                    if line:
                        try:
                            data = json.loads(line)
                            self.signals.data_received.emit(data)
                        except json.JSONDecodeError:
                            # Non-JSON lines (e.g., boot messages) are ignored
                            pass
                else:
                    time.sleep(0.005)
            except serial.SerialException as e:
                logger.error("Serial read error: %s", e)
                self.signals.status_changed.emit(f"Serial error: {e}")
                break
            except Exception as e:
                logger.exception("Unexpected error in serial loop: %s", e)
                break
        logger.info("Serial read loop exited")

    # ──────────────────────────────────────────────────────────────
    # Experiment
    # ──────────────────────────────────────────────────────────────
    def _on_method_changed(self, idx: int):
        self.param_tabs.setCurrentIndex(idx)

    def _start_experiment(self):
        try:
            cmd = self._build_command()
        except ValueError as e:
            QMessageBox.warning(self, "Invalid Parameters", str(e))
            return
        except Exception as e:
            QMessageBox.critical(self, "Error", f"Unexpected error: {e}")
            logger.exception("Error building command")
            return

        self.rawData.clear()
        self.curve.clear()
        self.export_btn.setEnabled(False)
        self.stop_btn.setEnabled(True)
        self.point_count_label.setText("Points: 0")

        cmd_str = json.dumps(cmd)
        self._send_raw(cmd_str)
        self._update_status(f"Running {cmd['method']}…")
        logger.info("Started experiment: %s", cmd_str[:120])

    def _build_command(self) -> dict:
        idx = self.method_select.currentIndex()

        if idx == 0:  # CV
            self.plot_widget.setLabel("left",   "Current", "A")
            self.plot_widget.setLabel("bottom", "Voltage", "V")
            return {"method": "CV", "params": {
                "start_voltage": self._pf(self.cv_start,    "Start Voltage",   -2.0, 2.0),
                "vertex_1":      self._pf(self.cv_v1,       "Vertex 1",        -2.0, 2.0),
                "vertex_2":      self._pf(self.cv_v2,       "Vertex 2",        -2.0, 2.0),
                "scan_rate":     self._pf(self.cv_scanrate, "Scan Rate",       0.001, 10.0),
                "cycles":        self._pi(self.cv_cycles,   "Cycles",          1, 100),
            }}

        if idx == 1:  # CA
            self.plot_widget.setLabel("left",   "Current", "A")
            self.plot_widget.setLabel("bottom", "Time",    "s")
            return {"method": "CA", "params": {
                "step_voltage": self._pf(self.ca_step, "Step Voltage", -2.0, 2.0),
                "duration":     self._pf(self.ca_dur,  "Duration",     0.1, 3600.0),
                "interval":     self._pf(self.ca_int,  "Interval",     0.001, 60.0),
            }}

        if idx == 2:  # SWV
            self.plot_widget.setLabel("left",   "ΔI",     "A")
            self.plot_widget.setLabel("bottom", "Voltage", "V")
            return {"method": "SWV", "params": {
                "start_voltage": self._pf(self.swv_start, "Start Voltage", -2.0, 2.0),
                "stop_voltage":  self._pf(self.swv_stop,  "Stop Voltage",  -2.0, 2.0),
                "step_height":   self._pf(self.swv_step,  "Step Height",   0.001, 0.1),
                "amplitude":     self._pf(self.swv_amp,   "Amplitude",     0.001, 0.5),
                "frequency":     self._pf(self.swv_freq,  "Frequency",     1.0, 1000.0),
            }}

        if idx == 3:  # EIS
            self.plot_widget.setLabel("left",   "-Z'' (Imag)", "Ω")
            self.plot_widget.setLabel("bottom", "Z' (Real)",   "Ω")
            start = self._pf(self.eis_start, "Start Frequency", 0.1, 1e6)
            stop  = self._pf(self.eis_stop,  "Stop Frequency",  start + 0.1, 1e6)
            return {"method": "EIS", "params": {
                "start_freq": start,
                "stop_freq":  stop,
                "steps":      self._pi(self.eis_steps, "Steps",     1, 1000),
                "amplitude":  self._pf(self.eis_amp,   "Amplitude", 1.0, 200.0),
                "bias":       self._pf(self.eis_bias,  "Bias",     -1000.0, 1000.0),
            }}

        raise ValueError("Unknown method index")

    def _pf(self, widget: QLineEdit, name: str, lo: float, hi: float) -> float:
        """Parse and validate a float field."""
        raw = widget.text().strip()
        if not raw:
            raise ValueError(f"'{name}' is empty.")
        try:
            val = float(raw)
        except ValueError:
            raise ValueError(f"'{name}' must be a number (got '{raw}').")
        if val < lo or val > hi:
            raise ValueError(f"'{name}' = {val} is out of range [{lo}, {hi}].")
        return val

    def _pi(self, widget: QLineEdit, name: str, lo: int, hi: int) -> int:
        """Parse and validate an integer field."""
        raw = widget.text().strip()
        if not raw:
            raise ValueError(f"'{name}' is empty.")
        try:
            val = int(raw)
        except ValueError:
            raise ValueError(f"'{name}' must be an integer (got '{raw}').")
        if val < lo or val > hi:
            raise ValueError(f"'{name}' = {val} is out of range [{lo}, {hi}].")
        return val

    def _send_abort(self):
        cmd_str = json.dumps({"method": "ABORT"})
        self._send_raw(cmd_str)
        self._update_status("Abort sent")
        self.stop_btn.setEnabled(False)
        logger.info("ABORT command sent")

    def _send_raw(self, text: str):
        """Send a raw string over whichever transport is active."""
        try:
            if self.serial_conn and self.serial_conn.is_open:
                self.serial_conn.write((text + "\n").encode("utf-8"))
            elif self.ws_conn:
                self.ws_conn.send(text)
        except Exception as e:
            logger.error("Send error: %s", e)
            self._update_status(f"Send error: {e}")

    # ──────────────────────────────────────────────────────────────
    # Incoming data handler (runs on GUI thread via Qt signal)
    # ──────────────────────────────────────────────────────────────
    def _handle_incoming_data(self, data: dict):
        # Handle special internal signals
        if data.get("__internal__") == "ws_open":
            self.connect_btn.setText("Disconnect")
            self.start_btn.setEnabled(True)
            return

        msg_type = data.get("type")
        status   = data.get("status")
        idx      = self.method_select.currentIndex()

        if msg_type == "data":
            self.rawData.append(data)   # ← was .add() — FIXED

            if idx == 0:   # CV: current vs voltage
                x = [d.get("voltage", 0.0) for d in self.rawData]
                y = [d.get("current", 0.0) for d in self.rawData]
            elif idx == 1: # CA: current vs time  (was incorrectly using voltage)
                x = [d.get("time",    0.0) for d in self.rawData]
                y = [d.get("current", 0.0) for d in self.rawData]
            elif idx == 2: # SWV: diff current vs voltage
                x = [d.get("voltage",     0.0) for d in self.rawData]
                y = [d.get("diffCurrent", 0.0) for d in self.rawData]
            else:
                x, y = [], []

            self.curve.setData(x, y)
            self.point_count_label.setText(f"Points: {len(self.rawData)}")

        elif msg_type == "eis_data":
            self.rawData.append(data)   # ← was .add() — FIXED

            x = [d.get("realZ",  0.0)  for d in self.rawData]
            y = [-d.get("imagZ", 0.0)  for d in self.rawData]
            self.curve.setData(x, y)
            self.point_count_label.setText(f"Points: {len(self.rawData)}")

        elif msg_type == "diagnostics":
            logger.info("Diagnostics: %s", data)

        elif status == "idle":
            self._update_status("Measurement finished.")
            self.export_btn.setEnabled(True)
            self.stop_btn.setEnabled(False)

        elif status == "started":
            self._update_status("Measurement running…")

        elif status == "aborting":
            self._update_status("Aborting…")
            self.stop_btn.setEnabled(False)

        elif status == "error":
            detail = data.get("detail", "Unknown error")
            self._update_status(f"Device error: {detail}")
            QMessageBox.warning(self, "Device Error", detail)

        elif status == "busy":
            detail = data.get("detail", "")
            self._update_status(f"Device busy: {detail}")

    # ──────────────────────────────────────────────────────────────
    # Status bar update
    # ──────────────────────────────────────────────────────────────
    def _update_status(self, text: str):
        # Internal signal from ws_open
        if text == "__WS_OPEN__":
            self.connect_btn.setText("Disconnect")
            self.start_btn.setEnabled(True)
            self._update_status(f"Connected to ws://{self.ip_input.text().strip()}:81")
            return

        self.status_label.setText(text)
        logger.info("Status: %s", text)

        if "Closed" in text or "Error" in text:
            self._disconnect_all()

    # ──────────────────────────────────────────────────────────────
    # CSV Export — complete data columns, correct axes per technique
    # ──────────────────────────────────────────────────────────────
    def _export_csv(self):
        if not self.rawData:
            QMessageBox.information(self, "No Data", "No data to export yet.")
            return

        path, _ = QFileDialog.getSaveFileName(
            self, "Save Data File", "",
            "CSV Files (*.csv);;All Files (*)")
        if not path:
            return

        try:
            idx = self.method_select.currentIndex()
            with open(path, "w", newline="", encoding="utf-8") as f:
                writer = csv.writer(f)

                if idx == 3:  # EIS
                    writer.writerow([
                        "Frequency (Hz)", "Real Z (Ω)", "Imag Z (Ω)",
                        "Magnitude (Ω)", "Phase (deg)"])
                    for d in self.rawData:
                        writer.writerow([
                            d.get("frequency", ""),
                            d.get("realZ",      ""),
                            d.get("imagZ",      ""),
                            d.get("magnitude",  ""),
                            d.get("phase",      ""),
                        ])
                elif idx == 1:  # CA — time axis
                    writer.writerow([
                        "Time (s)", "Voltage (V)", "Current (A)"])
                    for d in self.rawData:
                        writer.writerow([
                            d.get("time",    ""),
                            d.get("voltage", ""),
                            d.get("current", ""),
                        ])
                elif idx == 2:  # SWV — includes diffCurrent
                    writer.writerow([
                        "Voltage (V)", "Current (A)", "Diff Current (A)", "Time (s)"])
                    for d in self.rawData:
                        writer.writerow([
                            d.get("voltage",     ""),
                            d.get("current",     ""),
                            d.get("diffCurrent", ""),
                            d.get("time",        ""),
                        ])
                else:  # CV
                    writer.writerow([
                        "Time (s)", "Voltage (V)", "Current (A)", "Diff Current (A)"])
                    for d in self.rawData:
                        writer.writerow([
                            d.get("time",        ""),
                            d.get("voltage",     ""),
                            d.get("current",     ""),
                            d.get("diffCurrent", ""),
                        ])

            QMessageBox.information(
                self, "Exported",
                f"Data exported to:\n{path}\n({len(self.rawData)} rows)")
            logger.info("Data exported to %s (%d rows)", path, len(self.rawData))

        except OSError as e:
            logger.error("Export failed: %s", e)
            QMessageBox.critical(self, "Export Error", f"Failed to save:\n{e}")


# ──────────────────────────────────────────────────────────────────
# Entry point
# ──────────────────────────────────────────────────────────────────
if __name__ == "__main__":
    app = QApplication(sys.argv)
    app.setFont(QFont("Segoe UI", 10))
    window = PotentiostatGUI()
    window.show()
    sys.exit(app.exec())

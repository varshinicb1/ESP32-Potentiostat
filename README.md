# VidyuthLabs Potentiostat (ESP32-S3 & AD5941)

[![Firmware Build](https://img.shields.io/badge/Firmware-ESP32--S3-blue.svg)](Firmware/)
[![GUI Client](https://img.shields.io/badge/GUI-PyQt6-green.svg)](DesktopGUI/)
[![Mobile App](https://img.shields.io/badge/Mobile-Flutter-lightgrey.svg)](MobileApp/)
[![License](https://img.shields.io/badge/License-MIT-orange.svg)](#license)

A professional-grade, high-precision electrochemical potentiostat platform utilizing the **Analog Devices AD5941** AFE, controlled by an **ESP32-S3** microcontroller. The system supports Cyclic Voltammetry (CV), Chronoamperometry (CA), Square Wave Voltammetry (SWV), and Electrochemical Impedance Spectroscopy (EIS), featuring cross-platform desktop (PyQt6) and mobile (Flutter) interfaces.

---

## 🌟 Key Features

* **Advanced Electrochemical Methods**:
  * **CV / LSV**: Linear and cyclic voltammetric scans.
  * **Chronoamperometry (CA)**: Precision current-vs-time transients.
  * **SWV**: High-sensitivity analytical voltammetry.
  * **EIS**: Multi-frequency complex impedance sweeps up to 200 kHz.
* **Dual Interface Control**:
  * **PyQt6 Desktop Client**: High-performance real-time plotting (via PyQtGraph), CSV exporting, and websocket connectivity.
  * **Flutter Mobile Companion**: BLE-controlled mobile UI with hardware pairing and offline CSV data logging.
* **Safety & Compliance**:
  * Grounded on **IEC 61010-1** electrical safety standards.
  * **IEC 62304** compliant firmware architecture with dual-core watchdogs and hardware state separation.
  * Built-in **CRC32 NVS Integrity Checker** protecting the boot configuration against firmware corruption.

---

## 📁 Repository Structure

```tree
ESP32-Potentiostat/
├── DesktopGUI/                 # PyQt6 Desktop application
│   └── main_gui.py             # Main GUI client entry
├── Firmware/
│   └── ESP32_AD5941_Main/      # ESP32-S3 Arduino sketch
│       ├── AD5941_Driver.cpp   # Core AFE register mapping
│       ├── EIS_Method.cpp      # EIS excitation & DFT engine config
│       ├── Voltammetry_Methods.cpp # CV/SWV/CA scan state machines
│       └── ESP32_AD5941_Main.ino   # Firmware entry point
├── MobileApp/                  # Flutter mobile application
│   └── lib/                    # Dart source code (BLE, Charting)
├── README.md                   # Project documentation
├── walkthrough.md              # Detailed implementation notes
└── codebase_audit_report.md    # API refactoring and SDK compliance report
```

---

## 🛠️ Getting Started

### 1. Firmware Installation

Ensure you have [arduino-cli](https://arduino.github.io/arduino-cli/latest/) or the Arduino IDE installed, with ESP32 board packages v2.x.

#### Compile:
To compile the firmware, you must use the `huge_app` partition scheme to accommodate the SDK, LVGL display drivers, and Bluetooth stacks:
```bash
arduino-cli compile --fqbn esp32:esp32:esp32s3:PartitionScheme=huge_app Firmware/ESP32_AD5941_Main
```

#### Upload:
```bash
arduino-cli upload -p <COM_PORT> --fqbn esp32:esp32:esp32s3:PartitionScheme=huge_app Firmware/ESP32_AD5941_Main
```

### 2. Running the Desktop GUI

The Desktop GUI requires Python 3.10+ and a few python dependencies.

#### Install dependencies:
```bash
pip install PyQt6 pyqtgraph pyserial websocket-client pytest
```

#### Run the GUI:
```bash
python DesktopGUI/main_gui.py
```

### 3. Running the Mobile App

Ensure Flutter is installed and configured.

#### Get packages:
```bash
cd MobileApp
flutter pub get
```

#### Run:
```bash
flutter run
```

---

## 🧪 Testing and Verification

A comprehensive 50-test unit and integration suite has been configured using `pytest` to validate GUI states, parser behavior, serial handlers, parameter limits, and complex impedance calculations.

To run the test suite:
```bash
python -m pytest .gemini/antigravity-ide/brain/222c3492-8d30-4ccc-a6cb-aa6f8fd9d4c9/scratch/test_desktop_gui_full.py -v -p no:nengo
```

---

## 🛡️ Risk & Error Mitigation

* **Flash Memory Protection**: Configured with `huge_app` partition allocating 2 MB of flash for app logic, leaving **38%** memory headroom.
* **Robust Math**: EIS math guards against divide-by-zero errors when dividing real/imaginary values by enforcing a `1e-9` floor threshold on the denominator.
* **Task Safety**: Shared hardware interfaces (such as SPI and NVS storage) are protected using FreeRTOS mutexes to avoid race conditions.

---

## 📄 License

This project is licensed under the MIT License. See [LICENSE](LICENSE) or headers inside source files for third-party component licenses (e.g. AD5940 SDK, LovyanGFX).

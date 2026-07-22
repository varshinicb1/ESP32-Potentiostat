# AnalyteX — First Hardware Bring-Up Checklist

First-session guide for flashing this firmware to a real board and finding out
where it breaks **fast**, in the right order. Everything here is compile- and
logic-verified but has **never run on silicon** — the point of this document is
to turn "it doesn't work" into "step N failed, check file X".

Work top to bottom. **Do not connect a real sample or real electrodes until
Stage 4.** Use a resistor dummy-cell first.

---

## 0. Before you plug anything in

- [ ] **Set `RCAL_OHM`** in [`Firmware/ESP32_AD5941_Main/AD5941_Driver.h`](../Firmware/ESP32_AD5941_Main/AD5941_Driver.h) to the *exact* value of the precision resistor you soldered into the 2-pin RCAL placeholder. It's currently `10000.0f` (10 kΩ). **Every EIS impedance reading is scaled by this** — wrong value = every impedance off by a constant factor. This is the single most important one-line change before flashing.
- [ ] Recommended RCAL resistor: **10 kΩ, 0.1 %, 25 ppm/°C** (thin-film). Matches the default and sits mid-range for the gain settings in use.
- [ ] Serial monitor ready at **115200 baud**.
- [ ] Confirm the board's USB/UART port (COM number on Windows).

**Flash:**
```bash
arduino-cli upload -p <COM_PORT> --fqbn esp32:esp32:esp32s3:PartitionScheme=huge_app Firmware/ESP32_AD5941_Main
```
(Compile first if you changed `RCAL_OHM`:
`arduino-cli compile --fqbn esp32:esp32:esp32s3:PartitionScheme=huge_app Firmware/ESP32_AD5941_Main`)

---

## Pin map (what to probe if a subsystem is dead)

All verified against the schematic/netlist, **not** against a running board. If a
stage below fails, meter-check the relevant pins first.

| Signal | GPIO | Notes |
|---|---|---|
| AD5941 SCLK / MOSI / MISO | 12 / 11 / 13 | **Shared bus** — also display, touch, SD |
| AD5941 CS | 15 | |
| AD5941 RESET | 10 | |
| AD5941 INT (GP0) | 4 | interrupt-out from AFE |
| AD5941 "relay" | 8 | **NOT wired — no relay on this board** |
| TFT SCLK/MOSI/MISO | 12 / 11 / 13 | same shared bus |
| TFT DC | 40 | |
| TFT CS | 38 | |
| TFT RST | 39 | |
| TFT backlight | 41 | (was GPIO45 — a strapping pin — do not revert) |
| Touch CS | 42 | XPT2046 |
| Touch INT | 47 | |
| microSD CS | 14 | |

---

## Stage 1 — It boots and POST runs

Power on, watch serial. You are looking for this banner:

```
========================================
 AnalyteX — POST
 Firmware: <version>
========================================
[BOOT] POST attempt 1 of N
```

- [ ] **Banner appears.** If nothing prints → wrong baud, wrong port, or a boot-strapping-pin conflict. Not a firmware-logic problem.

Then each POST step prints. Expected happy path:

```
[POST] Verifying firmware CRC32... PASS (0x........)
[POST] Verifying RAM integrity... PASS
[POST] Initializing AD5941 and running diagnostics... PASS — POST PASS. Temp: 25.xC, Vref: 1.8xx V
[POST] Relay line GPIO 8 driven LOW (no relay populated on this board)
[POST] Checking SD card... PASS — SD card mounted        (or WARN if no card)
[POST] Device UID: ....
POST RESULT: ALL CHECKS PASSED
```

> **First-boot note on CRC32:** the integrity checker anchors its reference CRC
> on first boot, so step 1 should PASS. If you ever see `FAIL — Firmware image
> corrupted or tampered!` on a *fresh* flash, it's the NVS reference from a
> previous image — erase NVS/flash and reflash, don't chase a phantom.

---

## Stage 2 — The AD5941 SPI check (your most important diagnostic)

This is **the** make-or-break line:

```
[POST] Initializing AD5941 and running diagnostics... PASS — POST PASS. Temp: ..., Vref: ...
```

What it actually does (`AD5941_Driver::performPOST`): writes `0x1A5` to a scratch
register (`REG_AFE_REPEATADCCNV`), reads it back, then checks Vref ∈ [1.70, 1.95] V
and die-temp ∈ [−40, 85] °C.

| Serial output | Meaning | Fix |
|---|---|---|
| `PASS — POST PASS. Temp/Vref...` | AD5941 SPI + analog core alive | proceed |
| `FAIL — SPI register mismatch. Expected 0x1a5, got 0x...` | **Pins wrong or bus not reaching the chip.** `got 0x0` or `0xFFFF` = classic dead-MISO / wrong CS. | Meter-check CS=15, SCK=12, MOSI=11, MISO=13, RESET=10 against the AD5941. This is the #1 expected first-boot failure. |
| `FAIL — Vref out of bounds: X V` | SPI works but analog reference is off | check AD5941 power rails, AVDD decoupling |
| `FAIL — Die temp out of bounds` | usually a bad reads-as-garbage symptom | treat like SPI mismatch |

If POST fails all retries you'll see:
```
!!! CRITICAL: POST FAILED AFTER ALL RETRIES !!!
!!! SYSTEM HALTED IN SAFE STATE !!!
```
**Stop here and fix the AD5941 link before anything else.** Nothing downstream
matters if the AFE isn't talking.

---

## Stage 3 — Display, touch, radios (no electrodes yet)

- [ ] **Main LVGL screen appears** on the TFT: "AnalyteX" title, subtitle, four
  technique buttons (CV / CA / SWV / EIS). The layout is render-verified, so if
  the panel driver works, it will look like
  [`docs/screenshots/lvgl_main_screen.png`](screenshots/lvgl_main_screen.png).

  | Symptom | Likely cause |
  |---|---|
  | Blank / white screen | backlight (GPIO41) or CS=38 / DC=40 / RST=39 wrong |
  | Garbled / snow | SPI mode or the shared-bus mutex (see Stage 5) |
  | Mirrored / rotated | `setRotation()` in `Display_LVGL.cpp` — cosmetic, easy |
  | No touch response | touch CS=42 / INT=47, or calibration `x_min/x_max/y_min/y_max` in the `LGFX_Potentiostat` constructor |

- [ ] **BLE:** phone sees `AnalyteX` in the scan list (mobile app → search icon).
- [ ] **WiFi AP:** `AnalyteX_AP` appears; password is `VL-` + the device's eFuse
  MAC (printed as Device UID in POST — derive it from there, it is per-unit).
- [ ] App connects and the connection banner goes green.

---

## Stage 4 — First measurement: DUMMY CELL, not a sample

**Connect a known resistor as the cell first** — this is the cleanest possible
sanity check of the current path. Put e.g. a **10 kΩ** resistor between the
Working and Counter/Reference terminals (a 2-terminal Ohmic "cell").

- [ ] Run a **CV** (manual mode, or the app default: −0.5 → +0.8 / −0.8 V, 0.1 V/s).
- [ ] Expected: a **straight, ohmic I–V line** (no peaks) with slope **I = V / R**.
  For a 10 kΩ resistor at ±0.5 V you should see roughly **±50 µA**.
  - Slope wildly wrong (but linear) → LPTIA gain / RTIA mismatch → check
    `LPTIARTIA_10K` and the `LpTiaSW` note below.
  - Not linear / railed / zero → LP-loop electrode routing not reaching the pins.
- [ ] Confirm data streams to **both** the app chart **and** the on-device chart,
  and that a CSV appears on the SD card.

Only once the resistor reads sensibly:

- [ ] **Run the CAL command** (`{"method":"CAL"}`) with a precision resistor equal
  to `RCAL_OHM` connected as the cell. This drives the full EIS/HSTIA path against
  a known load and reports gain/phase. **A result far from 1.0× gain / 0° phase**
  means either the internal RCAL0/1 resistance doesn't match `RCAL_OHM`, or the
  EIS switch-matrix routing isn't reaching the electrodes — fix before trusting
  any EIS number. (This checks the HSTIA/EIS path only; CV/CA/SWV use the separate
  LPTIA path, which the dummy-resistor CV above is the check for.)

---

## Stage 5 — The things most likely to need iteration

These are the known-uncertain items, each with **how it shows up** so you can
recognise it at the bench:

1. **Shared SPI bus under real scheduling** (`SharedSPIBus`). AD5941 + display +
   touch + SD share GPIO11/12/13 across two cores. Never run concurrently on
   hardware.
   **Symptom:** works fine when idle, but the display garbles *or* measurement
   data glitches specifically **when a scan is running and you touch the screen**.
   If you see that, it's the mutex, not the sensor.

2. **`LpTiaSW` one-bit ambiguity.** The code uses bits {2,4,5,12,13} from ADI's
   `Amperometric.c`; the datasheet's Table 21 shows {2,3,5,12,13} (0x302C).
   **Symptom:** the dummy-resistor CV in Stage 4 reads a consistent but *wrong*
   current (e.g. right shape, wrong scale, or an offset). If so, try the datasheet
   value in `AD5941_Driver::configureAFE()` and re-run the resistor test.
   (The disambiguating info is only in datasheet Figure 22, an image.)

3. **EIS phase accuracy / N-switch.** Routing is datasheet-confirmed (`SWN_SE0` =
   the N9 switch), but end-to-end phase can only be trusted after the Stage-4 CAL
   check on a known resistor reads ~0° phase.

4. **Absolute accuracy in general.** Nothing here is calibrated against a
   reference instrument. Before trusting a real result, run the same dummy load on
   your lab potentiostat and compare.

---

## Stage 6 — Only now, a real electrode

- [ ] Real SPE / electrode, real supporting electrolyte, **no analyte** → confirm a
  clean baseline (blank).
- [ ] Known-concentration standard → confirm a response in the expected direction.
- [ ] Cross-check against the lab potentiostat on the **same** cell before believing
  absolute numbers.
- [ ] Only then: real samples.

---

## Safety reality check

There is **no galvanic isolation relay** on this board. "Safe state" =
`enterSafeState()` opens the AD5941 AFE switch matrix, stops the waveform
generator, and parks the DAC at 0 V — electrically safe (no driving potential),
but **not** a physical disconnect. The cell is returned to 0 V at the end of every
measurement and on abort. Don't build a safety case around a relay that isn't
populated. GPIO8 toggles are forward-compat no-ops for a future relay revision.

---

## Quick failure → location index

| You see | Look at |
|---|---|
| Nothing on serial | baud/port/strapping pins |
| `SPI register mismatch` | AD5941 CS/SCK/MOSI/MISO/RESET pins |
| `Vref out of bounds` | AD5941 power/decoupling |
| Blank TFT | backlight GPIO41, panel CS/DC/RST |
| TFT garbles only during a scan | `SharedSPIBus` mutex |
| CV on resistor: wrong scale | `LpTiaSW` / `LPTIARTIA_10K` in `configureAFE()` |
| EIS magnitudes off by constant | `RCAL_OHM` in `AD5941_Driver.h` |
| EIS phase wrong | CAL check + `EIS_SWITCH_CELL_*` in `EIS_Method.cpp` |
| No SD logging | `SD_CS_PIN` (GPIO14) in `Storage.h` |

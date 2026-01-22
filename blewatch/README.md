# Waveshare ESP32-C6 1.47" LCD — BleWatch
<img width="279" height="305" alt="image" src="https://github.com/user-attachments/assets/8a087e03-83cd-454a-91df-d3c52a6a983d" />


BleWatch is a **BLE proximity meter** with vendor vulnerability indication for the ESP32-C6 + 1.47" LCD. It scans for nearby Bluetooth Low Energy devices and provides visual feedback on proximity and potential security concerns.

## Features

- **BLE device scanning**: Continuously scans for nearby BLE devices using the standard ESP32 BLE library.
- **Proximity detection**: Shows distance state based on RSSI thresholds.
- **Device identification**: Displays device name (if advertised) or MAC address.
- **Vendor vulnerability check**: After 3 seconds in VERY CLOSE range, checks if the device's OUI matches vendors with known historical BLE CVEs.
- **RGB LED feedback**: Color-coded LED indicates proximity and security status.

## Proximity States

| State | RSSI Range | Bar | LED | Description |
|-------|------------|-----|-----|-------------|
| FAR | < −80 dBm | 0% | Off | No nearby devices or signal too weak |
| TOO FAR | −80 to −67 dBm | 0% | Orange | Device detected but too far for reliable tracking |
| NEAR | −67 to −50 dBm | 0–70% | Pulsing green | Device in range, pulse speed increases with proximity |
| CLOSE | −50 to −40 dBm | 70–100% | Cyan | Device is close |
| VERY CLOSE | ≥ −40 dBm | 100% | Blue → see below | Device is very close, name/MAC displayed |

## Vulnerability Check (VERY CLOSE only)

When a device stays in VERY CLOSE range for **3 seconds**, the OUI (first 3 bytes of MAC) is checked against vendors historically affected by BLE vulnerabilities (BlueBorne, KNOB, etc.):

| Result | Label Color | LED Behavior |
|--------|-------------|--------------|
| Checking... (< 3s) | Cyan | Steady blue |
| Potentially vulnerable | **Red** | Steady **red** |
| Not in vulnerable list | **Green** | **Blinks green twice**, then steady blue |


## Current Behavior
| Multiple devices | Multiple OUIs | Displayed | Sorting | Initial scan |
|------------------|---------------|-----------|---------|--------------|
| Tracks up to 64 devices internally| Checks against 39 OUI entries|Only shows 1 device (the strongest RSSI in VERY CLOSE)|By RSSI (strongest first), not MAC|No — jumps straight to live mode|

### Flagged Vendors (OUI list)

This list includes vendors with **documented BLE CVEs** (BlueBorne, SweynTooth, BrakTooth, KNOB, etc.) whose chips are typically found in devices **without over-the-air update capability** — meaning vulnerabilities often remain unpatched throughout the device's lifetime.

| Vendor | CVEs | Typical Devices |
|--------|------|-----------------|
| **Qualcomm / Atheros** | BlueBorne, BlueFrag | Older phones, tablets |
| **Broadcom** | BlueBorne, various stack bugs | Older phones, smart TVs |
| **Texas Instruments** | SweynTooth, BrakTooth | Medical devices, sensors |
| **CSR (now Qualcomm)** | Various legacy | Bluetooth dongles, speakers |
| **Dialog Semiconductor** | SweynTooth | Wearables, IoT sensors |
| **Microchip** | SweynTooth | Industrial embedded devices |
| **Silicon Labs** | BrakTooth | Industrial IoT, beacons |
| **Cypress (older)** | SweynTooth | Legacy embedded products |
| **Telink Semiconductor** | SweynTooth (CVE-2019-16336, CVE-2019-19194) | Cheap BLE devices, earbuds |
| **STMicroelectronics (BlueNRG)** | SweynTooth (CVE-2019-17519) | Medical, industrial devices |
| **NXP** | SweynTooth (CVE-2019-17517, CVE-2019-17518) | Embedded systems |
| **Samsung (older chipsets)** | BlueBorne (via Broadcom/Qualcomm) | Older phones, tablets |
| **Generic BT dongles** | Various (use CSR/Broadcom chips) | USB Bluetooth adapters |

> **Why these vendors?** These chips are commonly found in:
> - IoT devices without update mechanisms
> - Medical/industrial equipment with long lifecycles
> - Consumer electronics that never receive firmware updates
> - Cheap Bluetooth accessories (dongles, earbuds, speakers)
>
> Vendors like Apple, Intel, and modern Android phones are **not** flagged because they actively push security patches via OS updates.

> **Important**: Red means the vendor has shipped vulnerable firmware in the past — the specific device may have been patched. Green means the vendor is not in our list — it does not guarantee the device is secure.

## Sticky Device Selection

When multiple devices are in range, BleWatch "locks on" to the current VERY CLOSE device and only switches if:
- The current device drops below VERY CLOSE threshold, or
- Another device is **10+ dBm stronger**

This prevents the display from jumping between devices due to RSSI fluctuations.

## On-Screen Layout

- **Header**: "BLEwatch" title
- **Nearby devices**: Count of active BLE devices in range
- **Proximity bar**: Visual 0–100% indicator
- **RSSI**: Current best signal strength in dBm
- **State label**: FAR / TOO FAR / NEAR / CLOSE / VERY CLOSE
- **Name/MAC label**: Shown in VERY CLOSE, color indicates security status

## Configuration (in `blewatch.cpp`)

| Constant | Default | Description |
|----------|---------|-------------|
| `kFarRssiDbm` | −80 | Below this = FAR |
| `kNearStartRssiDbm` | −67 | Start of NEAR range |
| `kCloseStartRssiDbm` | −50 | Start of CLOSE range |
| `kVeryCloseRssiDbm` | −40 | Start of VERY CLOSE range |
| `kVulnCheckDwellMs` | 3000 | Milliseconds before vulnerability check |
| `kStickyRssiMarginDb` | 10 | dB margin for switching displayed device |
| `kRgbPin` | 8 | WS2812 RGB LED pin |
| `kDeviceStaleMs` | 3500 | Device timeout for "active" status |

## Build / Flash (Arduino IDE)
https://www.waveshare.com/wiki/ESP32-C6-LCD-1.47
1. Select the **ESP32-C6** board profile.
2. Open `blewatch.ino` and ensure dependencies are installed:
   - `LVGL`
   - `Adafruit_NeoPixel`
   - `ESP32 ` library
3. Flash to the board; scanning begins automatically when device is being booted.

## Limitations

- **Not a security scanner**: OUI-based checking is a heuristic, not a vulnerability test.
- **No active probing**: Only passive advertisement scanning; cannot detect patched firmware.
- **Name availability**: Many BLE devices don't advertise names; MAC is shown as fallback.
- **RSSI ≠ distance**: Signal strength varies with obstacles, orientation, and interference.

## Example

- Flipper Zero triggered red because it uses an STMicroelectronics STM32WB55 chip, and its MAC address starts with an STMicro OUI (likely 00:80:E1 or similar) that's in our list.

Why It Matched
Flipper Zero	Our List Entry
Uses STM32WB55 (STMicro chip)	STMicroelectronics flagged for BlueNRG CVE-2019-17519
MAC starts with STMicro OUI	We have 00:80:E1, 80:E1:26, 02:80:E1
Is Flipper Zero Actually Vulnerable?
Probably not — this is a limitation of OUI-based detection:

Different chip: The STM32WB55 uses a different BLE stack than BlueNRG (which had the CVE)
Gets updates: Flipper Zero has OTA updates via qFlipper app
Active security: The Flipper team patches vulnerabilities
The Problem
We're flagging all STMicro OUIs because:

- We can't tell STM32WB55 from BlueNRG by MAC alone
- Many STMicro-based IoT devices don't get updates
- Options
- Keep as-is: Accept some false positives (conservative approach)
- Remove STMicro: Reduces false positives but misses vulnerable BlueNRG devices- 
Add note: Explain that actively-updated devices (like Flipper) are likely safe despite the warning

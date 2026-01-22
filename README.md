# Waveshare ESP32-C6 1.47" LCD Projects

This repository contains applications developed for the **Waveshare ESP32-C6 Development Board** equipped with a **1.47" LCD display**.

The projects leverage the ESP32-C6's capabilities (Wi-Fi 6, BLE 5) and the onboard display to create useful monitoring tools.

## Projects

### 1. [Bandwatch](./bandwatch)
A Wi-Fi activity monitor that observes 802.11 traffic.
- **Features**: Promiscuous mode capture, channel hopping (1-13), per-channel metrics, and a "busy score" visualization.
- **Use case**: identifying busy Wi-Fi channels and checking signal activity.
- 👉 [Read more](./bandwatch/README.md)

### 2. [BleWatch](./blewatch)
A Bluetooth Low Energy (BLE) proximity meter and scanner.
- **Features**: Scans for nearby BLE devices, visualizes proximity using RSSI, and performs a basic vendor vulnerability check based on OUI.
- **Use case**: Detecting nearby BLE devices and checking if their vendor chipset has known historical vulnerabilities.
- 👉 [Read more](./blewatch/README.md)

## Hardware
- **Board**: [Waveshare ESP32-C6-LCD-1.47](https://www.waveshare.com/esp32-c6-lcd-1.47.htm)
- **SoC**: ESP32-C6 (RISC-V 32-bit, Wi-Fi 6, BLE 5, Zigbee/Thread)
- **Display**: 1.47" IPS LCD (172×320), ST7789 driver
- **Peripheral**: Onboard RGB LED (WS2812)

## Getting Started

Each project folder (`bandwatch` and `blewatch`) contains its own source code and references a local `README.md` for specific operating instructions.

### 1. Prerequisites (Arduino IDE)

To build and flash these projects, you need the Arduino IDE referencing the ESP32-C6 board definitions.

#### A. Install ESP32 Board Support
1. Open **Arduino IDE**.
2. Go to **File > Preferences**.
3. In "Additional Boards Manager URLs", add:
   ```
   https://espressif.github.io/arduino-esp32/package_esp32_index.json
   ```
4. Go to **Tools > Board > Boards Manager**.
5. Search for **esp32** (by Espressif Systems) and install the latest version (3.0.0 or newer recommended for C6 support).

#### B. Select the Board
1. Go to **Tools > Board > ESP32 Arduino**.
2. Select **ESP32C6 Dev Module** (or *Waveshare ESP32-C6-LCD-1.47* if available in your version).
3. configure the settings (typical defaults):
   - **Upload Speed**: 921600 or 115200
   - **USB CDC On Boot**: Enabled (allows Serial output over USB)
   - **Flash Mode**: DIO

### 2. Install Required Libraries

Use the **Library Manager** (*Tools > Manage Libraries...*) to install:

| Library | Version Note |
| :--- | :--- |
| **lvgl** | Install version **9.x** (Source code uses `lv_display_t`, which is LVGL v9+ syntax). |
| **Adafruit NeoPixel** | Latest version. |

> **Note on LVGL Config**: The projects include a local `lv_conf.h`. The source code uses `#define LV_CONF_INCLUDE_SIMPLE` to try and include this local configuration automatically. If you encounter errors about `lv_conf.h` not being found, ensure the library is installed and that the compiler is picking up the local header.

### 3. Build and Flash
1. Open the `.ino` file from the project folder (e.g., `bandwatch/bandwatch.ino`).
2. Connect your ESP32-C6 board via USB.
3. Select the correct **Port** in Tools.
4. Click **Upload**.

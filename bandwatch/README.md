# WaveshareESP32C6LCD
Waveshare ESP32-C6 1.47" LCD Dev Board

## Bandwatch (Wi-Fi air quality)

Bandwatch is a simple “how crowded is the Wi‑Fi air around me?” view. It doesn’t measure true RF power; it estimates **how many networks are nearby and how strong they are**.

### What’s on screen

- **Bar (0–100%)**: The main “crowdedness” indicator.
	- **Green**: quiet air (few and/or weak nearby networks)
	- **Yellow**: moderately busy
	- **Red**: very noisy (many and/or strong nearby networks)

- **WIFI _N_ APs**: How many access points were found in the *last completed scan*.
	- More APs usually pushes the bar up, especially if their signals are strong.

- **DEC 0–255**: The same interference level, shown as a byte (base‑10).
	- `0` means “essentially quiet”.
	- `255` means “very crowded” (according to this heuristic).
	- This is just a convenient compact number for display and the binary row.

- **HEX 0x00–0xFF**: The same byte as **DEC**, but displayed in hexadecimal.
	- Example: `DEC 170` equals `HEX 0xAA`.
	- If you’re not used to hex, you can ignore this—it's the same info.

- **Bits row (8 boxes)**: The DEC/HEX byte shown in binary.
	- Leftmost box is the **most significant bit** (bit 7), rightmost is bit 0.
	- **Purple** = `1`, dark = `0`.
	- This is mainly for a “techy” visual pattern; it’s still the same value.

### How the interference number is computed

1. The ESP32 does a Wi‑Fi scan and collects the RSSI (signal strength) for each network.
2. Each network contributes a weight based on RSSI:
	 - around **−100 dBm** contributes ~0
	 - around **−40 dBm** contributes ~1
	 - values are clamped to 0–1
3. Those weights are summed and normalized so that “about a dozen strong networks” feels like “very noisy”.

This produces a raw value in the range 0–1. That value is then:
- shown as **Bar %** (`0–100`)
- mapped to a byte for **DEC/HEX/Bits** (`0–255`)

### Smoothing and update timing

- **Scan interval**: about every **3.5 seconds** (asynchronous scan; the UI stays responsive).
- **Smoothing**: the displayed value is an **exponential moving average** of the raw scan result, so the bar doesn’t jump wildly when one scan sees more/fewer networks.
- **UI refresh**: roughly every **120 ms**; between scans it mostly re-draws the same smoothed value.

### Layout

Designed for the **172×320** display:
- Title header at the top
- Interference bar centered vertically
- Text readouts below
- Binary boxes near the bottom (wrapping to fit the narrow width)

# Waveshare ESP32-C6 1.47" LCD — Bandwatch

Bandwatch is a Wi‑Fi **activity** meter for the ESP32‑C6 + 1.47" LCD. It observes 802.11 traffic in promiscuous mode and reports a **busy score** as a proxy for channel busyness. It does **not** measure RF power or calibrated airtime.

## Measurement pipeline

- **Promiscuous capture**: counts real 802.11 frames (no decryption).
- **Channel hopping**: channels **1–13** with ~**260 ms** dwell; full sweep in ~3–4 s.
- **Per‑channel metrics** every dwell: frames, bytes, “strong” frames (RSSI ≥ −65 dBm), and best‑effort unique transmitters (hashed MACs in fixed slots).
- **Busy score (0–100)**: log‑scaled packets/s, bytes/s, strong‑frame proportion, and unique‑talker estimate.
- **Smoothing**: exponential moving average (α ≈ **0.22**) on the busy score only; raw counters are not smoothed.
- **Global activity**: **maximum** of the smoothed channel scores (stated in the UI).

## On-screen layout (compact)

- **Global bar**: 0–100 with green → yellow → red ramp.
- **Top 3**: busiest channels with smoothed score plus last dwell counts (packets, strong, unique).
- **Channel strip**: channels 1–13 with mini bars; active dwell channel marked with `*`.
- **Dwell line**: current channel, dwell time, live packet and byte counts during the ongoing window.

## Configuration knobs (in `Tamagotchi.cpp`)

- `kDwellMs` (default 260 ms): per‑channel dwell; keep 200–400 ms.
- `kStrongThresholdDbm` (default −65 dBm): strong-frame cutoff.
- `kBusyEmaAlpha` (default 0.22): busy-score smoothing (target 0.15–0.30).
- `kChannelCount` (default 13): set to 11 if you only need channels 1–11.

## Performance and safety

- Promiscuous callback only counts and hashes (no dynamic allocation, no UI work).
- Fixed-size structures: 13 channels × bounded unique MAC slots.
- UI timers keep rendering responsive during hopping.

## What Bandwatch does *not* do

- It does **not** measure true airtime occupancy.
- It does **not** detect non‑Wi‑Fi interference (Bluetooth, Zigbee, microwaves, etc.).
- It does **not** replace professional RF analysis tools or calibrated spectrum measurements.

## Quick validation

- Start a video stream or large file download near the device; the serving channel’s score should rise.
- Add multiple active clients on the same channel; top‑3 should reshuffle to include that channel.
- In a quiet environment, scores should stay low and stable after smoothing.

## Build / flash (Arduino IDE)

1. Select the **ESP32-C6** board profile.
2. Open `WaveshareESP32C6LCD.ino` and ensure `LVGL` and display dependencies are installed.
3. Flash to the board; the UI should appear and begin hopping within a few seconds.

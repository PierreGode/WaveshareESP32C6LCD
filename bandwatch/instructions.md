Project goal

Upgrade the existing “Bandwatch” project from a heuristic AP-count demo into a legitimate Wi-Fi activity / channel busyness meter using the existing ESP32-C6 + 1.47” LCD hardware and Arduino IDE.

The project must not claim to measure RF power, interference, or air quality.
It must measure observed 802.11 activity as a proxy for channel load.

⸻

Definitions (must be used consistently)
	•	Wi-Fi activity: observed 802.11 frame traffic received by the ESP32 radio
	•	Channel busyness: relative activity level on a specific Wi-Fi channel
	•	Busy score: normalized 0–100 value derived from real traffic metrics
	•	Proxy: an estimate based on observable traffic, not calibrated airtime

Forbidden terms:
	•	“air quality”
	•	“interference level”
	•	“RF noise”
	•	any claim of absolute or calibrated measurement

⸻

Measurement strategy (core requirement)

Do not rely on Wi-Fi scan results alone.

The system must use Wi-Fi promiscuous receive mode to observe real traffic.

Per Wi-Fi channel, the following metrics must be collected during a fixed dwell window:
	•	Number of received 802.11 frames
	•	Total bytes received
	•	Count of “strong” frames (RSSI above a defined threshold, e.g. −65 dBm)
	•	Approximate number of unique transmitters (best-effort via source MAC)

No decryption is required or expected.

⸻

Channel hopping model
	•	Channels: 1–13 (2.4 GHz)
	•	Operate in a continuous loop
	•	Dwell time per channel: 200–400 ms
	•	One full sweep should complete in ~3–6 seconds
	•	UI must remain responsive during hopping

Statistics must reset per channel at the start of each dwell window.

⸻

Busy score computation (conceptual, not exact math)

For each channel, compute a busy score (0–100) derived from:
	•	Packets per second (log-scaled)
	•	Bytes per second (log-scaled)
	•	Proportion of strong frames
	•	Number of unique transmitters (soft-capped)

The score must:
	•	Be normalized
	•	Be stable across very quiet and very busy environments
	•	Reflect real traffic changes (e.g. video streaming increases score)

⸻

Smoothing and time behavior
	•	Apply exponential moving average (EMA) after computing the busy score
	•	EMA alpha target range: 0.15–0.30
	•	Do not smooth raw packet counters
	•	Smoothing exists only to improve visual stability

⸻

UI requirements (LCD)

The display must prioritize actionable information:

Required elements:
	•	Top 3 busiest channels with scores
	•	A global activity indicator derived from channel scores
	•	Optional compact visualization of channels 1–13

Global activity must be defined explicitly as one of:
	•	Maximum channel score
	•	Median channel score
	•	Weighted average of the busiest channels

⸻

Use of Wi-Fi scan results

Wi-Fi scans may be used only for contextual data:
	•	AP count per channel
	•	Strongest AP per channel
	•	Environment overview

Scan results must not be used as the main activity metric.

⸻

Performance and safety constraints
	•	Promiscuous callbacks must be lightweight (counting only)
	•	No display rendering or heavy logic in callbacks
	•	Use fixed-size data structures (no unbounded memory growth)
	•	Unique transmitter tracking is best-effort, not exact

⸻

Technical limitations (must be documented)

The project must clearly state that it:
	•	Does not measure true airtime occupancy
	•	Does not detect non-Wi-Fi interference (Bluetooth, Zigbee, microwave)
	•	Does not replace professional RF analysis tools

⸻

Validation expectations

The implementation should visibly react to:
	•	Active video streaming
	•	Large file transfers
	•	Multiple active clients on the same channel

A quiet environment should produce consistently low busy scores.

⸻

Documentation tone

Documentation and UI text must be:
	•	Honest
	•	Conservative
	•	Technically accurate

The project should be defensible to a networking or RF-savvy audience.

⸻

End of instructions.

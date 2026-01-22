#include "blewatch.h"

#include <Arduino.h>
#include <math.h>
#include <lvgl.h>
#include <Adafruit_NeoPixel.h>
#include <string>

#if __has_include(<NimBLEDevice.h>)
  #include <NimBLEDevice.h>
  #define BLEWATCH_USE_NIMBLE 1
#else
  #include <BLEDevice.h>
  #include <BLEScan.h>
  #define BLEWATCH_USE_NIMBLE 0
#endif

namespace {

constexpr uint32_t kUiIntervalMs = 40;
constexpr uint32_t kDeviceStaleMs = 3500;

// Scan tuning: smaller scan cycles + higher duty-cycle => faster state updates.
// Note: this will increase power consumption.
constexpr uint32_t kBleScanDurationS = 1;
constexpr uint16_t kBleScanInterval = 16;
constexpr uint16_t kBleScanWindow = 16;
constexpr uint32_t kBleScanLoopDelayMs = 5;

// LED (WS2812) config (matches Bandwatch defaults)
constexpr int kRgbPin = 8;
constexpr int kRgbCount = 1;
constexpr uint16_t kNeoPixelType = NEO_RGB + NEO_KHZ800;

// Proximity heuristics (RSSI is not distance; these are tunable)
constexpr int kFarRssiDbm = -80;        // below this -> treat as far/none
constexpr int kNearStartRssiDbm = -67;  // start pulsing green
constexpr int kCloseStartRssiDbm = -50; // close band starts (steady green)
constexpr int kVeryCloseRssiDbm = -40;  // ~"very close" heuristic => steady blue

// Pulse behavior
constexpr float kMinPulseHz = 0.35f;    // slow pulse when just "near"
constexpr float kMaxPulseHz = 2.60f;    // fast pulse when very near (but not very-close)
constexpr uint8_t kNearMinBrightness = 35;   // percent
constexpr uint8_t kNearMaxBrightness = 100;  // percent
constexpr uint8_t kNearAvgBrightness = 50;   // percent target for "ish close"

constexpr float kPi = 3.14159265358979323846f;

// Device table (fixed size, no heap churn in callbacks)
constexpr int kMaxDevices = 64;

struct DeviceSlot {
  uint8_t mac[6] = {0};
  uint32_t lastSeenMs = 0;
  int8_t lastRssi = -127;
  char name[32] = {0};
  bool used = false;
};

portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;
DeviceSlot g_devices[kMaxDevices];

// Stats updated from scan callback
volatile int g_bestRssi = -127;
volatile uint32_t g_lastAnySeenMs = 0;

// UI
lv_obj_t* g_root = nullptr;
lv_obj_t* g_title = nullptr;
lv_obj_t* g_countLabel = nullptr;
lv_obj_t* g_rssiLabel = nullptr;
lv_obj_t* g_stateLabel = nullptr;
lv_obj_t* g_nameLabel = nullptr;
lv_obj_t* g_bar = nullptr;

Adafruit_NeoPixel g_rgb(kRgbCount, kRgbPin, kNeoPixelType);

struct RgbColor { uint8_t r; uint8_t g; uint8_t b; };
constexpr RgbColor LED_OFF  = {0, 0, 0};
constexpr RgbColor LED_GREEN = {0, 180, 40};
constexpr RgbColor LED_ORANGE = {255, 90, 0};
constexpr RgbColor LED_CYAN = {0, 180, 180};
constexpr RgbColor LED_BLUE  = {0, 60, 255};
constexpr RgbColor LED_RED   = {255, 0, 0};

// OUI prefixes (first 3 bytes) of vendors historically affected by BLE vulnerabilities
// (BlueBorne, KNOB, etc.). This is indicative, not definitive.
struct OuiEntry { uint8_t oui[3]; };
constexpr OuiEntry kVulnerableOuis[] = {
  {{0x00, 0x1A, 0x7D}},  // Cyber-Blue (Bluetooth dongles)
  {{0x00, 0x02, 0x72}},  // CC&C Technologies (various BT chips)
  {{0x00, 0x25, 0xDB}},  // Qualcomm (various)
  {{0x9C, 0x8C, 0xD8}},  // Qualcomm
  {{0x00, 0x26, 0xE8}},  // Qualcomm Atheros
  {{0x00, 0x03, 0x7A}},  // Texas Instruments
  {{0xD0, 0x5F, 0xB8}},  // Texas Instruments
  {{0x34, 0xB1, 0xF7}},  // Broadcom
  {{0x00, 0x10, 0x18}},  // Broadcom
  {{0xAC, 0x37, 0x43}},  // Samsung (older devices)
  {{0x8C, 0xF5, 0xA3}},  // Samsung
  {{0x78, 0xD7, 0x5F}},  // Samsung
};
constexpr size_t kVulnerableOuiCount = sizeof(kVulnerableOuis) / sizeof(kVulnerableOuis[0]);

inline bool isOuiPotentiallyVulnerable(const uint8_t mac[6]) {
  for (size_t i = 0; i < kVulnerableOuiCount; i++) {
    if (mac[0] == kVulnerableOuis[i].oui[0] &&
        mac[1] == kVulnerableOuis[i].oui[1] &&
        mac[2] == kVulnerableOuis[i].oui[2]) {
      return true;
    }
  }
  return false;
}

// Track VERY CLOSE dwell time for vulnerability check
uint8_t g_veryCloseMac[6] = {0};
uint32_t g_veryCloseStartMs = 0;
constexpr uint32_t kVulnCheckDwellMs = 3000;  // 3 seconds

// Stickiness: only switch displayed device if new one is significantly stronger.
constexpr int kStickyRssiMarginDb = 10;

// Safe-blink animation: blink green twice when device is confirmed safe.
bool g_safeBlinkTriggered = false;   // Has the blink been triggered for current device?
uint32_t g_safeBlinkStartMs = 0;     // When the blink animation started.
constexpr uint32_t kBlinkDurationMs = 600;  // Total duration of 2-blink animation.

inline float clamp01(float v) {
  if (v < 0.0f) return 0.0f;
  if (v > 1.0f) return 1.0f;
  return v;
}

inline void setLedColor(const RgbColor& c, uint8_t brightnessPercent) {
  const uint16_t scale = static_cast<uint16_t>(brightnessPercent) * 255 / 100;
  const uint8_t r = static_cast<uint8_t>((static_cast<uint16_t>(c.r) * scale) / 255);
  const uint8_t g = static_cast<uint8_t>((static_cast<uint16_t>(c.g) * scale) / 255);
  const uint8_t b = static_cast<uint8_t>((static_cast<uint16_t>(c.b) * scale) / 255);
  g_rgb.setPixelColor(0, g_rgb.Color(r, g, b));
  g_rgb.show();
}

inline uint16_t macHash16(const uint8_t* mac) {
  return (static_cast<uint16_t>(mac[4]) << 8) | mac[5];
}

inline void formatMac(const uint8_t mac[6], char* out, size_t outLen) {
  if (!out || outLen == 0) return;
  snprintf(out, outLen, "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void noteDeviceSeen(const uint8_t mac[6], int rssi, const char* name);

void noteDeviceSeen(const uint8_t mac[6], int rssi) {
  noteDeviceSeen(mac, rssi, nullptr);
}

void noteDeviceSeen(const uint8_t mac[6], int rssi, const char* name) {
  const uint32_t nowMs = millis();
  const uint16_t h = macHash16(mac);

  portENTER_CRITICAL(&g_mux);

  // Best RSSI tracking
  g_lastAnySeenMs = nowMs;
  if (rssi > g_bestRssi) g_bestRssi = rssi;

  // Find existing
  int freeIdx = -1;
  int matchIdx = -1;
  for (int i = 0; i < kMaxDevices; i++) {
    if (!g_devices[i].used) {
      if (freeIdx < 0) freeIdx = i;
      continue;
    }
    // Quick hash filter first, then memcmp
    if (macHash16(g_devices[i].mac) == h && memcmp(g_devices[i].mac, mac, 6) == 0) {
      matchIdx = i;
      break;
    }
  }

  const int idx = (matchIdx >= 0) ? matchIdx : freeIdx;
  if (idx >= 0) {
    memcpy(g_devices[idx].mac, mac, 6);
    g_devices[idx].lastSeenMs = nowMs;
    g_devices[idx].lastRssi = static_cast<int8_t>(rssi);
    g_devices[idx].used = true;

    if (name && name[0] != '\0') {
      strncpy(g_devices[idx].name, name, sizeof(g_devices[idx].name) - 1);
      g_devices[idx].name[sizeof(g_devices[idx].name) - 1] = '\0';
    }
  }

  portEXIT_CRITICAL(&g_mux);
}

int countActiveDevicesAndBest(int* outBestRssi, char* outBestName, size_t outBestNameLen, uint8_t outBestMac[6]) {
  const uint32_t nowMs = millis();
  int count = 0;
  int best = -127;
  int bestIdx = -1;

  // Check if currently tracked VERY CLOSE device is still valid.
  int stickyIdx = -1;
  int stickyRssi = -127;

  portENTER_CRITICAL(&g_mux);
  for (int i = 0; i < kMaxDevices; i++) {
    if (!g_devices[i].used) continue;
    if ((nowMs - g_devices[i].lastSeenMs) <= kDeviceStaleMs) {
      count++;
      const int devRssi = g_devices[i].lastRssi;

      // Track absolute best.
      if (devRssi > best) {
        best = devRssi;
        bestIdx = i;
      }

      // Check if this is our sticky (currently tracked) device.
      if (memcmp(g_devices[i].mac, g_veryCloseMac, 6) == 0) {
        stickyIdx = i;
        stickyRssi = devRssi;
      }
    }
  }

  // Stickiness logic: if the sticky device is still VERY CLOSE, keep it
  // unless the new best is significantly stronger.
  if (stickyIdx >= 0 && stickyRssi >= kVeryCloseRssiDbm) {
    // Only switch if new best is > kStickyRssiMarginDb stronger.
    if (bestIdx != stickyIdx && (best - stickyRssi) <= kStickyRssiMarginDb) {
      bestIdx = stickyIdx;
      best = stickyRssi;
    }
  }

  if (outBestName && outBestNameLen > 0) {
    outBestName[0] = '\0';
    if (bestIdx >= 0) {
      strncpy(outBestName, g_devices[bestIdx].name, outBestNameLen - 1);
      outBestName[outBestNameLen - 1] = '\0';
    }
  }

  if (outBestMac) {
    memset(outBestMac, 0, 6);
    if (bestIdx >= 0) {
      memcpy(outBestMac, g_devices[bestIdx].mac, 6);
    }
  }
  portEXIT_CRITICAL(&g_mux);

  if (outBestRssi) *outBestRssi = best;
  return count;
}

#if BLEWATCH_USE_NIMBLE

class AdvCallbacks : public NimBLEAdvertisedDeviceCallbacks {
  void onResult(NimBLEAdvertisedDevice* dev) override {
    if (!dev) return;
    const int rssi = dev->getRSSI();
    const NimBLEAddress addr = dev->getAddress();
    uint8_t mac[6];
    memcpy(mac, addr.getNative(), 6);

    // Only pull name when "very close" to keep callback lightweight.
    if (rssi >= kVeryCloseRssiDbm) {
      const std::string name = dev->getName();
      noteDeviceSeen(mac, rssi, name.empty() ? nullptr : name.c_str());
    } else {
      noteDeviceSeen(mac, rssi);
    }
  }
};

AdvCallbacks g_advCb;

void bleTask(void* param) {
  (void)param;

  NimBLEDevice::init("");

  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setAdvertisedDeviceCallbacks(&g_advCb, true /* want duplicates */);
  scan->setActiveScan(true);
  scan->setInterval(kBleScanInterval);
  scan->setWindow(kBleScanWindow);

  while (true) {
    // Run short scans repeatedly (keeps memory stable on Arduino builds)
    scan->start(kBleScanDurationS /* seconds */, false /* is_continue */);
    scan->clearResults();
    vTaskDelay(pdMS_TO_TICKS(kBleScanLoopDelayMs));
  }
}

#else

class AdvCallbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice dev) override {
    // Keep callbacks lightweight.
    const int rssi = dev.getRSSI();
    const String s = dev.getAddress().toString();
    uint8_t mac[6] = {0};
    // Parse "aa:bb:cc:dd:ee:ff"
    unsigned int b[6];
    if (sscanf(s.c_str(), "%02x:%02x:%02x:%02x:%02x:%02x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) == 6) {
      for (int i = 0; i < 6; i++) mac[i] = static_cast<uint8_t>(b[i]);

      // Only pull name when "very close" to keep callback lightweight.
      if (rssi >= kVeryCloseRssiDbm) {
        const String name = dev.getName();
        noteDeviceSeen(mac, rssi, (name.length() == 0) ? nullptr : name.c_str());
      } else {
        noteDeviceSeen(mac, rssi);
      }
    }
  }
};

AdvCallbacks g_advCb;

// Some ESP32 BLE libraries expose a "want duplicates" overload, others don't.
// This helper picks the best available overload at compile time.
template <typename T>
auto setBleCallbacksWithDuplicates(T* scan, BLEAdvertisedDeviceCallbacks* cb, int)
  -> decltype(scan->setAdvertisedDeviceCallbacks(cb, true), void()) {
  scan->setAdvertisedDeviceCallbacks(cb, true);
}

template <typename T>
void setBleCallbacksWithDuplicates(T* scan, BLEAdvertisedDeviceCallbacks* cb, ...) {
  scan->setAdvertisedDeviceCallbacks(cb);
}

void bleTask(void* param) {
  (void)param;

  BLEDevice::init("");
  BLEScan* scan = BLEDevice::getScan();
  setBleCallbacksWithDuplicates(scan, &g_advCb, 0);
  scan->setActiveScan(true);

  // Prefer a high duty-cycle scan when supported.
  scan->setInterval(kBleScanInterval);
  scan->setWindow(kBleScanWindow);

  while (true) {
    scan->start(kBleScanDurationS /* seconds */, false /* is_continue */);
    scan->clearResults();
    vTaskDelay(pdMS_TO_TICKS(kBleScanLoopDelayMs));
  }
}

#endif

lv_obj_t* makeLabel(lv_obj_t* parent, const char* txt, lv_color_t color, const lv_font_t* font = nullptr) {
  lv_obj_t* lbl = lv_label_create(parent);
  lv_label_set_text(lbl, txt);
  lv_obj_set_style_text_color(lbl, color, 0);
  if (font) lv_obj_set_style_text_font(lbl, font, 0);
  return lbl;
}

void buildUi() {
  g_root = lv_obj_create(lv_scr_act());
  lv_obj_set_size(g_root, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(g_root, lv_color_hex(0x061322), 0);
  lv_obj_set_style_border_width(g_root, 0, 0);
  lv_obj_set_style_pad_all(g_root, 6, 0);

  lv_obj_t* header = lv_obj_create(g_root);
  lv_obj_set_size(header, LV_PCT(100), 34);
  lv_obj_set_style_bg_color(header, lv_color_hex(0x0A2238), 0);
  lv_obj_set_style_border_width(header, 0, 0);
  lv_obj_set_style_radius(header, 6, 0);
  lv_obj_set_style_pad_all(header, 6, 0);
  lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);

  g_title = makeLabel(header, "BLEwatch", lv_color_hex(0xFFFFFF), &lv_font_montserrat_14);
  lv_obj_align(g_title, LV_ALIGN_LEFT_MID, 4, 0);

  // Big count panel
  lv_obj_t* panel = lv_obj_create(g_root);
  lv_obj_set_size(panel, LV_PCT(100), 140);
  lv_obj_set_style_bg_color(panel, lv_color_hex(0x0A2238), 0);
  lv_obj_set_style_border_width(panel, 0, 0);
  lv_obj_set_style_radius(panel, 10, 0);
  lv_obj_set_style_pad_all(panel, 10, 0);
  lv_obj_align(panel, LV_ALIGN_TOP_MID, 0, 42);

  makeLabel(panel, "Nearby devices", lv_color_hex(0xFFD000), &lv_font_montserrat_14);

  g_countLabel = makeLabel(panel, "0", lv_color_hex(0xFFFFFF), &lv_font_montserrat_20);
  lv_obj_align(g_countLabel, LV_ALIGN_CENTER, 0, 10);
  lv_obj_set_style_text_font(g_countLabel, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_letter_space(g_countLabel, 2, 0);
  lv_obj_set_style_text_align(g_countLabel, LV_TEXT_ALIGN_CENTER, 0);

  g_bar = lv_bar_create(panel);
  lv_bar_set_range(g_bar, 0, 100);
  lv_obj_set_size(g_bar, 190, 18);
  lv_obj_align(g_bar, LV_ALIGN_BOTTOM_MID, 0, -6);
  lv_obj_set_style_bg_color(g_bar, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(g_bar, LV_OPA_40, 0);
  lv_obj_set_style_radius(g_bar, 5, 0);

  // RSSI + state
  g_rssiLabel = makeLabel(g_root, "RSSI -- dBm", lv_color_hex(0x8BE9FD), &lv_font_montserrat_14);
  lv_obj_align(g_rssiLabel, LV_ALIGN_TOP_MID, 0, 196);

  g_stateLabel = makeLabel(g_root, "FAR", lv_color_hex(0xFFFFFF), &lv_font_montserrat_20);
  lv_obj_align(g_stateLabel, LV_ALIGN_TOP_MID, 0, 230);
  lv_obj_set_style_text_align(g_stateLabel, LV_TEXT_ALIGN_CENTER, 0);

  g_nameLabel = makeLabel(g_root, "", lv_color_hex(0x8BE9FD), &lv_font_montserrat_14);
  lv_obj_align(g_nameLabel, LV_ALIGN_TOP_MID, 0, 258);
  lv_obj_set_style_text_align(g_nameLabel, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_add_flag(g_nameLabel, LV_OBJ_FLAG_HIDDEN);
}

float rssiToNearT(int bestRssi) {
  // Map [near start .. close start] -> [0..1]
  const float t = (static_cast<float>(bestRssi) - static_cast<float>(kNearStartRssiDbm)) /
                  (static_cast<float>(kCloseStartRssiDbm) - static_cast<float>(kNearStartRssiDbm));
  return clamp01(t);
}

float rssiToCloseT(int bestRssi) {
  // Map [close start .. very close] -> [0..1]
  const float t = (static_cast<float>(bestRssi) - static_cast<float>(kCloseStartRssiDbm)) /
                  (static_cast<float>(kVeryCloseRssiDbm) - static_cast<float>(kCloseStartRssiDbm));
  return clamp01(t);
}

void updateLedAndUi() {
  int bestRssi = -127;
  char bestName[32] = {0};
  uint8_t bestMac[6] = {0};
  const int count = countActiveDevicesAndBest(&bestRssi, bestName, sizeof(bestName), bestMac);

  // UI text
  char buf[64];
  snprintf(buf, sizeof(buf), "%d", count);
  lv_label_set_text(g_countLabel, buf);

  if (bestRssi <= -120 || count == 0) {
    lv_label_set_text(g_rssiLabel, "RSSI -- dBm");
  } else {
    snprintf(buf, sizeof(buf), "RSSI %d dBm", bestRssi);
    lv_label_set_text(g_rssiLabel, buf);
  }

  // Proximity state + bar + LED
  const uint32_t nowMs = millis();

  if (count == 0 || bestRssi < kFarRssiDbm) {
    lv_label_set_text(g_stateLabel, "FAR");
    lv_obj_add_flag(g_nameLabel, LV_OBJ_FLAG_HIDDEN);
    lv_bar_set_value(g_bar, 0, LV_ANIM_OFF);
    setLedColor(LED_OFF, 0);
    // Reset VERY CLOSE tracking.
    memset(g_veryCloseMac, 0, 6);
    g_veryCloseStartMs = 0;
    g_safeBlinkTriggered = false;
    return;
  }

  // Extra step: RSSI in [-80..-67) is "too far" (weak but present).
  if (bestRssi < kNearStartRssiDbm) {
    lv_label_set_text(g_stateLabel, "TOO FAR");
    lv_obj_add_flag(g_nameLabel, LV_OBJ_FLAG_HIDDEN);
    lv_bar_set_value(g_bar, 0, LV_ANIM_OFF);
    setLedColor(LED_ORANGE, 100);
    // Reset VERY CLOSE tracking.
    memset(g_veryCloseMac, 0, 6);
    g_veryCloseStartMs = 0;
    g_safeBlinkTriggered = false;
    return;
  }

  if (bestRssi >= kVeryCloseRssiDbm) {
    lv_label_set_text(g_stateLabel, "VERY CLOSE");

    // Track how long this device has been VERY CLOSE.
    bool sameDevice = (memcmp(g_veryCloseMac, bestMac, 6) == 0);
    if (!sameDevice) {
      memcpy(g_veryCloseMac, bestMac, 6);
      g_veryCloseStartMs = nowMs;
      g_safeBlinkTriggered = false;  // Reset blink for new device.
    }
    const uint32_t dwellMs = nowMs - g_veryCloseStartMs;

    // After 3 seconds, check OUI for potential vulnerability.
    bool showVulnWarning = false;
    if (dwellMs >= kVulnCheckDwellMs) {
      showVulnWarning = isOuiPotentiallyVulnerable(bestMac);
    }

    // Build display string (name or MAC).
    char displayBuf[48];
    if (bestName[0] != '\0') {
      strncpy(displayBuf, bestName, sizeof(displayBuf) - 1);
      displayBuf[sizeof(displayBuf) - 1] = '\0';
    } else {
      formatMac(bestMac, displayBuf, sizeof(displayBuf));
    }

    lv_obj_clear_flag(g_nameLabel, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(g_nameLabel, displayBuf);

    // Color the label: red if potentially vulnerable, green if safe, default cyan while checking.
    if (dwellMs >= kVulnCheckDwellMs) {
      if (showVulnWarning) {
        lv_obj_set_style_text_color(g_nameLabel, lv_color_hex(0xFF0000), 0);  // red
      } else {
        lv_obj_set_style_text_color(g_nameLabel, lv_color_hex(0x00FF00), 0);  // green
      }
    } else {
      lv_obj_set_style_text_color(g_nameLabel, lv_color_hex(0x8BE9FD), 0);  // cyan (checking...)
    }

    lv_bar_set_value(g_bar, 100, LV_ANIM_OFF);

    // LED behavior.
    if (showVulnWarning) {
      // Vulnerable: steady red.
      setLedColor(LED_RED, 100);
    } else if (dwellMs >= kVulnCheckDwellMs) {
      // Safe: blink green twice, then steady blue.
      if (!g_safeBlinkTriggered) {
        g_safeBlinkTriggered = true;
        g_safeBlinkStartMs = nowMs;
      }
      const uint32_t blinkElapsed = nowMs - g_safeBlinkStartMs;
      if (blinkElapsed < kBlinkDurationMs) {
        // Two blinks in 600ms: on 0-100, off 100-200, on 200-300, off 300-400, then done.
        const uint32_t phase = blinkElapsed % 200;
        const uint32_t cycle = blinkElapsed / 200;
        if (cycle < 2 && phase < 100) {
          setLedColor(LED_GREEN, 100);  // Blink on.
        } else {
          setLedColor(LED_OFF, 0);      // Blink off.
        }
      } else {
        setLedColor(LED_BLUE, 100);     // After blink, steady blue.
      }
    } else {
      // Still checking: steady blue.
      setLedColor(LED_BLUE, 100);
    }
    return;
  }

  // Close: steady green in [-50..-40)
  if (bestRssi >= kCloseStartRssiDbm) {
    const float ct = rssiToCloseT(bestRssi);
    lv_label_set_text(g_stateLabel, "CLOSE");
    lv_obj_add_flag(g_nameLabel, LV_OBJ_FLAG_HIDDEN);
    lv_bar_set_value(g_bar, static_cast<int>(70.0f + ct * 30.0f + 0.5f), LV_ANIM_OFF);
    setLedColor(LED_CYAN, 100);
    return;
  }

  // Near: pulse green. Pulse speed + peak brightness scale with RSSI.
  const float t = rssiToNearT(bestRssi);
  const float hz = kMinPulseHz + (kMaxPulseHz - kMinPulseHz) * t;

  // Brightness behavior:
  // - When barely near: average around ~50% and slower.
  // - When closer: higher peak and faster.
  const uint8_t peak = static_cast<uint8_t>(kNearMinBrightness + (kNearMaxBrightness - kNearMinBrightness) * t);
  const uint8_t trough = static_cast<uint8_t>((kNearAvgBrightness > 20) ? (kNearAvgBrightness - 20) : 10);

  const float phase = (static_cast<float>(nowMs) / 1000.0f) * (2.0f * kPi * hz);
  const float s = 0.5f * (1.0f + sinf(phase)); // 0..1
  const uint8_t b = static_cast<uint8_t>(trough + (peak - trough) * s);

  lv_label_set_text(g_stateLabel, "NEAR");
  lv_obj_add_flag(g_nameLabel, LV_OBJ_FLAG_HIDDEN);
  lv_bar_set_value(g_bar, static_cast<int>(t * 70.0f + 0.5f), LV_ANIM_OFF);
  setLedColor(LED_GREEN, b);
}

void uiTimerCb(lv_timer_t* t) {
  (void)t;
  updateLedAndUi();
}

} // namespace

void Blewatch_Init(void) {
  // RGB LED init
  g_rgb.begin();
  g_rgb.setBrightness(255);
  g_rgb.clear();
  g_rgb.show();

  buildUi();

  // Start BLE scan task
  xTaskCreatePinnedToCore(
    bleTask,
    "ble_scan",
    4096,
    nullptr,
    1,
    nullptr,
    0
  );

  lv_timer_create(uiTimerCb, kUiIntervalMs, nullptr);
  updateLedAndUi();
}

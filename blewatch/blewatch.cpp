#include "blewatch.h"

#include <Arduino.h>
#include <math.h>
#include <lvgl.h>
#include <Adafruit_NeoPixel.h>

#if __has_include(<NimBLEDevice.h>)
  #include <NimBLEDevice.h>
  #define BLEWATCH_USE_NIMBLE 1
#else
  #include <BLEDevice.h>
  #include <BLEScan.h>
  #define BLEWATCH_USE_NIMBLE 0
#endif

namespace {

constexpr uint32_t kUiIntervalMs = 80;
constexpr uint32_t kDeviceStaleMs = 3500;

// LED (WS2812) config (matches Bandwatch defaults)
constexpr int kRgbPin = 8;
constexpr int kRgbCount = 1;
constexpr uint16_t kNeoPixelType = NEO_RGB + NEO_KHZ800;

// Proximity heuristics (RSSI is not distance; these are tunable)
constexpr int kFarRssiDbm = -80;        // below this -> treat as far/none
constexpr int kNearStartRssiDbm = -70;  // start pulsing green
constexpr int kVeryCloseRssiDbm = -35;  // ~"within 5cm" heuristic => steady blue

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
lv_obj_t* g_bar = nullptr;

Adafruit_NeoPixel g_rgb(kRgbCount, kRgbPin, kNeoPixelType);

struct RgbColor { uint8_t r; uint8_t g; uint8_t b; };
constexpr RgbColor LED_OFF  = {0, 0, 0};
constexpr RgbColor LED_GREEN = {0, 180, 40};
constexpr RgbColor LED_BLUE  = {0, 60, 255};

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

void noteDeviceSeen(const uint8_t mac[6], int rssi) {
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
  }

  portEXIT_CRITICAL(&g_mux);
}

int countActiveDevicesAndBestRssi(int* outBestRssi) {
  const uint32_t nowMs = millis();
  int count = 0;
  int best = -127;

  portENTER_CRITICAL(&g_mux);
  for (int i = 0; i < kMaxDevices; i++) {
    if (!g_devices[i].used) continue;
    if ((nowMs - g_devices[i].lastSeenMs) <= kDeviceStaleMs) {
      count++;
      if (g_devices[i].lastRssi > best) best = g_devices[i].lastRssi;
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
    noteDeviceSeen(mac, rssi);
  }
};

AdvCallbacks g_advCb;

void bleTask(void* param) {
  (void)param;

  NimBLEDevice::init("");

  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setAdvertisedDeviceCallbacks(&g_advCb, true /* want duplicates */);
  scan->setActiveScan(true);
  scan->setInterval(45);
  scan->setWindow(15);

  while (true) {
    // Run short scans repeatedly (keeps memory stable on Arduino builds)
    scan->start(2 /* seconds */, false /* is_continue */);
    scan->clearResults();
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

#else

class AdvCallbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice dev) override {
    // BLEAdvertisedDevice in this library uses std::string; avoid heavy work
    const int rssi = dev.getRSSI();
    const String s = dev.getAddress().toString();
    uint8_t mac[6] = {0};
    // Parse "aa:bb:cc:dd:ee:ff"
    unsigned int b[6];
    if (sscanf(s.c_str(), "%02x:%02x:%02x:%02x:%02x:%02x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) == 6) {
      for (int i = 0; i < 6; i++) mac[i] = static_cast<uint8_t>(b[i]);
      noteDeviceSeen(mac, rssi);
    }
  }
};

AdvCallbacks g_advCb;

void bleTask(void* param) {
  (void)param;

  BLEDevice::init("");
  BLEScan* scan = BLEDevice::getScan();
  scan->setAdvertisedDeviceCallbacks(&g_advCb);
  scan->setActiveScan(true);

  while (true) {
    scan->start(2 /* seconds */, false /* is_continue */);
    scan->clearResults();
    vTaskDelay(pdMS_TO_TICKS(20));
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
}

float rssiToNearT(int bestRssi) {
  // Map [-70 .. -40] -> [0..1]
  const float t = (static_cast<float>(bestRssi) - static_cast<float>(kNearStartRssiDbm)) /
                  (static_cast<float>(-40) - static_cast<float>(kNearStartRssiDbm));
  return clamp01(t);
}

void updateLedAndUi() {
  int bestRssi = -127;
  const int count = countActiveDevicesAndBestRssi(&bestRssi);

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
    lv_bar_set_value(g_bar, 0, LV_ANIM_OFF);
    setLedColor(LED_OFF, 0);
    return;
  }

  if (bestRssi >= kVeryCloseRssiDbm) {
    lv_label_set_text(g_stateLabel, "VERY CLOSE");
    lv_bar_set_value(g_bar, 100, LV_ANIM_OFF);
    setLedColor(LED_BLUE, 100);
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
  lv_bar_set_value(g_bar, static_cast<int>(t * 100.0f + 0.5f), LV_ANIM_OFF);
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

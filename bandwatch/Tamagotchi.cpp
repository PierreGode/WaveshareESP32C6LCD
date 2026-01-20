#include "Tamagotchi.h"

#include <Arduino.h>
#include <WiFi.h>

namespace {

// Display is 16-bit (RGB565). LVGL runs with LV_COLOR_DEPTH=16 by default in most Arduino LVGL setups.

constexpr uint32_t kScanIntervalMs = 3500;
constexpr uint32_t kUiIntervalMs = 120;  // UI refresh cadence

// Simple RGB565 colors
inline lv_color_t c565(uint16_t v) {
    const uint8_t r5 = (v >> 11) & 0x1F;
    const uint8_t g6 = (v >> 5) & 0x3F;
    const uint8_t b5 = v & 0x1F;
    const uint8_t r8 = (uint16_t(r5) * 255) / 31;
    const uint8_t g8 = (uint16_t(g6) * 255) / 63;
    const uint8_t b8 = (uint16_t(b5) * 255) / 31;
    return lv_color_make(r8, g8, b8);
}

constexpr uint16_t BG_565      = 0x0022; // deep blue/black
constexpr uint16_t PANEL_565   = 0x0843; // muted navy
constexpr uint16_t WHITE_565   = 0xFFFF;
constexpr uint16_t BLACK_565   = 0x0000;
constexpr uint16_t GREEN_565   = 0x07E0;
constexpr uint16_t RED_565     = 0xF800;
constexpr uint16_t CYAN_565    = 0x07FF;
constexpr uint16_t PURPLE_565  = 0x780F; // violet accent
constexpr uint16_t YELLOW_565  = 0xFFE0;

struct State {
    uint32_t lastScanStartMs = 0;
    bool scanInFlight = false;

    float interference = 0.0f;     // 0..1 (raw)
    float interferenceEma = 0.0f;  // 0..1 (smoothed)
    int   lastScanCount = 0;
};

State st;

lv_obj_t* root = nullptr;
lv_obj_t* titleLabel = nullptr;
lv_obj_t* bar = nullptr;
lv_obj_t* decLabel = nullptr;
lv_obj_t* hexLabel = nullptr;
lv_obj_t* netLabel = nullptr;
lv_obj_t* binStrip = nullptr;
lv_obj_t* bitBoxes[8];
lv_obj_t* bitLabels[8];

static float clamp01(float v) {
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

static void wifiEnsureSetupOnce() {
    static bool did = false;
    if (did) return;
    did = true;

    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.disconnect(true, true);

    st.lastScanStartMs = millis();
    WiFi.scanNetworks(true /* async */, true /* show hidden */);
    st.scanInFlight = true;
}

static float computeInterferenceFromScan(int networksFound) {
    if (networksFound <= 0) return 0.0f;

    float sum = 0.0f;
    for (int i = 0; i < networksFound; i++) {
        int32_t rssi = WiFi.RSSI(i);
        float w = (static_cast<float>(rssi) + 100.0f) / 60.0f; // -100 => 0, -40 => 1
        w = clamp01(w);
        sum += w;
    }

    float norm = sum / 12.0f; // ~12 strong APs => "very noisy"
    return clamp01(norm);
}

static void startScanIfNeeded() {
    const uint32_t now = millis();
    if (st.scanInFlight) return;
    if ((now - st.lastScanStartMs) < kScanIntervalMs) return;

    st.lastScanStartMs = now;
    WiFi.scanNetworks(true /* async */, true /* show hidden */);
    st.scanInFlight = true;
}

static void pollScan() {
    if (!st.scanInFlight) return;

    int res = WiFi.scanComplete();
    if (res == WIFI_SCAN_RUNNING) return;

    st.scanInFlight = false;

    if (res >= 0) {
        st.lastScanCount = res;
        st.interference = computeInterferenceFromScan(res);
        WiFi.scanDelete();

        constexpr float alpha = 0.18f; // smoothing
        st.interferenceEma = (1.0f - alpha) * st.interferenceEma + alpha * st.interference;
        st.interferenceEma = clamp01(st.interferenceEma);
    } else {
        WiFi.scanDelete();
    }
}

static void updateBits(uint8_t value) {
    for (int i = 0; i < 8; i++) {
        const bool on = (value >> (7 - i)) & 0x1;
        lv_obj_set_style_bg_color(bitBoxes[i], c565(on ? PURPLE_565 : PANEL_565), 0);
        lv_label_set_text(bitLabels[i], on ? "1" : "0");
    }
}

static void updateUI() {
    const int32_t pct = static_cast<int32_t>(st.interferenceEma * 100.0f + 0.5f);
    const uint8_t byteVal = static_cast<uint8_t>(st.interferenceEma * 255.0f + 0.5f);

    lv_bar_set_value(bar, pct, LV_ANIM_OFF);

    // Bar color from green->yellow->red
    lv_color_t barColor = c565(GREEN_565);
    if (pct > 70) barColor = c565(RED_565);
    else if (pct > 40) barColor = c565(YELLOW_565);
    lv_obj_set_style_bg_color(bar, barColor, LV_PART_INDICATOR);

    char buf[32];
    snprintf(buf, sizeof(buf), "DEC %3d", byteVal);
    lv_label_set_text(decLabel, buf);

    snprintf(buf, sizeof(buf), "HEX 0x%02X", byteVal);
    lv_label_set_text(hexLabel, buf);

    snprintf(buf, sizeof(buf), "WIFI %d APs", st.lastScanCount);
    lv_label_set_text(netLabel, buf);

    updateBits(byteVal);
}

static void timerCb(lv_timer_t* t) {
    (void)t;
    wifiEnsureSetupOnce();
    pollScan();
    startScanIfNeeded();
    updateUI();
}

static lv_obj_t* make_label(lv_obj_t* parent, const char* txt, lv_color_t color, lv_coord_t x, lv_coord_t y, bool mono=false) {
    lv_obj_t* lbl = lv_label_create(parent);
    lv_label_set_text(lbl, txt);
    lv_obj_set_style_text_color(lbl, color, 0);
    if (mono) lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(lbl, x, y);
    return lbl;
}

} // namespace

void Tamagotchi_Init(void) {
    root = lv_obj_create(lv_scr_act());
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(root, c565(BG_565), 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);

    // Header (compact to fit 172px width)
    lv_obj_t* header = lv_obj_create(root);
    lv_obj_set_size(header, LV_PCT(100), 52);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(header, c565(PANEL_565), 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_radius(header, 0, 0);

    titleLabel = make_label(header, "BANDWATCH", c565(WHITE_565), 6, 4, true);

    // Middle bar container
    lv_obj_t* barWrap = lv_obj_create(root);
    lv_obj_set_size(barWrap, 150, 26);
    lv_obj_align(barWrap, LV_ALIGN_TOP_MID, 0, 132);
    lv_obj_set_style_bg_color(barWrap, c565(PANEL_565), 0);
    lv_obj_set_style_border_width(barWrap, 0, 0);
    lv_obj_set_style_radius(barWrap, 6, 0);
    lv_obj_set_style_pad_all(barWrap, 6, 0);

    bar = lv_bar_create(barWrap);
    lv_obj_set_size(bar, 138, 12);
    lv_obj_align(bar, LV_ALIGN_CENTER, 0, 0);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, c565(BLACK_565), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_40, 0);

    // Info row with wrapping to fit narrow width
    lv_obj_t* info = lv_obj_create(root);
    lv_obj_set_size(info, LV_PCT(100), 72);
    lv_obj_align(info, LV_ALIGN_TOP_MID, 0, 56);
    lv_obj_set_style_bg_color(info, c565(BG_565), 0);
    lv_obj_set_style_border_width(info, 0, 0);
    lv_obj_set_style_pad_all(info, 8, 0);
    lv_obj_set_style_pad_row(info, 4, 0);
    lv_obj_set_style_pad_column(info, 8, 0);
    lv_obj_set_flex_flow(info, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(info, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    decLabel = make_label(info, "DEC --", c565(CYAN_565), 0, 0, true);
    hexLabel = make_label(info, "HEX --", c565(WHITE_565), 0, 0, true);
    netLabel = make_label(info, "WIFI --", c565(YELLOW_565), 0, 0, true);

    // Binary strip container (wrap to multiple rows if needed)
    binStrip = lv_obj_create(root);
    lv_obj_set_size(binStrip, LV_PCT(100), 140);
    lv_obj_align(binStrip, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_set_style_bg_color(binStrip, c565(BG_565), 0);
    lv_obj_set_style_border_width(binStrip, 0, 0);
    lv_obj_set_style_pad_all(binStrip, 8, 0);
    lv_obj_set_style_pad_column(binStrip, 4, 0);
    lv_obj_set_style_pad_row(binStrip, 6, 0);
    lv_obj_set_flex_flow(binStrip, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(binStrip, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    for (int i = 0; i < 8; i++) {
        lv_obj_t* cell = lv_obj_create(binStrip);
        bitBoxes[i] = cell;
        lv_obj_set_size(cell, 22, 48);
        lv_obj_set_style_bg_color(cell, c565(PANEL_565), 0);
        lv_obj_set_style_border_width(cell, 0, 0);
        lv_obj_set_style_radius(cell, 4, 0);

        lv_obj_t* lbl = lv_label_create(cell);
        bitLabels[i] = lbl;
        lv_label_set_text(lbl, "0");
        lv_obj_center(lbl);
        lv_obj_set_style_text_color(lbl, c565(WHITE_565), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    }

    updateBits(0);

    lv_timer_create(timerCb, kUiIntervalMs, nullptr);
}

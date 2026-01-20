#include "Tamagotchi.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_wifi_types.h>
#include <math.h>

namespace {

// Display is 16-bit (RGB565). LVGL runs with LV_COLOR_DEPTH=16 by default in most Arduino LVGL setups.

constexpr uint32_t kDwellMs = 260;          // Dwell per channel (instructions: 200–400 ms)
constexpr uint32_t kUiIntervalMs = 120;     // UI refresh cadence
constexpr int kChannelCount = 13;           // 2.4 GHz 1–13
constexpr int kStrongThresholdDbm = -65;    // "Strong" frame threshold
constexpr float kBusyEmaAlpha = 0.22f;      // Smoothing within required 0.15–0.30
constexpr int kUniqueSlots = 24;            // Best-effort unique transmitter slots

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

constexpr uint16_t BG_565      = 0x0122; // deep blue/black
constexpr uint16_t PANEL_565   = 0x0843; // muted navy
constexpr uint16_t WHITE_565   = 0xFFFF;
constexpr uint16_t BLACK_565   = 0x0000;
constexpr uint16_t GREEN_565   = 0x07E0;
constexpr uint16_t RED_565     = 0xF800;
constexpr uint16_t CYAN_565    = 0x07FF;
constexpr uint16_t PURPLE_565  = 0x780F; // violet accent
constexpr uint16_t YELLOW_565  = 0xFFE0;

struct Accum {
    uint32_t frames = 0;
    uint32_t bytes = 0;
    uint16_t strong = 0;
    uint16_t unique = 0;
    uint16_t macHashes[kUniqueSlots] = {0};
    uint8_t macFill = 0;
};

struct ChannelMetrics {
    uint32_t frames = 0;
    uint32_t bytes = 0;
    uint16_t strong = 0;
    uint16_t unique = 0;
};

struct ChannelState {
    ChannelMetrics metrics;
    float busyCurrent = 0.0f;  // Last dwell busy score (0–100)
    float busyEma = 0.0f;      // Smoothed busy score (0–100)
    bool hasData = false;
};

// IEEE 802.11 header (truncated – enough to read transmitter address)
typedef struct {
    uint16_t frame_ctrl;
    uint16_t duration_id;
    uint8_t addr1[6];
    uint8_t addr2[6];
    uint8_t addr3[6];
    uint16_t seq_ctrl;
    uint8_t addr4[6];
} wifi_ieee80211_mac_hdr_t;

typedef struct {
    wifi_ieee80211_mac_hdr_t hdr;
    uint8_t payload[0];
} wifi_ieee80211_packet_t;

volatile Accum g_accum;
portMUX_TYPE g_accumMux = portMUX_INITIALIZER_UNLOCKED;

ChannelState channels[kChannelCount];
int currentChannel = 1;
uint32_t dwellStartedMs = 0;

lv_obj_t* root = nullptr;
lv_obj_t* titleLabel = nullptr;
lv_obj_t* globalBar = nullptr;
lv_obj_t* globalLabel = nullptr;
lv_obj_t* methodLabel = nullptr;
lv_obj_t* topRows[3] = {nullptr};
lv_obj_t* stripBars[kChannelCount] = {nullptr};
lv_obj_t* stripLabels[kChannelCount] = {nullptr};
lv_obj_t* dwellLabel = nullptr;

inline float clamp01(float v) {
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

inline uint16_t macHash(const uint8_t* mac) {
    // Lightweight hash using last two bytes to avoid heavy math in ISR.
    return (static_cast<uint16_t>(mac[4]) << 8) | mac[5];
}

void IRAM_ATTR promiscuousCb(void* buf, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_MGMT && type != WIFI_PKT_DATA && type != WIFI_PKT_CTRL) return;
    const wifi_promiscuous_pkt_t* pkt = reinterpret_cast<const wifi_promiscuous_pkt_t*>(buf);
    if (pkt->rx_ctrl.sig_len < sizeof(wifi_ieee80211_mac_hdr_t)) return; // malformed
    const wifi_ieee80211_packet_t* ipkt = reinterpret_cast<const wifi_ieee80211_packet_t*>(pkt->payload);

    portENTER_CRITICAL_ISR(&g_accumMux);
    g_accum.frames += 1;
    g_accum.bytes += pkt->rx_ctrl.sig_len;
    if (pkt->rx_ctrl.rssi >= kStrongThresholdDbm) {
        g_accum.strong += 1;
    }

    if (ipkt) {
        const uint8_t* src = ipkt->hdr.addr2;  // Best-effort transmitter
        const uint16_t h = macHash(src);
        bool known = false;
        for (uint8_t i = 0; i < g_accum.macFill; i++) {
            if (g_accum.macHashes[i] == h) {
                known = true;
                break;
            }
        }
        if (!known && g_accum.macFill < kUniqueSlots) {
            g_accum.macHashes[g_accum.macFill++] = h;
            g_accum.unique += 1;
        }
    }
    portEXIT_CRITICAL_ISR(&g_accumMux);
}

void resetAccum() {
    portENTER_CRITICAL(&g_accumMux);
    g_accum.frames = 0;
    g_accum.bytes = 0;
    g_accum.strong = 0;
    g_accum.unique = 0;
    g_accum.macFill = 0;
    for (int i = 0; i < kUniqueSlots; i++) {
        g_accum.macHashes[i] = 0;
    }
    portEXIT_CRITICAL(&g_accumMux);
}

void applyChannel(int ch) {
    esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
    dwellStartedMs = millis();
}

void ensureWifiMonitor() {
    static bool started = false;
    if (started) return;
    started = true;

    WiFi.persistent(false);
    WiFi.disconnect(true, true);
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);

    wifi_country_t country = {
        .cc = "EU",
        .schan = 1,
        .nchan = 13,
        .max_tx_power = 20,
        .policy = WIFI_COUNTRY_POLICY_AUTO
    };
    esp_wifi_set_country(&country);

    wifi_promiscuous_filter_t filt{};
    filt.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA | WIFI_PROMIS_FILTER_MASK_CTRL;
    esp_wifi_set_promiscuous_filter(&filt);
    esp_wifi_set_promiscuous_rx_cb(promiscuousCb);
    esp_wifi_set_promiscuous(true);

    currentChannel = 1;
    resetAccum();
    applyChannel(currentChannel);
}

float computeBusyScore(const ChannelMetrics& m) {
    const float dwellSec = static_cast<float>(kDwellMs) / 1000.0f;
    const float pps = m.frames / dwellSec;           // packets per second
    const float bps = m.bytes / dwellSec;            // bytes per second
    const float strongRatio = (m.frames > 0) ? (static_cast<float>(m.strong) / static_cast<float>(m.frames)) : 0.0f;

    // Log-scaled terms to keep stability across quiet and busy environments.
    const float ppsScore = clamp01(log1pf(pps) / logf(600.0f));          // ~600 pps -> near 1
    const float bpsScore = clamp01(log1pf(bps) / logf(50000.0f));        // ~50 KB/s -> near 1
    const float uniqueScore = clamp01(log1pf(static_cast<float>(m.unique)) / logf(20.0f)); // soft cap ~20 talkers

    const float raw = 0.40f * ppsScore + 0.30f * bpsScore + 0.20f * strongRatio + 0.10f * uniqueScore;
    return clamp01(raw) * 100.0f;
}

void finishDwell() {
    ChannelMetrics snap{};
    portENTER_CRITICAL(&g_accumMux);
    snap.frames = g_accum.frames;
    snap.bytes = g_accum.bytes;
    snap.strong = g_accum.strong;
    snap.unique = g_accum.unique;
    portEXIT_CRITICAL(&g_accumMux);

    ChannelState& ch = channels[currentChannel - 1];
    ch.metrics = snap;
    ch.busyCurrent = computeBusyScore(snap);
    if (!ch.hasData) {
        ch.busyEma = ch.busyCurrent;
        ch.hasData = true;
    } else {
        ch.busyEma = (1.0f - kBusyEmaAlpha) * ch.busyEma + kBusyEmaAlpha * ch.busyCurrent;
    }
}

void hopIfNeeded() {
    const uint32_t now = millis();
    if ((now - dwellStartedMs) < kDwellMs) return;

    finishDwell();
    resetAccum();

    currentChannel += 1;
    if (currentChannel > kChannelCount) currentChannel = 1;
    applyChannel(currentChannel);
}

float globalActivityMax() {
    float maxVal = 0.0f;
    for (int i = 0; i < kChannelCount; i++) {
        if (channels[i].hasData && channels[i].busyEma > maxVal) {
            maxVal = channels[i].busyEma;
        }
    }
    return maxVal;
}

void sortTop3(int outIdx[3]) {
    for (int i = 0; i < 3; i++) outIdx[i] = -1;
    for (int i = 0; i < kChannelCount; i++) {
        if (!channels[i].hasData) continue;
        for (int pos = 0; pos < 3; pos++) {
            if (outIdx[pos] == -1 || channels[i].busyEma > channels[outIdx[pos]].busyEma) {
                for (int shift = 2; shift > pos; shift--) outIdx[shift] = outIdx[shift - 1];
                outIdx[pos] = i;
                break;
            }
        }
    }
}

lv_obj_t* make_label(lv_obj_t* parent, const char* txt, lv_color_t color, bool mono=false) {
    lv_obj_t* lbl = lv_label_create(parent);
    lv_label_set_text(lbl, txt);
    lv_obj_set_style_text_color(lbl, color, 0);
    if (mono) lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    return lbl;
}

void buildUi() {
    root = lv_obj_create(lv_scr_act());
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(root, c565(BG_565), 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_pad_all(root, 4, 0);

    // Header
    lv_obj_t* header = lv_obj_create(root);
    lv_obj_set_size(header, LV_PCT(100), 34);
    lv_obj_set_style_bg_color(header, c565(PANEL_565), 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_radius(header, 4, 0);
    lv_obj_set_style_pad_all(header, 4, 0);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);

    titleLabel = make_label(header, "Bandwatch", c565(WHITE_565), true);
    lv_obj_align(titleLabel, LV_ALIGN_LEFT_MID, 4, 0);

    methodLabel = make_label(header, "max", c565(CYAN_565));
    lv_obj_align(methodLabel, LV_ALIGN_RIGHT_MID, -2, 0);

    // Global activity bar
    lv_obj_t* globalWrap = lv_obj_create(root);
    lv_obj_set_size(globalWrap, LV_PCT(100), 50);
    lv_obj_set_style_bg_color(globalWrap, c565(PANEL_565), 0);
    lv_obj_set_style_border_width(globalWrap, 0, 0);
    lv_obj_set_style_radius(globalWrap, 6, 0);
    lv_obj_set_style_pad_all(globalWrap, 8, 0);
    lv_obj_align(globalWrap, LV_ALIGN_TOP_MID, 0, 40);
    globalLabel = make_label(globalWrap, "Global 0", c565(WHITE_565), true);
    lv_obj_align(globalLabel, LV_ALIGN_TOP_LEFT, 0, -2);

    globalBar = lv_bar_create(globalWrap);
    lv_bar_set_range(globalBar, 0, 100);
    lv_obj_set_size(globalBar, 150, 12);
    lv_obj_align(globalBar, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_set_style_bg_color(globalBar, c565(BLACK_565), 0);
    lv_obj_set_style_bg_opa(globalBar, LV_OPA_40, 0);

    // Top 3 busiest channels
    lv_obj_t* topBox = lv_obj_create(root);
    lv_obj_set_size(topBox, LV_PCT(100), 74);
    lv_obj_set_style_bg_color(topBox, c565(BG_565), 0);
    lv_obj_set_style_border_width(topBox, 0, 0);
    lv_obj_set_style_pad_all(topBox, 4, 0);
    lv_obj_align(topBox, LV_ALIGN_TOP_MID, 0, 92);
    make_label(topBox, "Top", c565(YELLOW_565), true);
    for (int i = 0; i < 3; i++) {
        topRows[i] = make_label(topBox, "--", c565(WHITE_565));
        lv_obj_set_style_text_font(topRows[i], &lv_font_montserrat_14, 0);
        lv_obj_align(topRows[i], LV_ALIGN_TOP_LEFT, 0, 14 + i * 18);
    }

    // Channel strip visualization
    lv_obj_t* strip = lv_obj_create(root);
    lv_obj_set_size(strip, LV_PCT(100), 132);
    lv_obj_set_style_bg_color(strip, c565(PANEL_565), 0);
    lv_obj_set_style_border_width(strip, 0, 0);
    lv_obj_set_style_radius(strip, 6, 0);
    lv_obj_set_style_pad_all(strip, 4, 0);
    lv_obj_set_style_pad_row(strip, 4, 0);
    lv_obj_set_flex_flow(strip, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(strip, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_align(strip, LV_ALIGN_BOTTOM_MID, 0, -4);

    for (int i = 0; i < kChannelCount; i++) {
        lv_obj_t* row = lv_obj_create(strip);
        lv_obj_set_size(row, LV_PCT(100), 14);
        lv_obj_set_style_bg_color(row, c565(BG_565), 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        stripLabels[i] = make_label(row, "Ch01", c565(CYAN_565));
        lv_obj_set_style_text_font(stripLabels[i], &lv_font_montserrat_14, 0);
        lv_obj_set_width(stripLabels[i], 34);

        stripBars[i] = lv_bar_create(row);
        lv_bar_set_range(stripBars[i], 0, 100);
        lv_obj_set_size(stripBars[i], 112, 10);
        lv_obj_set_style_bg_color(stripBars[i], c565(BLACK_565), 0);
        lv_obj_set_style_bg_opa(stripBars[i], LV_OPA_30, 0);
        lv_obj_set_style_radius(stripBars[i], 3, 0);
    }

    dwellLabel = make_label(root, "Channel dwell", c565(WHITE_565));
    lv_obj_set_style_text_font(dwellLabel, &lv_font_montserrat_14, 0);
    lv_obj_align(dwellLabel, LV_ALIGN_TOP_LEFT, 6, 232);
}

void refreshUi() {
    const float global = globalActivityMax();
    lv_bar_set_value(globalBar, static_cast<int>(global + 0.5f), LV_ANIM_OFF);

    lv_color_t barColor = c565(GREEN_565);
    if (global > 70.0f) barColor = c565(RED_565);
    else if (global > 40.0f) barColor = c565(YELLOW_565);
    lv_obj_set_style_bg_color(globalBar, barColor, LV_PART_INDICATOR);

    char buf[64];
    snprintf(buf, sizeof(buf), "Global %.0f", global);
    lv_label_set_text(globalLabel, buf);

    int top[3];
    sortTop3(top);
    for (int i = 0; i < 3; i++) {
        if (top[i] < 0) {
            lv_label_set_text(topRows[i], "--");
            continue;
        }
        const ChannelState& ch = channels[top[i]];
        snprintf(buf, sizeof(buf), "%d)%02d %.0f p:%lu s:%u u:%u", i + 1, top[i] + 1, ch.busyEma, static_cast<unsigned long>(ch.metrics.frames), ch.metrics.strong, ch.metrics.unique);
        lv_label_set_text(topRows[i], buf);
    }

    for (int i = 0; i < kChannelCount; i++) {
        snprintf(buf, sizeof(buf), "Ch%02d%s", i + 1, (i + 1 == currentChannel) ? " *" : "");
        lv_label_set_text(stripLabels[i], buf);
        lv_bar_set_value(stripBars[i], static_cast<int>(channels[i].busyEma + 0.5f), LV_ANIM_OFF);
    }

    Accum live{};
    portENTER_CRITICAL(&g_accumMux);
    live.frames = g_accum.frames;
    live.bytes = g_accum.bytes;
    live.strong = g_accum.strong;
    live.unique = g_accum.unique;
    portEXIT_CRITICAL(&g_accumMux);

    snprintf(buf, sizeof(buf), "Ch%02d dwell%lums pkt %lu bytes %lu", currentChannel, static_cast<unsigned long>(kDwellMs), static_cast<unsigned long>(live.frames), static_cast<unsigned long>(live.bytes));
    lv_label_set_text(dwellLabel, buf);
}

void uiTimerCb(lv_timer_t* t) {
    (void)t;
    ensureWifiMonitor();
    hopIfNeeded();
    refreshUi();
}

} // namespace

void Tamagotchi_Init(void) {
    buildUi();
    lv_timer_create(uiTimerCb, kUiIntervalMs, nullptr);
    ensureWifiMonitor();
    refreshUi();
}

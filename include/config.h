#pragma once

#include <Arduino.h>

// ============================================================
// FIRMWARE
// ============================================================

#define FIRMWARE_NAME "Hue Light Control"
#define FIRMWARE_VERSION "2.2.0"
#define DEVICE_HOSTNAME "hue-light-control"

// ============================================================
// SECRETS / IDS (from .env via load_env.py)
// ============================================================
//
// Empty fallbacks let the firmware still boot and show
// "Settings missing" on the OLED instead of failing to compile.

#ifndef WIFI_SSID
#define WIFI_SSID ""
#endif

#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD ""
#endif

#ifndef HUE_BRIDGE_IP
#define HUE_BRIDGE_IP ""
#endif

#ifndef HUE_USERNAME
#define HUE_USERNAME ""
#endif

#ifndef HUE_GROUP_ID
#define HUE_GROUP_ID ""
#endif

#ifndef HUE_GROUP_NAME
#define HUE_GROUP_NAME "Hue Lights"
#endif

// ============================================================
// PIN ASSIGNMENTS
// ============================================================

// OLED - I2C
#define OLED_SDA 23
#define OLED_SCL 18

// Rotary encoder
#define ENCODER_BUTTON_PIN 19
#define ENCODER_CLOCK_PIN 21
#define ENCODER_DATA_PIN 22

// ============================================================
// OLED
// ============================================================

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define OLED_ADDRESS 0x3C
#define OLED_ADDRESS_ALT 0x3D

// Two-colour panels: rows 0-15 are yellow, 16-63 blue. Titles
// stay inside the yellow band; content starts below it.
#define HEADER_ZONE_HEIGHT 16

// ============================================================
// INPUT
// ============================================================

const unsigned long BUTTON_DEBOUNCE_MS = 30;

// Main screen, acting on release:
//   shorter than TAP_MAX_MS          toggle lights
//   TAP_MAX_MS .. SETTINGS_HOLD_MS   cancel (nothing)
//   SETTINGS_HOLD_MS .. RESTART      open Settings
//   RESTART_HOLD_MS or longer        restart the device
// Turning the knob while holding cancels.
const unsigned long TAP_MAX_MS = 800;
const unsigned long SETTINGS_HOLD_MS = 2000;
const unsigned long RESTART_HOLD_MS = 6000;

// Inside Settings, holding this long goes back one level.
const unsigned long BACK_HOLD_MS = 1000;

// "Held" longer than this is treated as a wiring fault: the
// button is ignored and the screen says so, until it releases.
const unsigned long BUTTON_STUCK_MS = 15000;

const int BRIGHTNESS_STEP = 13;
const int TRANSITIONS_PER_DETENT = 4;

// ============================================================
// HUE
// ============================================================

// Units are 100 ms.
const int POWER_TRANSITION_TIME = 4;      // 400 ms
const int BRIGHTNESS_TRANSITION_TIME = 2; // 200 ms

// Minimum gap between brightness commands while turning. With a
// kept-alive connection each command takes well under 100 ms, so
// this sets the update rate. Each command's transition matches it,
// which makes the ramp look continuous. If the bridge pushes back
// (busy/rate limited) the network task backs off automatically.
const unsigned long BRIGHTNESS_SEND_INTERVAL_MS = 200;

const unsigned long HUE_POLL_INTERVAL_MS = 15000;
const unsigned long HUE_POLL_INTERVAL_UNHEALTHY_MS = 5000;
const unsigned long USER_ACTION_POLL_GUARD_MS = 3000;

// A toggle that can't be delivered is dropped after this many
// tries so the light doesn't flip unexpectedly much later.
const int TOGGLE_MAX_ATTEMPTS = 3;

// If the bridge closes our idle connection, reopen it in the
// background at most this often.
const unsigned long BRIDGE_REWARM_MIN_INTERVAL_MS = 5000;

// Bridge rediscovery (IP changed after a power cut / DHCP).
const int BRIDGE_FAILURES_BEFORE_DISCOVERY = 3;
const unsigned long MDNS_DISCOVERY_INTERVAL_MS = 120000;
// discovery.meethue.com is rate limited; stay well under it.
const unsigned long CLOUD_DISCOVERY_INTERVAL_MS = 900000;

// ============================================================
// HTTP
// ============================================================

const uint16_t HTTP_TIMEOUT_MS = 2500;
const int32_t HTTP_CONNECT_TIMEOUT_MS = 1500;

// A TLS handshake needs roughly this much contiguous heap.
const uint32_t MIN_TLS_HEAP_BYTES = 40000;

// ============================================================
// WI-FI
// ============================================================

const unsigned long WIFI_CONNECT_TIMEOUT_MS = 15000;
const unsigned long WIFI_RETRY_MIN_MS = 2000;
const unsigned long WIFI_RETRY_MAX_MS = 30000;

// Every N failed attempts, power-cycle the radio.
const int WIFI_HARD_RESET_EVERY = 5;

// Reboot if Wi-Fi has been down this long (and Settings is closed).
const unsigned long WIFI_REBOOT_AFTER_MS = 20UL * 60UL * 1000UL;

const int RSSI_GOOD_DBM = -67;
const int RSSI_WEAK_DBM = -80;

// ============================================================
// DISPLAY / UI
// ============================================================

const unsigned long DISPLAY_INTERVAL_MS = 50;
const unsigned long DISPLAY_CHECK_INTERVAL_MS = 5000;

// Settings closes itself after this much inactivity.
const unsigned long SETTINGS_IDLE_EXIT_MS = 120000;

// ============================================================
// WATCHDOG
// ============================================================

// Log when the UI loop is blocked this long (serial monitor).
const unsigned long LOOP_STALL_LOG_MS = 150;

// Reboot if the network task stops making progress.
const unsigned long NETWORK_TASK_STALL_MS = 60000;

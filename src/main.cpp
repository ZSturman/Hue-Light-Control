// ============================================================
// HUE LIGHT CONTROL
// ============================================================
//
// ESP32 + rotary encoder + SSD1306 OLED that controls one Hue
// room/zone.
//
//   Turn         brightness
//   Tap          on / off
//   Hold 2 s     Settings (release when the screen says so)
//   Hold 6 s     restart
//   Hold at boot Settings straight away (recovery)
//
// Layout:
//   config.h       pins, timings, .env fallbacks
//   app_state      shared state, health codes, saved settings
//   input          encoder + button gestures
//   hue_network    Wi-Fi, Hue HTTPS, discovery (core 0 task)
//   display_ui     main screen + Settings screens

#include <Arduino.h>

#include "app_state.h"
#include "config.h"
#include "display_ui.h"
#include "hue_network.h"
#include "input.h"

// ============================================================
// NETWORK TASK WATCHDOG
// ============================================================
//
// Every network call has a timeout, but if the task still stops
// making progress, reboot rather than sit on a stale screen.

static void checkNetworkWatchdog() {
  unsigned long heartbeat;

  {
    StateLock lock;

    heartbeat = shared.networkHeartbeat;
  }

  if (millis() - heartbeat < NETWORK_TASK_STALL_MS) {
    return;
  }

  Serial.println("Network task stalled; rebooting");

  recordRestartNote("Network task stalled");

  delay(100);

  ESP.restart();
}

// ============================================================
// SETUP
// ============================================================

void setup() {
  Serial.begin(115200);

  delay(300);

  Serial.println();
  Serial.print(FIRMWARE_NAME " v" FIRMWARE_VERSION "  reset: ");
  Serial.println(resetReasonName());

  initSharedState();

  loadSettings();

  if (lastRestartNote[0] != '\0') {
    Serial.print("Previous restart: ");
    Serial.println(lastRestartNote);
  }

  initInput();

  initDisplay();

  showBootScreen();

  startNetwork();

  // Recovery path: hold the button while plugging in.
  if (buttonHeldAtBoot()) {
    Serial.println("Button held at boot: opening Settings");
    openSettings();
  }

  Serial.println("Ready. Hold the button 2 s for Settings.");
}

// ============================================================
// MAIN LOOP
// ============================================================

void loop() {
  // There are NO HTTP calls anywhere in this loop.

  // Anything that blocks this loop also delays button and knob
  // handling, so report it.
  static unsigned long lastLoopAt = millis();
  unsigned long loopStart = millis();

  if (loopStart - lastLoopAt >= LOOP_STALL_LOG_MS) {
    Serial.printf("Loop stalled %lu ms\n", loopStart - lastLoopAt);
  }

  lastLoopAt = loopStart;

  int detents = readEncoderDetents();

  ButtonEvent event = readButton();

  uiHandleInput(detents, event);

  uiUpdate();

  checkNetworkWatchdog();

  // A 1 ms yield is enough to prevent this loop from starving
  // other RTOS work while still sampling the encoder rapidly.
  delay(1);
}

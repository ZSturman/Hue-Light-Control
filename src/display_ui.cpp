#include "display_ui.h"

#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WiFi.h>
#include <Wire.h>
#include <esp_system.h>

#include "app_state.h"
#include "config.h"
#include "hue_network.h"

static Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// ============================================================
// DISPLAY STATE
// ============================================================

static uint8_t oledAddress = OLED_ADDRESS;

// Buffer allocated (display.begin succeeded).
static bool displayReady = false;

// Panel answered on I2C at the last check.
static bool displayPresent = false;

static bool displayAwake = true;

static unsigned long lastDisplayActivity = 0;
static unsigned long lastDisplayUpdate = 0;
static unsigned long lastDisplayCheck = 0;

// ============================================================
// LAYOUT
// ============================================================
//
// Titles sit inside the yellow band (rows 0-15). Everything
// else starts at CONTENT_Y so nothing straddles the colour split.

static const int TITLE_Y = 4;
static const int CONTENT_Y = HEADER_ZONE_HEIGHT + 1;
static const int LINE_HEIGHT = 9;
static const int CONTENT_LINES = 5;

static const int MENU_ROW_HEIGHT = 12;
static const int MENU_VISIBLE_ROWS = 4;

// ============================================================
// UI STATE
// ============================================================

enum class Screen : uint8_t {
  Main,
  Menu,
  Status,
  SelfTest,
  Command,
  KnobTest,
  I2CScan,
  ScreenTest,
};

enum MenuItem : uint8_t {
  // Main menu
  ITEM_STATUS,
  ITEM_TEST_AND_FIX,
  ITEM_MORE,
  ITEM_EXIT,

  // More menu
  ITEM_FLASH,
  ITEM_RECONNECT,
  ITEM_FIND_BRIDGE,
  ITEM_KNOB_TEST,
  ITEM_I2C_SCAN,
  ITEM_SCREEN_TEST,
  ITEM_SLEEP,
  ITEM_CONTRAST,
  ITEM_FLIP,
  ITEM_RESTART,
  ITEM_FACTORY_RESET,
  ITEM_BACK,
};

static const uint8_t MAIN_MENU[] = {ITEM_STATUS, ITEM_TEST_AND_FIX, ITEM_MORE,
                                    ITEM_EXIT};

static const uint8_t MORE_MENU[] = {
    ITEM_FLASH,       ITEM_RECONNECT, ITEM_FIND_BRIDGE, ITEM_KNOB_TEST,
    ITEM_I2C_SCAN,    ITEM_SCREEN_TEST, ITEM_SLEEP,     ITEM_CONTRAST,
    ITEM_FLIP,        ITEM_RESTART,   ITEM_FACTORY_RESET, ITEM_BACK};

static const int MAIN_MENU_COUNT = sizeof(MAIN_MENU);
static const int MORE_MENU_COUNT = sizeof(MORE_MENU);

static const int STATUS_PAGE_COUNT = 4;
static const int SCREEN_TEST_PATTERNS = 4;

static Screen screen = Screen::Main;

static bool inMoreMenu = false;
static int menuIndex = 0;
static int menuTop = 0;

// Where the main menu cursor was before opening More.
static int mainMenuIndex = 0;

// Status page, screen-test pattern, or self-test cursor (-1 = none).
static int subIndex = 0;

static const char *commandTitle = "";

static unsigned long lastInputAt = 0;

// "Press again to confirm" for destructive items.
static int confirmItem = -1;
static unsigned long confirmUntil = 0;

// The current press already did something (went back a level),
// so its release is ignored.
static bool holdHandled = false;

// Knob turned during a main-screen hold: release does nothing.
static bool holdCancelled = false;

static uint8_t i2cFound[16];
static int i2cCount = 0;

// ============================================================
// SETTINGS VALUES
// ============================================================

static const unsigned long SLEEP_OPTIONS_MS[] = {15000, 30000, 60000, 300000,
                                                 0};
static const char *SLEEP_LABELS[] = {"15s", "30s", "1m", "5m", "Never"};
static const int SLEEP_OPTION_COUNT = 5;

static const uint8_t CONTRAST_VALUES[] = {0x08, 0x60, 0xCF};
static const char *CONTRAST_LABELS[] = {"Low", "Med", "High"};
static const int CONTRAST_OPTION_COUNT = 3;

// ============================================================
// LOW-LEVEL OLED
// ============================================================

static bool probeI2C(uint8_t address) {
  Wire.beginTransmission(address);

  return Wire.endTransmission() == 0;
}

static void applyDisplaySettings() {
  if (settings.contrastIndex >= CONTRAST_OPTION_COUNT) {
    settings.contrastIndex = CONTRAST_OPTION_COUNT - 1;
  }

  if (settings.sleepIndex >= SLEEP_OPTION_COUNT) {
    settings.sleepIndex = 1;
  }

  display.ssd1306_command(SSD1306_SETCONTRAST);
  display.ssd1306_command(CONTRAST_VALUES[settings.contrastIndex]);

  display.setRotation(settings.flipped ? 2 : 0);
}

static void beginPanel(bool firstTime) {
  // Adafruit's begin() returns true even with nothing wired, so
  // presence is checked separately with an I2C probe.
  displayReady = display.begin(SSD1306_SWITCHCAPVCC, oledAddress, false,
                               firstTime);

  if (!displayReady) {
    Serial.println("OLED: buffer allocation failed");
    return;
  }

  applyDisplaySettings();

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.display();

  displayAwake = true;
  lastDisplayActivity = millis();
}

void initDisplay() {
  Wire.begin(OLED_SDA, OLED_SCL);
  Wire.setClock(400000);

  // Keep a missing/unplugged OLED from stalling the UI loop.
  Wire.setTimeOut(20);

  // The panel can miss the very first probe after reset, so give
  // it a few tries before deciding it isn't there.
  displayPresent = false;

  for (int attempt = 0; attempt < 5 && !displayPresent; attempt++) {
    if (probeI2C(OLED_ADDRESS)) {
      oledAddress = OLED_ADDRESS;
      displayPresent = true;
    } else if (probeI2C(OLED_ADDRESS_ALT)) {
      oledAddress = OLED_ADDRESS_ALT;
      displayPresent = true;
    } else {
      delay(20);
    }
  }

  if (displayPresent) {
    Serial.printf("OLED: found at 0x%02X\n", oledAddress);
  } else {
    Serial.println("OLED: not found at 0x3C/0x3D. Check SDA/SCL/power.");
  }

  beginPanel(true);
}

// A loose OLED cable or brief power dip blanks the panel. Detect
// it coming back and re-run the init sequence.
static void checkDisplayConnection() {
  unsigned long now = millis();

  if (now - lastDisplayCheck < DISPLAY_CHECK_INTERVAL_MS) {
    return;
  }

  lastDisplayCheck = now;

  bool present = probeI2C(oledAddress);

  if (present && !displayPresent) {
    Serial.println("OLED: reconnected, reinitialising");
    displayPresent = true;
    beginPanel(false);
  } else if (!present && displayPresent) {
    Serial.println("OLED: lost (check wiring)");
    displayPresent = false;
  }
}

static bool canDraw() { return displayReady && displayPresent && displayAwake; }

// ============================================================
// SLEEP / WAKE
// ============================================================

static void wakeDisplay() {
  lastDisplayActivity = millis();

  if (!displayReady || displayAwake) {
    return;
  }

  displayAwake = true;

  display.ssd1306_command(SSD1306_DISPLAYON);
}

static void sleepDisplay() {
  if (!displayReady || !displayAwake) {
    return;
  }

  display.ssd1306_command(SSD1306_DISPLAYOFF);

  displayAwake = false;
}

static void processDisplaySleep() {
  if (!displayAwake || screen != Screen::Main) {
    return;
  }

  unsigned long timeout = SLEEP_OPTIONS_MS[settings.sleepIndex];

  if (timeout == 0) {
    return;
  }

  if (millis() - lastDisplayActivity >= timeout) {
    sleepDisplay();
  }
}

// ============================================================
// DRAWING HELPERS
// ============================================================

static void drawTitle(const char *title, const char *right = nullptr) {
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, TITLE_Y);
  display.print(title);

  if (right != nullptr) {
    display.setCursor(SCREEN_WIDTH - strlen(right) * 6, TITLE_Y);
    display.print(right);
  }
}

static void drawLine(int row, const char *text) {
  display.setCursor(0, CONTENT_Y + row * LINE_HEIGHT);
  display.print(text);
}

static void drawCentered(const char *text, int y, int textSize) {
  int width = strlen(text) * 6 * textSize;

  display.setTextSize(textSize);
  display.setCursor(max(0, (SCREEN_WIDTH - width) / 2), y);
  display.print(text);
  display.setTextSize(1);
}

// Word-wraps text into up to maxLines lines of 21 characters.
// Returns the number of lines used.
static int drawWrapped(const char *text, int firstRow, int maxLines) {
  const int width = 21;

  int line = 0;
  const char *cursor = text;

  while (*cursor != '\0' && line < maxLines) {
    while (*cursor == ' ') {
      cursor++;
    }

    int length = strlen(cursor);
    int take = min(length, width);

    if (length > width) {
      int lastSpace = -1;

      for (int i = 0; i <= width && i < length; i++) {
        if (cursor[i] == ' ') {
          lastSpace = i;
        }
      }

      if (lastSpace > 0) {
        take = lastSpace;
      }
    }

    char buffer[width + 1];
    memcpy(buffer, cursor, take);
    buffer[take] = '\0';

    drawLine(firstRow + line, buffer);

    cursor += take;
    line++;
  }

  return line;
}

static void formatAgo(unsigned long ms, char *out, size_t size) {
  unsigned long seconds = ms / 1000;

  if (seconds < 60) {
    snprintf(out, size, "%lus", seconds);
  } else if (seconds < 3600) {
    snprintf(out, size, "%lum", seconds / 60);
  } else if (seconds < 86400) {
    snprintf(out, size, "%luh%02lum", seconds / 3600, (seconds / 60) % 60);
  } else {
    snprintf(out, size, "%lud%02luh", seconds / 86400, (seconds / 3600) % 24);
  }
}

static void drawSpinner(int x, int y) {
  static const char frames[] = {'|', '/', '-', '\\'};

  display.setCursor(x, y);
  display.print(frames[(millis() / 150) % 4]);
}

static void drawProgressBar(int y, int height, long value, long full) {
  display.drawRect(0, y, SCREEN_WIDTH, height, SSD1306_WHITE);

  long width = constrain(value * (SCREEN_WIDTH - 4) / full, 0L,
                         static_cast<long>(SCREEN_WIDTH - 4));

  display.fillRect(2, y + 2, width, height - 4, SSD1306_WHITE);
}

// ============================================================
// MAIN SCREEN
// ============================================================

// On/off as the user expects it: the bridge's state, flipped once
// for every press that hasn't been confirmed yet. Updates the
// moment the button is tapped; snaps back if a toggle fails.
static bool displayedRoomOn(const SharedState &state) {
  uint32_t pending =
      state.requestedToggleGeneration - state.processedToggleGeneration;

  return state.roomOn != ((pending & 1) != 0);
}

static void drawMainFooter(const SharedState &state) {
  display.setTextSize(1);
  display.setCursor(0, 55);

  // Hardware fault beats everything: the button can't be used.
  if (buttonStuck()) {
    if ((millis() / 2500) % 2 == 0) {
      display.print("Button stuck pressed");
    } else {
      display.print("Check button wiring");
    }
    return;
  }

  // Something's wrong: alternate the reason with how to get help.
  if (!isHealthOk(state.health)) {
    if ((millis() / 2500) % 2 == 0) {
      display.print(describeHealth(state.health).message);
    } else {
      display.print("Hold 2s for help");
    }
    return;
  }

  if (!displayedRoomOn(state)) {
    display.print("Press: on");
  } else if (state.brightnessDirty) {
    display.print("Adjusting");
  } else {
    display.print("Turn: dim");
  }

  display.setCursor(98, 55);
  display.print("Ready");
}

static void drawMainScreen() {
  SharedState state = snapshotState();

  int percent = map(state.brightness, 1, 254, 1, 100);

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  // ==========================================================
  // HEADER
  // ==========================================================

  display.setTextSize(1);

  const char *tag = describeHealth(state.health).tag;
  int tagWidth = strlen(tag) * 6;
  int tagX = SCREEN_WIDTH - tagWidth - 2;

  // Room name, truncated so it never runs into the tag.
  char name[22];
  int maxNameChars = (tagX - 4) / 6;
  strlcpy(name, HUE_GROUP_NAME, min<int>(sizeof(name), maxNameChars + 1));

  display.setCursor(0, 0);
  display.print(name);

  if (isHealthOk(state.health)) {
    display.setCursor(tagX, 0);
    display.print(tag);
  } else {
    // Inverted box so a problem stands out.
    display.fillRect(tagX - 2, 0, tagWidth + 3, 9, SSD1306_WHITE);
    display.setTextColor(SSD1306_BLACK);
    display.setCursor(tagX, 1);
    display.print(tag);
    display.setTextColor(SSD1306_WHITE);
  }

  display.drawLine(0, 10, 127, 10, SSD1306_WHITE);

  // ==========================================================
  // ON / OFF
  // ==========================================================

  display.setTextSize(2);
  display.setCursor(0, 20);

  if (!state.roomStateKnown) {
    display.print("--");
  } else if (displayedRoomOn(state)) {
    display.print("ON");
  } else {
    display.print("OFF");
  }

  // ==========================================================
  // BRIGHTNESS %
  // ==========================================================

  if (percent >= 100) {
    display.setCursor(78, 20);
  } else if (percent >= 10) {
    display.setCursor(90, 20);
  } else {
    display.setCursor(102, 20);
  }

  display.print(percent);
  display.print("%");

  // ==========================================================
  // BRIGHTNESS BAR
  // ==========================================================

  display.drawRect(0, 40, 128, 11, SSD1306_WHITE);

  int barWidth = map(state.brightness, 1, 254, 0, 124);

  if (barWidth > 0) {
    display.fillRect(2, 42, barWidth, 7, SSD1306_WHITE);
  }

  // ==========================================================
  // FOOTER
  // ==========================================================

  drawMainFooter(state);

  display.display();
}

// Full-screen countdown while the button is held on the main
// screen, so it's always clear what letting go will do.
static void drawHoldScreen(unsigned long held) {
  display.clearDisplay();

  if (held < SETTINGS_HOLD_MS) {
    drawTitle("Keep holding...");
    drawCentered("Settings", 22, 2);
    drawProgressBar(42, 8, held - TAP_MAX_MS, SETTINGS_HOLD_MS - TAP_MAX_MS);
    drawLine(4, "Let go now: cancel");
  } else if (held < RESTART_HOLD_MS) {
    drawTitle("Let go: Settings");
    drawCentered("Settings", 22, 2);
    drawProgressBar(42, 8, held - SETTINGS_HOLD_MS,
                    RESTART_HOLD_MS - SETTINGS_HOLD_MS);
    drawLine(4, "Hold more: restart");
  } else {
    drawTitle("Let go: Restart");
    drawCentered("Restart", 22, 2);
    drawProgressBar(42, 8, 1, 1);
    drawLine(4, "Turn knob: cancel");
  }

  display.display();
}

// ============================================================
// MENUS
// ============================================================

static const uint8_t *currentMenu() {
  return inMoreMenu ? MORE_MENU : MAIN_MENU;
}

static int currentMenuCount() {
  return inMoreMenu ? MORE_MENU_COUNT : MAIN_MENU_COUNT;
}

static bool confirmPending(int item) {
  return confirmItem == item && static_cast<long>(confirmUntil - millis()) > 0;
}

static void menuLabel(int item, char *out, size_t size) {
  switch (item) {
  case ITEM_STATUS:
    strlcpy(out, "Status & help", size);
    break;
  case ITEM_TEST_AND_FIX:
    strlcpy(out, "Test & fix", size);
    break;
  case ITEM_MORE:
    strlcpy(out, "More settings  >", size);
    break;
  case ITEM_EXIT:
    strlcpy(out, "Exit", size);
    break;
  case ITEM_FLASH:
    strlcpy(out, "Flash lights", size);
    break;
  case ITEM_RECONNECT:
    strlcpy(out, "Reconnect Wi-Fi", size);
    break;
  case ITEM_FIND_BRIDGE:
    strlcpy(out, "Find bridge", size);
    break;
  case ITEM_KNOB_TEST:
    strlcpy(out, "Knob test", size);
    break;
  case ITEM_I2C_SCAN:
    strlcpy(out, "I2C scan", size);
    break;
  case ITEM_SCREEN_TEST:
    strlcpy(out, "Screen test", size);
    break;
  case ITEM_SLEEP:
    snprintf(out, size, "Screen off: %s", SLEEP_LABELS[settings.sleepIndex]);
    break;
  case ITEM_CONTRAST:
    snprintf(out, size, "Contrast: %s",
             CONTRAST_LABELS[settings.contrastIndex]);
    break;
  case ITEM_FLIP:
    snprintf(out, size, "Flip screen: %s", settings.flipped ? "On" : "Off");
    break;
  case ITEM_RESTART:
    strlcpy(out, confirmPending(item) ? "Press again" : "Restart", size);
    break;
  case ITEM_FACTORY_RESET:
    strlcpy(out, confirmPending(item) ? "Press again: erase" : "Factory reset",
            size);
    break;
  case ITEM_BACK:
    strlcpy(out, "< Back", size);
    break;
  default:
    out[0] = '\0';
    break;
  }
}

static void drawMenuScreen() {
  SharedState state = snapshotState();

  const uint8_t *items = currentMenu();
  int count = currentMenuCount();

  display.clearDisplay();

  drawTitle(inMoreMenu ? "More settings" : "Settings",
            describeHealth(state.health).tag);

  bool scrolls = count > MENU_VISIBLE_ROWS;
  int rowWidth = scrolls ? SCREEN_WIDTH - 5 : SCREEN_WIDTH;

  for (int row = 0; row < MENU_VISIBLE_ROWS; row++) {
    int index = menuTop + row;

    if (index >= count) {
      break;
    }

    int y = HEADER_ZONE_HEIGHT + row * MENU_ROW_HEIGHT;

    char label[24];
    menuLabel(items[index], label, sizeof(label));

    if (index == menuIndex) {
      display.fillRect(0, y, rowWidth, MENU_ROW_HEIGHT, SSD1306_WHITE);
      display.setTextColor(SSD1306_BLACK);
    } else {
      display.setTextColor(SSD1306_WHITE);
    }

    display.setCursor(3, y + 2);
    display.print(label);
  }

  display.setTextColor(SSD1306_WHITE);

  if (scrolls) {
    int trackTop = HEADER_ZONE_HEIGHT;
    int trackHeight = SCREEN_HEIGHT - trackTop;
    int thumbHeight = max(4, trackHeight * MENU_VISIBLE_ROWS / count);
    int thumbY = trackTop + (trackHeight - thumbHeight) * menuIndex / (count - 1);

    display.drawFastVLine(126, trackTop, trackHeight, SSD1306_WHITE);
    display.fillRect(125, thumbY, 3, thumbHeight, SSD1306_WHITE);
  }

  display.display();
}

// ============================================================
// STATUS PAGES
// ============================================================

static void drawStatusScreen() {
  SharedState state = snapshotState();

  char line[32];
  char ago[12];

  static const char *titles[STATUS_PAGE_COUNT] = {"Health", "Wi-Fi",
                                                  "Bridge", "Device"};

  char pages[8];
  snprintf(pages, sizeof(pages), "%d/%d", subIndex + 1, STATUS_PAGE_COUNT);

  display.clearDisplay();

  drawTitle(titles[subIndex], pages);

  const HealthInfo &info = describeHealth(state.health);

  switch (subIndex) {
  case 0:
    drawLine(0, info.message);
    drawWrapped(info.hint, 1, 4);
    break;

  case 1:
    snprintf(line, sizeof(line), "SSID %.16s", WIFI_SSID);
    drawLine(0, line);

    if (state.wifiConnected) {
      snprintf(line, sizeof(line), "Connected %d dBm", state.rssi);
      drawLine(1, line);
      snprintf(line, sizeof(line), "IP %s",
               IPAddress(state.localIp).toString().c_str());
      drawLine(2, line);
    } else {
      drawLine(1, "Not connected");
      drawLine(2, "IP -");
    }

    snprintf(line, sizeof(line), "Why %u %s", state.wifiDisconnectReason,
             wifiReasonName(state.wifiDisconnectReason));
    drawLine(3, line);

    snprintf(line, sizeof(line), "Tries %lu  ch %u",
             static_cast<unsigned long>(state.wifiAttempts), state.wifiChannel);
    drawLine(4, line);
    break;

  case 2:
    snprintf(line, sizeof(line), "%s %s",
             state.bridgeIp[0] ? state.bridgeIp : "(no IP)",
             state.bridgeIpDiscovered ? "found" : ".env");
    drawLine(0, line);

    snprintf(line, sizeof(line), "HTTP %d  Hue err %d", state.lastHttpCode,
             state.lastHueErrorType);
    drawLine(1, line);

    if (state.lastHttpCode < 0 && state.lastHttpCode > -12) {
      drawLine(2, "(no connection)");
    } else if (state.lastHueError[0] != '\0') {
      snprintf(line, sizeof(line), "%.21s", state.lastHueError);
      drawLine(2, line);
    }

    if (state.lastHueSuccessAt == 0) {
      drawLine(3, "Last OK never");
    } else {
      formatAgo(millis() - state.lastHueSuccessAt, ago, sizeof(ago));
      snprintf(line, sizeof(line), "Last OK %s ago", ago);
      drawLine(3, line);
    }

    snprintf(line, sizeof(line), "Errors %lu streak %lu",
             static_cast<unsigned long>(state.totalErrors),
             static_cast<unsigned long>(state.hueFailureStreak));
    drawLine(4, line);
    break;

  case 3:
    snprintf(line, sizeof(line), "FW %s  Boots %lu", FIRMWARE_VERSION,
             static_cast<unsigned long>(bootCount));
    drawLine(0, line);

    formatAgo(millis(), ago, sizeof(ago));
    snprintf(line, sizeof(line), "Up %s", ago);
    drawLine(1, line);

    snprintf(line, sizeof(line), "Reset %s", resetReasonName());
    drawLine(2, line);

    snprintf(line, sizeof(line), "Note %.16s",
             lastRestartNote[0] ? lastRestartNote : "-");
    drawLine(3, line);

    snprintf(line, sizeof(line), "Heap %uk max %uk",
             static_cast<unsigned>(ESP.getFreeHeap() / 1024),
             static_cast<unsigned>(ESP.getMaxAllocHeap() / 1024));
    drawLine(4, line);
    break;
  }

  display.display();
}

// ============================================================
// SELF-TEST SCREEN
// ============================================================

static const char *TEST_NAMES[TEST_STEP_COUNT] = {"Config", "Wi-Fi", "Bridge",
                                                  "Hue", "Memory"};

static const char *testStatusLabel(TestStatus status) {
  switch (status) {
  case TestStatus::Pending:
    return "  - ";
  case TestStatus::Running:
    return " .. ";
  case TestStatus::Pass:
    return "PASS";
  case TestStatus::Warn:
    return "WARN";
  case TestStatus::Fail:
    return "FAIL";
  case TestStatus::Skip:
    return "SKIP";
  }

  return "";
}

static void drawSelfTestScreen() {
  SharedState state = snapshotState();

  display.clearDisplay();

  // Title doubles as the full detail for the highlighted step.
  if (subIndex >= 0 && state.testSteps[subIndex].detail[0] != '\0') {
    drawTitle(state.testSteps[subIndex].detail);
  } else if (state.selfTestRunning) {
    drawTitle("Testing...");
  } else {
    drawTitle("Test & fix", "turn");
  }

  for (int i = 0; i < TEST_STEP_COUNT; i++) {
    int y = CONTENT_Y + i * LINE_HEIGHT;

    const TestStep &step = state.testSteps[i];

    if (i == subIndex) {
      display.fillRect(0, y - 1, SCREEN_WIDTH, LINE_HEIGHT, SSD1306_WHITE);
      display.setTextColor(SSD1306_BLACK);
    } else {
      display.setTextColor(SSD1306_WHITE);
    }

    char line[24];
    snprintf(line, sizeof(line), "%-6s %s %.9s", TEST_NAMES[i],
             testStatusLabel(step.status), step.detail);

    display.setCursor(0, y);
    display.print(line);
  }

  display.setTextColor(SSD1306_WHITE);

  display.display();
}

// ============================================================
// COMMAND SCREEN (reconnect / find bridge / flash)
// ============================================================

static void drawCommandScreen() {
  SharedState state = snapshotState();

  bool busy = state.commandBusy || state.pendingCommand != NetCommand::None;

  display.clearDisplay();

  drawTitle(commandTitle);

  if (busy) {
    drawSpinner(0, CONTENT_Y + LINE_HEIGHT);
    display.setCursor(12, CONTENT_Y + LINE_HEIGHT);
    display.print(state.commandStatus[0] ? state.commandStatus : "Working...");
  } else {
    drawLine(1, state.commandStatus[0] ? state.commandStatus : "Done");
  }

  drawLine(3, describeHealth(state.health).message);

  drawLine(4, busy ? "Please wait" : "Press: back");

  display.display();
}

// ============================================================
// HARDWARE TESTS
// ============================================================

static void drawKnobTestScreen() {
  InputDiagnostics diagnostics = inputDiagnostics();

  bool button, clock, data;
  readRawPins(button, clock, data);

  char line[32];

  display.clearDisplay();

  drawTitle("Knob test", "hold=back");

  snprintf(line, sizeof(line), "Position %ld", diagnostics.position);
  drawLine(0, line);

  snprintf(line, sizeof(line), "Presses  %lu",
           static_cast<unsigned long>(diagnostics.buttonPresses));
  drawLine(1, line);

  snprintf(line, sizeof(line), "BTN %d  CLK %d  DT %d", !button, clock, data);
  drawLine(2, line);

  snprintf(line, sizeof(line), "Steps ok %lu bad %lu",
           static_cast<unsigned long>(diagnostics.validTransitions),
           static_cast<unsigned long>(diagnostics.invalidTransitions));
  drawLine(3, line);

  // Lots of invalid steps = bouncy contacts or a loose wire.
  if (buttonStuck()) {
    drawLine(4, "BTN stuck: check SW");
  } else if (diagnostics.validTransitions > 20 &&
      diagnostics.invalidTransitions * 4 > diagnostics.validTransitions) {
    drawLine(4, "Noisy! Check wiring");
  }

  display.display();
}

static void runI2CScan() {
  i2cCount = 0;

  for (uint8_t address = 1; address < 127; address++) {
    if (probeI2C(address) && i2cCount < 16) {
      i2cFound[i2cCount++] = address;
    }
  }

  Serial.print("I2C scan: ");
  Serial.print(i2cCount);
  Serial.println(" device(s)");
}

static void drawI2CScanScreen() {
  char line[32];

  display.clearDisplay();

  snprintf(line, sizeof(line), "I2C: %d found", i2cCount);
  drawTitle(line);

  if (i2cCount == 0) {
    drawLine(0, "Nothing found.");
    drawLine(1, "Check SDA/SCL/3V3");
  }

  for (int i = 0; i < i2cCount && i < 8; i++) {
    const char *name = "";

    if (i2cFound[i] == 0x3C || i2cFound[i] == 0x3D) {
      name = "OLED";
    }

    snprintf(line, sizeof(line), "0x%02X %s", i2cFound[i], name);

    // Two columns of four.
    display.setCursor((i / 4) * 64, CONTENT_Y + (i % 4) * LINE_HEIGHT);
    display.print(line);
  }

  drawLine(4, "Press: rescan");

  display.display();
}

static void drawScreenTestScreen() {
  display.clearDisplay();

  switch (subIndex) {
  case 0:
    display.fillRect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, SSD1306_WHITE);
    break;

  case 1:
  case 2:
    for (int y = 0; y < SCREEN_HEIGHT; y += 4) {
      for (int x = 0; x < SCREEN_WIDTH; x += 4) {
        if (((x + y) / 4) % 2 == subIndex - 1) {
          display.fillRect(x, y, 4, 4, SSD1306_WHITE);
        }
      }
    }
    break;

  case 3:
    display.drawRect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, SSD1306_WHITE);
    display.drawLine(0, 0, SCREEN_WIDTH - 1, SCREEN_HEIGHT - 1, SSD1306_WHITE);
    display.drawLine(SCREEN_WIDTH - 1, 0, 0, SCREEN_HEIGHT - 1, SSD1306_WHITE);
    display.setCursor(22, 28);
    display.print("Turn / Hold:back");
    break;
  }

  display.display();
}

// ============================================================
// NAVIGATION
// ============================================================

static void goToScreen(Screen next) {
  screen = next;
  subIndex = 0;

  setSettingsOpen(next != Screen::Main);
}

static void showMenu(bool more, int index) {
  inMoreMenu = more;
  menuIndex = index;
  menuTop = max(0, index - MENU_VISIBLE_ROWS + 1);
  confirmItem = -1;

  goToScreen(Screen::Menu);
}

// Back to whichever menu the current screen was opened from.
static void returnToMenu() { showMenu(inMoreMenu, menuIndex); }

static void goBack() {
  if (screen == Screen::Menu) {
    if (inMoreMenu) {
      showMenu(false, mainMenuIndex);
    } else {
      goToScreen(Screen::Main);
    }
    return;
  }

  returnToMenu();
}

void openSettings() {
  wakeDisplay();

  lastInputAt = millis();
  mainMenuIndex = 0;

  showMenu(false, 0);
}

static void restartDevice(const char *note) {
  if (displayReady) {
    display.clearDisplay();
    drawTitle("Restart");
    drawCentered("Restarting", 30, 1);
    display.display();
  }

  Serial.println("Restarting...");

  recordRestartNote(note);

  delay(800);

  ESP.restart();
}

static void startCommand(NetCommand command, const char *title) {
  commandTitle = title;

  if (!requestNetCommand(command)) {
    setCommandStatus("Busy, try again");
  }

  goToScreen(Screen::Command);
}

static void cycleSetting(int item) {
  switch (item) {
  case ITEM_SLEEP:
    settings.sleepIndex = (settings.sleepIndex + 1) % SLEEP_OPTION_COUNT;
    break;
  case ITEM_CONTRAST:
    settings.contrastIndex =
        (settings.contrastIndex + 1) % CONTRAST_OPTION_COUNT;
    break;
  case ITEM_FLIP:
    settings.flipped = !settings.flipped;
    break;
  }

  applyDisplaySettings();
  saveDisplaySettings();
}

static void activateMenuItem(int item) {
  // Destructive items need a second press within 3 s.
  if (item == ITEM_RESTART || item == ITEM_FACTORY_RESET) {
    if (!confirmPending(item)) {
      confirmItem = item;
      confirmUntil = millis() + 3000;
      return;
    }

    if (item == ITEM_FACTORY_RESET) {
      eraseAllSettings();
      restartDevice("Factory reset");
    } else {
      restartDevice("Restart from menu");
    }
  }

  confirmItem = -1;

  switch (item) {
  case ITEM_STATUS:
    goToScreen(Screen::Status);
    break;

  case ITEM_TEST_AND_FIX:
    // The self-test reconnects Wi-Fi and searches for the bridge
    // on its own when those steps fail.
    if (requestNetCommand(NetCommand::SelfTest)) {
      goToScreen(Screen::SelfTest);
      subIndex = -1;
    } else {
      commandTitle = "Test & fix";
      setCommandStatus("Busy, try again");
      goToScreen(Screen::Command);
    }
    break;

  case ITEM_MORE:
    mainMenuIndex = menuIndex;
    showMenu(true, 0);
    break;

  case ITEM_EXIT:
    goToScreen(Screen::Main);
    break;

  case ITEM_FLASH:
    startCommand(NetCommand::FlashLights, "Flash lights");
    break;

  case ITEM_RECONNECT:
    startCommand(NetCommand::ReconnectWiFi, "Reconnect Wi-Fi");
    break;

  case ITEM_FIND_BRIDGE:
    startCommand(NetCommand::FindBridge, "Find bridge");
    break;

  case ITEM_KNOB_TEST:
    goToScreen(Screen::KnobTest);
    break;

  case ITEM_I2C_SCAN:
    runI2CScan();
    goToScreen(Screen::I2CScan);
    break;

  case ITEM_SCREEN_TEST:
    goToScreen(Screen::ScreenTest);
    break;

  case ITEM_SLEEP:
  case ITEM_CONTRAST:
  case ITEM_FLIP:
    cycleSetting(item);
    break;

  case ITEM_BACK:
    goBack();
    break;
  }
}

static bool networkBusy() {
  SharedState state = snapshotState();

  return state.commandBusy || state.selfTestRunning ||
         state.pendingCommand != NetCommand::None;
}

// ============================================================
// INPUT
// ============================================================

static void handleMainScreenInput(int detents, ButtonEvent event) {
  if (detents != 0) {
    if (buttonIsDown()) {
      // Turning while holding cancels the hold.
      holdCancelled = true;
    } else {
      adjustBrightness(detents);
    }
  }

  if (event != ButtonEvent::Released || holdCancelled) {
    return;
  }

  unsigned long held = lastPressDurationMs();

  if (held < TAP_MAX_MS) {
    requestToggle();
  } else if (held < SETTINGS_HOLD_MS) {
    // Let go early: cancelled.
  } else if (held < RESTART_HOLD_MS) {
    openSettings();
  } else {
    restartDevice("Restart (button hold)");
  }
}

// A quick press (release before BACK_HOLD_MS) inside Settings.
static void handleSettingsPress() {
  switch (screen) {
  case Screen::Menu:
    activateMenuItem(currentMenu()[menuIndex]);
    break;

  case Screen::Status:
  case Screen::SelfTest:
  case Screen::Command:
    returnToMenu();
    break;

  case Screen::KnobTest:
    // Presses are part of the test.
    break;

  case Screen::I2CScan:
    runI2CScan();
    break;

  case Screen::ScreenTest:
    subIndex = (subIndex + 1) % SCREEN_TEST_PATTERNS;
    break;

  case Screen::Main:
    break;
  }
}

static void handleSettingsTurn(int detents) {
  switch (screen) {
  case Screen::Menu: {
    int count = currentMenuCount();

    menuIndex = constrain(menuIndex + detents, 0, count - 1);

    if (menuIndex < menuTop) {
      menuTop = menuIndex;
    } else if (menuIndex >= menuTop + MENU_VISIBLE_ROWS) {
      menuTop = menuIndex - MENU_VISIBLE_ROWS + 1;
    }

    confirmItem = -1;
    break;
  }

  case Screen::Status:
    subIndex = (subIndex + detents + STATUS_PAGE_COUNT * 4) %
               STATUS_PAGE_COUNT;
    break;

  case Screen::SelfTest:
    subIndex = constrain(subIndex + detents, -1, TEST_STEP_COUNT - 1);
    break;

  case Screen::ScreenTest:
    subIndex = (subIndex + detents + SCREEN_TEST_PATTERNS * 4) %
               SCREEN_TEST_PATTERNS;
    break;

  default:
    break;
  }
}

void uiHandleInput(int detents, ButtonEvent event) {
  if (detents == 0 && event == ButtonEvent::None) {
    return;
  }

  lastInputAt = millis();

  // Input while the screen is asleep still acts, as before.
  wakeDisplay();

  if (event == ButtonEvent::Pressed) {
    holdHandled = false;
    holdCancelled = false;
    return;
  }

  // This press already went back a level; ignore its release.
  if (event == ButtonEvent::Released && holdHandled) {
    holdHandled = false;
    return;
  }

  if (screen == Screen::Main) {
    handleMainScreenInput(detents, event);
    return;
  }

  if (detents != 0) {
    handleSettingsTurn(detents);
  }

  if (event == ButtonEvent::Released) {
    handleSettingsPress();
  }
}

// Inside Settings, holding goes back as soon as BACK_HOLD_MS is
// reached (no need to let go first).
static void processBackHold() {
  if (screen == Screen::Main || holdHandled) {
    return;
  }

  if (buttonHeldMs() >= BACK_HOLD_MS) {
    holdHandled = true;
    lastInputAt = millis();
    goBack();
  }
}

// ============================================================
// UPDATE
// ============================================================

void uiUpdate() {
  checkDisplayConnection();

  processBackHold();

  unsigned long now = millis();

  // Settings closes itself if forgotten, unless something is
  // still running.
  if (screen != Screen::Main && now - lastInputAt >= SETTINGS_IDLE_EXIT_MS &&
      !networkBusy()) {
    goToScreen(Screen::Main);
  }

  // Holding the button keeps the screen on.
  if (buttonIsDown()) {
    wakeDisplay();
  }

  processDisplaySleep();

  if (!canDraw() || now - lastDisplayUpdate < DISPLAY_INTERVAL_MS) {
    return;
  }

  lastDisplayUpdate = now;

  switch (screen) {
  case Screen::Main: {
    unsigned long held = buttonHeldMs();

    if (held >= TAP_MAX_MS && !holdCancelled && !holdHandled) {
      drawHoldScreen(held);
    } else {
      drawMainScreen();
    }
    break;
  }
  case Screen::Menu:
    drawMenuScreen();
    break;
  case Screen::Status:
    drawStatusScreen();
    break;
  case Screen::SelfTest:
    drawSelfTestScreen();
    break;
  case Screen::Command:
    drawCommandScreen();
    break;
  case Screen::KnobTest:
    drawKnobTestScreen();
    break;
  case Screen::I2CScan:
    drawI2CScanScreen();
    break;
  case Screen::ScreenTest:
    drawScreenTestScreen();
    break;
  }
}

// ============================================================
// BOOT SCREEN
// ============================================================

void showBootScreen() {
  if (!displayReady || !displayPresent) {
    return;
  }

  display.clearDisplay();

  drawCentered(FIRMWARE_NAME, TITLE_Y, 1);
  drawCentered("v" FIRMWARE_VERSION, CONTENT_Y, 1);

  bool warn = false;

  // Brownout usually means a weak USB cable/charger: the Wi-Fi
  // radio's current spike at connect drags the supply down.
  if (lastResetWasPowerProblem()) {
    drawLine(2, "Last reset: brownout");
    drawLine(3, "Check USB cable/power");
    warn = true;
  } else if (esp_reset_reason() == ESP_RST_PANIC ||
             esp_reset_reason() == ESP_RST_TASK_WDT ||
             esp_reset_reason() == ESP_RST_INT_WDT) {
    char line[24];
    snprintf(line, sizeof(line), "Last reset: %s", resetReasonName());
    drawLine(2, line);
    warn = true;
  } else if (lastRestartNote[0] != '\0') {
    drawLine(2, "Restarted:");
    drawLine(3, lastRestartNote);
    warn = true;
  } else {
    drawCentered("Starting...", CONTENT_Y + 2 * LINE_HEIGHT, 1);
  }

  drawLine(4, "Hold 2s: settings");

  display.display();

  delay(warn ? 2500 : 800);
}

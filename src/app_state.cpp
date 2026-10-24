#include "app_state.h"

#include <Preferences.h>
#include <esp_system.h>

#include "config.h"

SharedState shared;

portMUX_TYPE stateMux = portMUX_INITIALIZER_UNLOCKED;

DeviceSettings settings;

uint32_t bootCount = 0;
char lastRestartNote[32] = "";

static const char *PREFS_NAMESPACE = "huectl";

// ============================================================
// HEALTH TEXT
// ============================================================

static const HealthInfo HEALTH_TABLE[] = {
    // Booting
    {"...", "Starting up", "Starting up."},
    // ConfigMissing
    {"CFG", "Settings missing",
     "Wi-Fi or Hue values are empty. Fill in .env (see README) and upload "
     "again."},
    // WifiConnecting
    {"WIFI", "Joining Wi-Fi...",
     "Waiting for the router. If this lasts, check the 2.4GHz network is "
     "on."},
    // WifiNoNetwork
    {"WIFI", "Wi-Fi not found",
     "Network name not seen. ESP32 is 2.4GHz only. Check WIFI_SSID and "
     "range."},
    // WifiAuthFailed
    {"WIFI", "Wi-Fi rejected login",
     "Check WIFI_PASSWORD. Also caused by weak signal or WPA3-only "
     "routers."},
    // WifiLost
    {"WIFI", "Wi-Fi dropped",
     "Reconnecting on its own. Router restart or weak signal is likely."},
    // WifiNoIp
    {"DHCP", "No IP from router",
     "Joined Wi-Fi but got no address. Restart the router or check DHCP."},
    // BridgeUnreachable
    {"HUE", "Bridge not reachable",
     "Bridge off or its IP changed. Run Test & fix. Reserve its IP in "
     "the router."},
    // HueUnauthorized
    {"AUTH", "Hue key rejected",
     "HUE_USERNAME is not valid on this bridge. Make a new one (README)."},
    // HueGroupMissing
    {"ROOM", "Room not found",
     "HUE_GROUP_ID does not exist on the bridge. Check the ID (README)."},
    // HueBusy
    {"BUSY", "Bridge busy",
     "Bridge is rate limiting or overloaded. Slowing down and retrying."},
    // HueBadReply
    {"HUE?", "Odd bridge reply",
     "Unexpected answer. See Status > Bridge for the code and message."},
    // LowMemory
    {"MEM", "Low memory",
     "Not enough RAM for HTTPS. Restarts itself if it persists."},
    // Ok
    {"OK", "Connected", "Everything is working."},
};

static_assert(sizeof(HEALTH_TABLE) / sizeof(HEALTH_TABLE[0]) ==
                  static_cast<size_t>(Health::Ok) + 1,
              "HEALTH_TABLE must match the Health enum");

const HealthInfo &describeHealth(Health health) {
  return HEALTH_TABLE[static_cast<uint8_t>(health)];
}

bool isHealthOk(Health health) { return health == Health::Ok; }

const char *wifiReasonName(uint8_t reason) {
  switch (reason) {
  case 0:
    return "none";
  case 2:
    return "auth expired";
  case 3:
    return "AP left";
  case 4:
    return "assoc expired";
  case 6:
  case 7:
    return "not authed";
  case 8:
    return "left network";
  case 15:
    return "handshake t/o";
  case 23:
    return "802.1X failed";
  case 200:
    return "beacon lost";
  case 201:
    return "no AP found";
  case 202:
    return "auth failed";
  case 203:
    return "assoc failed";
  case 204:
    return "handshake t/o";
  case 205:
    return "connect failed";
  default:
    return "other";
  }
}

// ============================================================
// SHARED STATE
// ============================================================

void initSharedState() {
  StateLock lock;

  memset(&shared, 0, sizeof(shared));

  shared.brightness = 128;
  shared.lastSentBrightness = 128;
  shared.health = Health::Booting;
  shared.lastHueErrorType = 0;
}

SharedState snapshotState() {
  SharedState copy;

  StateLock lock;

  copy = shared;

  return copy;
}

void setCommandStatus(const char *text) {
  StateLock lock;

  strlcpy(shared.commandStatus, text, sizeof(shared.commandStatus));
}

// ============================================================
// NVS SETTINGS
// ============================================================

void loadSettings() {
  Preferences prefs;

  settings.sleepIndex = 1;
  settings.contrastIndex = 2;
  settings.flipped = false;
  settings.bridgeIpOverride[0] = '\0';

  if (!prefs.begin(PREFS_NAMESPACE, false)) {
    Serial.println("NVS unavailable; using defaults");
    return;
  }

  settings.sleepIndex = prefs.getUChar("sleep", settings.sleepIndex);
  settings.contrastIndex = prefs.getUChar("contrast", settings.contrastIndex);
  settings.flipped = prefs.getBool("flip", settings.flipped);

  prefs.getString("bridgeIp", settings.bridgeIpOverride,
                  sizeof(settings.bridgeIpOverride));

  bootCount = prefs.getUInt("boots", 0) + 1;
  prefs.putUInt("boots", bootCount);

  prefs.getString("restartNote", lastRestartNote, sizeof(lastRestartNote));
  if (lastRestartNote[0] != '\0') {
    prefs.remove("restartNote");
  }

  prefs.end();
}

void saveDisplaySettings() {
  Preferences prefs;

  if (!prefs.begin(PREFS_NAMESPACE, false)) {
    return;
  }

  prefs.putUChar("sleep", settings.sleepIndex);
  prefs.putUChar("contrast", settings.contrastIndex);
  prefs.putBool("flip", settings.flipped);

  prefs.end();
}

void saveBridgeIpOverride(const char *ip) {
  Preferences prefs;

  if (!prefs.begin(PREFS_NAMESPACE, false)) {
    return;
  }

  prefs.putString("bridgeIp", ip);

  prefs.end();
}

void eraseAllSettings() {
  Preferences prefs;

  if (!prefs.begin(PREFS_NAMESPACE, false)) {
    return;
  }

  prefs.clear();

  prefs.end();
}

void recordRestartNote(const char *note) {
  Preferences prefs;

  if (!prefs.begin(PREFS_NAMESPACE, false)) {
    return;
  }

  prefs.putString("restartNote", note);

  prefs.end();
}

// ============================================================
// RESET REASON
// ============================================================

const char *resetReasonName() {
  switch (esp_reset_reason()) {
  case ESP_RST_POWERON:
    return "Power on";
  case ESP_RST_SW:
    return "Software";
  case ESP_RST_PANIC:
    return "Crash";
  case ESP_RST_INT_WDT:
  case ESP_RST_TASK_WDT:
  case ESP_RST_WDT:
    return "Watchdog";
  case ESP_RST_BROWNOUT:
    return "Brownout";
  case ESP_RST_DEEPSLEEP:
    return "Deep sleep";
  case ESP_RST_EXT:
    return "Reset pin";
  default:
    return "Unknown";
  }
}

bool lastResetWasPowerProblem() {
  return esp_reset_reason() == ESP_RST_BROWNOUT;
}

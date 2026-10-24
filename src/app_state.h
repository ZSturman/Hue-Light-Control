#pragma once

#include <Arduino.h>

// ============================================================
// HEALTH
// ============================================================
//
// One value that says exactly what is wrong (or that all is
// well). Drives the header tag, footer message and Status page.

enum class Health : uint8_t {
  Booting,
  ConfigMissing,
  WifiConnecting,
  WifiNoNetwork,
  WifiAuthFailed,
  WifiLost,
  WifiNoIp,
  BridgeUnreachable,
  HueUnauthorized,
  HueGroupMissing,
  HueBusy,
  HueBadReply,
  LowMemory,
  Ok,
};

struct HealthInfo {
  const char *tag;     // <= 4 chars, header
  const char *message; // <= 21 chars, footer
  const char *hint;    // longer fix-it text, Status page
};

const HealthInfo &describeHealth(Health health);

bool isHealthOk(Health health);

const char *wifiReasonName(uint8_t reason);

// ============================================================
// NETWORK COMMANDS (UI -> network task)
// ============================================================

enum class NetCommand : uint8_t {
  None,
  ReconnectWiFi,
  FindBridge,
  SelfTest,
  FlashLights,
};

// ============================================================
// SELF-TEST
// ============================================================

enum class TestStatus : uint8_t { Pending, Running, Pass, Warn, Fail, Skip };

enum TestStepId : uint8_t {
  TEST_CONFIG,
  TEST_WIFI,   // includes signal strength
  TEST_BRIDGE,
  TEST_HUE,    // API key + room ID
  TEST_MEMORY,
  TEST_STEP_COUNT,
};

struct TestStep {
  TestStatus status;
  char detail[16];
};

// ============================================================
// SHARED STATE
// ============================================================
//
// The UI loop (core 1) and the network task (core 0) both use
// this. Always access it while holding StateLock, and keep the
// locked section tiny (no Serial, no delays, no I/O).

struct SharedState {
  // Light
  bool roomOn;
  bool roomStateKnown;
  int brightness;
  int lastSentBrightness;
  bool brightnessDirty;

  uint32_t requestedToggleGeneration;
  uint32_t processedToggleGeneration;

  unsigned long lastUserAction;

  // Health
  Health health;
  int lastHttpCode;
  int lastHueErrorType;
  char lastHueError[40];
  unsigned long lastHueSuccessAt;
  uint32_t hueFailureStreak;
  uint32_t totalErrors;

  // Wi-Fi
  bool wifiConnected;
  uint8_t wifiDisconnectReason;
  int8_t rssi;
  uint8_t wifiChannel;
  uint32_t localIp;
  uint32_t wifiAttempts;
  unsigned long wifiConnectedSince;

  // Bridge
  char bridgeIp[16];
  bool bridgeIpDiscovered;

  // Commands
  NetCommand pendingCommand;
  bool commandBusy;
  char commandStatus[22];

  // Self-test
  TestStep testSteps[TEST_STEP_COUNT];
  bool selfTestRunning;

  // Watchdog / UI flags
  unsigned long networkHeartbeat;
  uint32_t networkStackFree;
  bool settingsOpen;
};

extern SharedState shared;

extern portMUX_TYPE stateMux;

struct StateLock {
  StateLock() { portENTER_CRITICAL(&stateMux); }
  ~StateLock() { portEXIT_CRITICAL(&stateMux); }
};

// Copy everything at once for drawing.
SharedState snapshotState();

void initSharedState();

void setCommandStatus(const char *text);

// ============================================================
// PERSISTED SETTINGS (NVS)
// ============================================================

struct DeviceSettings {
  uint8_t sleepIndex;
  uint8_t contrastIndex;
  bool flipped;
  char bridgeIpOverride[16];
};

extern DeviceSettings settings;

extern uint32_t bootCount;
extern char lastRestartNote[32];

void loadSettings();
void saveDisplaySettings();
void saveBridgeIpOverride(const char *ip);

// Wipes NVS. Caller should reboot.
void eraseAllSettings();

// Saved before a deliberate reboot, shown after boot.
void recordRestartNote(const char *note);

const char *resetReasonName();
bool lastResetWasPowerProblem();

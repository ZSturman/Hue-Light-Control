#include "hue_network.h"

#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include "config.h"

// ============================================================
// LOCAL STATE (network task only)
// ============================================================

// Pseudo HTTP code: skipped because TLS needs more heap.
static const int HUE_ERR_LOW_MEMORY = -100;

static TaskHandle_t networkTaskHandle = nullptr;

// Bridge IP actually in use (.env, or found by discovery).
static char bridgeIp[16] = "";

struct NetworkContext {
  unsigned long lastBrightnessSend = 0;
  bool lastBrightnessSendFailed = false;

  unsigned long lastHuePoll = 0;
  bool pollNow = true;

  unsigned long nextWiFiAttemptAt = 0;
  int wifiFailures = 0;
  bool wasConnected = false;
  unsigned long wifiDownSince = 0;

  bool mdnsTried = false;
  unsigned long lastMdnsDiscovery = 0;
  bool cloudTried = false;
  unsigned long lastCloudDiscovery = 0;

  int toggleAttempts = 0;

  unsigned long lowMemorySince = 0;

  unsigned long lastLinkPublish = 0;
  unsigned long lastStackCheck = 0;

  unsigned long lastWarmCheck = 0;
  unsigned long lastWarmReconnect = 0;
};

static NetworkContext ctx;

// ============================================================
// WI-FI EVENTS
// ============================================================
//
// Runs in the Wi-Fi event task. Only records what happened so
// the network task can report the real reason for a failure.

static volatile uint8_t lastDisconnectReason = 0;
static volatile uint8_t disconnectCount = 0;
static volatile bool associated = false;

static void onWiFiEvent(arduino_event_id_t event, arduino_event_info_t info) {
  switch (event) {
  case ARDUINO_EVENT_WIFI_STA_CONNECTED:
    associated = true;
    break;

  case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
    associated = false;
    lastDisconnectReason = info.wifi_sta_disconnected.reason;
    disconnectCount++;
    break;

  default:
    break;
  }
}

// ============================================================
// SMALL HELPERS
// ============================================================

static bool timeReached(unsigned long now, unsigned long target) {
  return static_cast<long>(now - target) >= 0;
}

static void heartbeat() {
  StateLock lock;

  shared.networkHeartbeat = millis();
}

static void waitMs(unsigned long ms) {
  heartbeat();
  vTaskDelay(pdMS_TO_TICKS(ms));
}

static bool isValidIp(const char *text) {
  IPAddress ip;

  return text != nullptr && text[0] != '\0' && ip.fromString(text);
}

static bool configComplete() {
  return strlen(WIFI_SSID) > 0 && strlen(HUE_USERNAME) > 0 &&
         strlen(HUE_GROUP_ID) > 0;
}

static String groupPath() { return String("/groups/") + HUE_GROUP_ID; }

static String actionPath() { return groupPath() + "/action"; }

static void setHealth(Health health) {
  Health previous;

  {
    StateLock lock;

    previous = shared.health;

    if (previous != health && !isHealthOk(health)) {
      shared.totalErrors++;
    }

    shared.health = health;
  }

  if (previous != health) {
    Serial.print("Health: ");
    Serial.print(describeHealth(previous).message);
    Serial.print(" -> ");
    Serial.println(describeHealth(health).message);
  }
}

static Health currentHealth() {
  StateLock lock;

  return shared.health;
}

static void publishBridgeIp(bool discovered) {
  StateLock lock;

  strlcpy(shared.bridgeIp, bridgeIp, sizeof(shared.bridgeIp));
  shared.bridgeIpDiscovered = discovered;
}

static void publishLinkInfo() {
  bool connected = WiFi.status() == WL_CONNECTED;

  int8_t rssi = connected ? WiFi.RSSI() : 0;
  uint8_t channel = connected ? WiFi.channel() : 0;
  uint32_t ip = connected ? static_cast<uint32_t>(WiFi.localIP()) : 0;

  StateLock lock;

  shared.wifiConnected = connected;
  shared.rssi = rssi;
  shared.wifiChannel = channel;
  shared.localIp = ip;
  shared.wifiDisconnectReason = lastDisconnectReason;
}

// ============================================================
// MINIMAL JSON HELPERS
// ============================================================
//
// The Hue replies we read are small and flat enough that a
// full JSON library isn't worth the RAM.

static int findJsonValue(const String &body, const char *key, int from) {
  String needle = String("\"") + key + "\"";

  int keyPosition = body.indexOf(needle, from);

  if (keyPosition < 0) {
    return -1;
  }

  int colon = body.indexOf(':', keyPosition + needle.length());

  if (colon < 0) {
    return -1;
  }

  int value = colon + 1;

  while (value < static_cast<int>(body.length()) &&
         isspace(static_cast<unsigned char>(body[value]))) {
    value++;
  }

  return value;
}

static bool jsonBool(const String &body, const char *key, int from,
                     bool &out) {
  int value = findJsonValue(body, key, from);

  if (value < 0) {
    return false;
  }

  if (body.startsWith("true", value)) {
    out = true;
    return true;
  }

  if (body.startsWith("false", value)) {
    out = false;
    return true;
  }

  return false;
}

static bool jsonInt(const String &body, const char *key, int from, int &out) {
  int value = findJsonValue(body, key, from);

  if (value < 0) {
    return false;
  }

  char first = body[value];

  if (first != '-' && !isdigit(static_cast<unsigned char>(first))) {
    return false;
  }

  out = body.substring(value, value + 8).toInt();

  return true;
}

static bool jsonString(const String &body, const char *key, int from,
                       char *out, size_t outSize, int *endPosition = nullptr) {
  int value = findJsonValue(body, key, from);

  if (value < 0 || body[value] != '"') {
    return false;
  }

  int end = body.indexOf('"', value + 1);

  if (end < 0) {
    return false;
  }

  strlcpy(out, body.substring(value + 1, end).c_str(), outSize);

  if (endPosition != nullptr) {
    *endPosition = end;
  }

  return true;
}

// ============================================================
// HUE REQUESTS
// ============================================================

struct HueResult {
  int httpCode = 0;
  int hueErrorType = 0;
  char hueError[40] = "";
  String body;

  bool transportOk() const { return httpCode > 0; }

  bool ok() const {
    // 201 = "device is off": harmless for group actions.
    return httpCode >= 200 && httpCode < 300 &&
           (hueErrorType == 0 || hueErrorType == 201);
  }
};

static void parseHueError(HueResult &result) {
  // Hue v1 reports most errors as HTTP 200 with a body like
  // [{"error":{"type":1,"address":"/","description":"unauthorized user"}}]
  int errorPosition = result.body.indexOf("\"error\"");

  if (errorPosition < 0) {
    return;
  }

  int type = 0;

  if (jsonInt(result.body, "type", errorPosition, type)) {
    result.hueErrorType = type;
  } else {
    result.hueErrorType = -1;
  }

  jsonString(result.body, "description", errorPosition, result.hueError,
             sizeof(result.hueError));
}

static void configureSecureClient(WiFiClientSecure &client) {
  // The bridge uses a self-signed certificate.
  client.setInsecure();

  // Default is 120 s, which would freeze this task if the bridge
  // accepts TCP but stalls TLS.
  client.setHandshakeTimeout(5);
}

static void logRequest(const char *method, const String &url,
                       const HueResult &result, unsigned long elapsed,
                       bool reused) {
  // Log the path without the API key.
  String path = url.substring(url.indexOf('/', 8));
  path.replace(HUE_USERNAME, "<key>");

  Serial.print(method);
  Serial.print(' ');
  Serial.print(path);
  Serial.print(" -> ");
  Serial.print(result.httpCode);

  if (result.httpCode <= 0) {
    Serial.print(" (");
    Serial.print(HTTPClient::errorToString(result.httpCode));
    Serial.print(")");
  }

  if (result.hueErrorType != 0) {
    Serial.print(" hue error ");
    Serial.print(result.hueErrorType);
    Serial.print(": ");
    Serial.print(result.hueError);
  }

  Serial.print(" / ");
  Serial.print(elapsed);
  Serial.println(reused ? " ms (reused)" : " ms (new)");
}

// Sends one request on the given client. With reuse enabled the
// TLS connection stays open afterwards.
static HueResult sendRequest(HTTPClient &http, WiFiClientSecure &client,
                             const String &url, const char *method,
                             const String &body) {
  HueResult result;

  bool reused = client.connected();

  // A new TLS session needs ~40 KB; a reused one needs nothing.
  if (!reused && ESP.getMaxAllocHeap() < MIN_TLS_HEAP_BYTES) {
    result.httpCode = HUE_ERR_LOW_MEMORY;
    Serial.println("HTTPS skipped: low memory");
    return result;
  }

  unsigned long started = millis();

  if (!http.begin(client, url)) {
    result.httpCode = HTTPC_ERROR_CONNECTION_REFUSED;
    return result;
  }

  if (body.length() > 0) {
    http.addHeader("Content-Type", "application/json");
  }

  result.httpCode = http.sendRequest(method, body);

  if (result.httpCode > 0) {
    // Must read the whole reply or the connection can't be reused.
    result.body = http.getString();
  }

  http.end();

  parseHueError(result);

  logRequest(method, url, result, millis() - started, reused);

  heartbeat();

  return result;
}

// One-off request (discovery, checking a candidate bridge).
static HueResult httpsRequest(const String &url, const char *method,
                              const String &body) {
  WiFiClientSecure client;
  configureSecureClient(client);

  HTTPClient http;
  http.setConnectTimeout(HTTP_CONNECT_TIMEOUT_MS);
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.setReuse(false);

  return sendRequest(http, client, url, method, body);
}

static HueResult hueRequestAt(const char *ip, const char *method,
                              const String &path, const String &body = "") {
  String url = String("https://") + ip + "/api/" + HUE_USERNAME + path;

  return httpsRequest(url, method, body);
}

// ============================================================
// PERSISTENT BRIDGE CONNECTION
// ============================================================
//
// The TLS handshake costs ~2 s on an ESP32; the request itself
// takes well under 100 ms. Keeping one connection open to the
// bridge (like the phone app does) removes the handshake from
// every command after the first.

static WiFiClientSecure bridgeClient;
static HTTPClient bridgeHttp;
static bool bridgeHttpConfigured = false;

// Bridge IP the open connection belongs to.
static char connectedBridgeIp[16] = "";

// Whether the bridge kept the connection open after the last
// reply. If it never does, don't try to keep it warm.
static bool bridgeKeepsAlive = false;

static unsigned long lastBridgeRequestAt = 0;

static void closeBridgeConnection() {
  if (bridgeClient.connected()) {
    Serial.println("Bridge connection closed");
  }

  bridgeClient.stop();
  connectedBridgeIp[0] = '\0';
}

static HueResult hueRequest(const char *method, const String &path,
                            const String &body = "") {
  if (!isValidIp(bridgeIp)) {
    HueResult result;
    result.httpCode = HTTPC_ERROR_CONNECTION_REFUSED;
    return result;
  }

  if (!bridgeHttpConfigured) {
    configureSecureClient(bridgeClient);

    bridgeHttp.setConnectTimeout(HTTP_CONNECT_TIMEOUT_MS);
    bridgeHttp.setTimeout(HTTP_TIMEOUT_MS);
    bridgeHttp.setReuse(true);

    bridgeHttpConfigured = true;
  }

  // Bridge moved: the old connection is useless.
  if (strcmp(connectedBridgeIp, bridgeIp) != 0) {
    closeBridgeConnection();
  }

  String url = String("https://") + bridgeIp + "/api/" + HUE_USERNAME + path;

  bool wasOpen = bridgeClient.connected();

  HueResult result = sendRequest(bridgeHttp, bridgeClient, url, method, body);

  // The bridge may have dropped an idle connection without us
  // noticing. Retry once on a fresh one before reporting failure.
  if (!result.transportOk() && wasOpen) {
    Serial.println("Reused connection failed; reconnecting");

    closeBridgeConnection();

    result = sendRequest(bridgeHttp, bridgeClient, url, method, body);
  }

  if (result.transportOk()) {
    strlcpy(connectedBridgeIp, bridgeIp, sizeof(connectedBridgeIp));
    bridgeKeepsAlive = bridgeClient.connected();
  } else {
    closeBridgeConnection();
  }

  lastBridgeRequestAt = millis();

  return result;
}

static Health healthForResult(const HueResult &result) {
  if (result.httpCode == HUE_ERR_LOW_MEMORY) {
    return Health::LowMemory;
  }

  if (result.httpCode <= 0) {
    return Health::BridgeUnreachable;
  }

  switch (result.httpCode) {
  case 401:
  case 403:
    return Health::HueUnauthorized;
  case 404:
    return Health::HueGroupMissing;
  case 429:
  case 503:
    return Health::HueBusy;
  default:
    break;
  }

  if (result.httpCode < 200 || result.httpCode >= 300) {
    return Health::HueBadReply;
  }

  switch (result.hueErrorType) {
  case 0:
  case 201:
    return Health::Ok;
  case 1:
    return Health::HueUnauthorized;
  case 3:
    return Health::HueGroupMissing;
  case 901:
    return Health::HueBusy;
  default:
    return Health::HueBadReply;
  }
}

// Updates health/diagnostics from a Hue reply. Returns ok().
static bool recordHueResult(const HueResult &result) {
  Health health = healthForResult(result);

  bool ok = result.ok();

  {
    StateLock lock;

    shared.lastHttpCode = result.httpCode;
    shared.lastHueErrorType = result.hueErrorType;
    strlcpy(shared.lastHueError, result.hueError,
            sizeof(shared.lastHueError));

    if (ok) {
      shared.lastHueSuccessAt = millis();
      shared.hueFailureStreak = 0;
    } else {
      shared.hueFailureStreak++;
    }
  }

  setHealth(health);

  return ok;
}

static uint32_t hueFailureStreak() {
  StateLock lock;

  return shared.hueFailureStreak;
}

// ============================================================
// READ HUE GROUP
// ============================================================

static bool pollGroupState() {
  HueResult result = hueRequest("GET", groupPath());

  if (!recordHueResult(result)) {
    return false;
  }

  bool parsedOn = false;
  bool hasOn = false;

  int statePosition = result.body.indexOf("\"state\"");

  if (statePosition >= 0) {
    hasOn = jsonBool(result.body, "any_on", statePosition, parsedOn);
  }

  int parsedBrightness = 0;
  bool hasBrightness = false;

  int actionPosition = result.body.indexOf("\"action\"");

  if (actionPosition >= 0) {
    hasBrightness =
        jsonInt(result.body, "bri", actionPosition, parsedBrightness);
  }

  unsigned long now = millis();

  StateLock lock;

  if (hasOn) {
    shared.roomOn = parsedOn;
    shared.roomStateKnown = true;
  }

  // Follow changes made in the Hue app, unless the knob is
  // being used right now.
  if (hasBrightness && !shared.brightnessDirty &&
      now - shared.lastUserAction >= USER_ACTION_POLL_GUARD_MS) {

    shared.brightness = constrain(parsedBrightness, 1, 254);
    shared.lastSentBrightness = shared.brightness;
  }

  return true;
}

// ============================================================
// BRIDGE DISCOVERY
// ============================================================
//
// The bridge usually gets its IP from DHCP. After a power cut
// it can come back on a different address, which used to leave
// this device stuck on "ERR" forever.

enum class DiscoveryResult : uint8_t { Moved, SameIp, Rejected, NotFound };

static const int MAX_CANDIDATES = 6;

static void addCandidate(char candidates[][16], int &count, const char *ip) {
  if (!isValidIp(ip) || count >= MAX_CANDIDATES) {
    return;
  }

  for (int i = 0; i < count; i++) {
    if (strcmp(candidates[i], ip) == 0) {
      return;
    }
  }

  strlcpy(candidates[count], ip, 16);
  count++;
}

static void discoverViaMdns(char candidates[][16], int &count) {
  setCommandStatus("Searching (mDNS)...");

  if (!MDNS.begin(DEVICE_HOSTNAME)) {
    Serial.println("mDNS start failed");
    return;
  }

  int found = MDNS.queryService("hue", "tcp");

  for (int i = 0; i < found; i++) {
    addCandidate(candidates, count, MDNS.IP(i).toString().c_str());
  }

  MDNS.end();

  Serial.print("mDNS bridges: ");
  Serial.println(found);

  heartbeat();
}

static void discoverViaCloud(char candidates[][16], int &count) {
  setCommandStatus("Searching (cloud)...");

  HueResult result = httpsRequest("https://discovery.meethue.com/", "GET", "");

  if (!result.transportOk() || result.httpCode != 200) {
    Serial.println("Cloud discovery failed");
    return;
  }

  int position = 0;
  char ip[16];

  while (count < MAX_CANDIDATES &&
         jsonString(result.body, "internalipaddress", position, ip,
                    sizeof(ip), &position)) {
    addCandidate(candidates, count, ip);
  }
}

static DiscoveryResult discoverBridge(bool userRequested) {
  unsigned long now = millis();

  char candidates[MAX_CANDIDATES][16];
  int count = 0;

  bool mdnsAllowed = userRequested || !ctx.mdnsTried ||
                     now - ctx.lastMdnsDiscovery >= MDNS_DISCOVERY_INTERVAL_MS;

  if (mdnsAllowed) {
    ctx.mdnsTried = true;
    ctx.lastMdnsDiscovery = now;

    discoverViaMdns(candidates, count);
  }

  bool cloudAllowed =
      !ctx.cloudTried ||
      now - ctx.lastCloudDiscovery >=
          (userRequested ? 60000UL : CLOUD_DISCOVERY_INTERVAL_MS);

  if (count == 0 && cloudAllowed) {
    ctx.cloudTried = true;
    ctx.lastCloudDiscovery = now;

    discoverViaCloud(candidates, count);
  }

  if (count == 0) {
    return DiscoveryResult::NotFound;
  }

  bool sawRejection = false;

  for (int i = 0; i < count; i++) {
    setCommandStatus("Checking bridge...");

    HueResult result = hueRequestAt(candidates[i], "GET", groupPath());

    if (!result.transportOk()) {
      continue;
    }

    // A bridge that doesn't know our username is someone else's
    // (or was reset). Don't switch to it.
    if (result.hueErrorType == 1 || result.httpCode == 401 ||
        result.httpCode == 403) {
      sawRejection = true;
      continue;
    }

    if (strcmp(candidates[i], bridgeIp) == 0) {
      recordHueResult(result);
      return DiscoveryResult::SameIp;
    }

    Serial.print("Bridge moved: ");
    Serial.print(bridgeIp);
    Serial.print(" -> ");
    Serial.println(candidates[i]);

    strlcpy(bridgeIp, candidates[i], sizeof(bridgeIp));

    // Only remember it if it differs from .env.
    bool matchesEnv = strcmp(bridgeIp, HUE_BRIDGE_IP) == 0;

    saveBridgeIpOverride(matchesEnv ? "" : bridgeIp);

    publishBridgeIp(!matchesEnv);

    recordHueResult(result);

    ctx.pollNow = true;

    return DiscoveryResult::Moved;
  }

  return sawRejection ? DiscoveryResult::Rejected : DiscoveryResult::NotFound;
}

// ============================================================
// WI-FI CONNECTION
// ============================================================

static Health healthForWiFiFailure(uint8_t reason, bool gotAssociated) {
  if (gotAssociated) {
    return Health::WifiNoIp;
  }

  switch (reason) {
  case 201:
    return Health::WifiNoNetwork;
  case 2:
  case 15:
  case 23:
  case 202:
  case 204:
    return Health::WifiAuthFailed;
  case 0:
    return Health::WifiConnecting;
  default:
    return Health::WifiLost;
  }
}

// Blocks this task only (never the UI) for up to
// WIFI_CONNECT_TIMEOUT_MS.
static bool connectWiFi(bool hardReset) {
  if (WiFi.status() == WL_CONNECTED) {
    return true;
  }

  if (hardReset) {
    // Fully power-cycle the radio. Fixes the driver getting
    // wedged after repeated failures.
    Serial.println("Wi-Fi: radio reset");

    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    waitMs(500);
  } else {
    // Calling begin() while a previous attempt is still active
    // is the classic "connects every other boot" ESP32 bug.
    // Always stop the old attempt first.
    WiFi.disconnect(false, false);
    waitMs(100);
  }

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);

  lastDisconnectReason = 0;
  disconnectCount = 0;
  associated = false;

  Health health = currentHealth();

  // Keep a specific error on screen while retrying rather than
  // flickering back to "Joining".
  if (health != Health::WifiNoNetwork && health != Health::WifiAuthFailed &&
      health != Health::WifiNoIp) {
    setHealth(Health::WifiConnecting);
  }

  {
    StateLock lock;

    shared.wifiAttempts++;
  }

  Serial.print("Wi-Fi: connecting to ");
  Serial.println(WIFI_SSID);

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long started = millis();

  while (WiFi.status() != WL_CONNECTED &&
         millis() - started < WIFI_CONNECT_TIMEOUT_MS) {

    // The core retries the first connect after boot once by
    // itself (its fix for the "every other boot" bug). After a
    // second failure with auto-reconnect off, nothing else will
    // happen, so stop waiting.
    if (disconnectCount >= 2 && !associated) {
      break;
    }

    waitMs(50);
  }

  if (WiFi.status() == WL_CONNECTED) {
    publishLinkInfo();
    return true;
  }

  uint8_t reason = lastDisconnectReason;
  bool gotAssociated = associated;

  Serial.print("Wi-Fi failed: reason ");
  Serial.print(reason);
  Serial.print(" (");
  Serial.print(wifiReasonName(reason));
  Serial.println(")");

  // Stop the attempt so the next begin() starts clean.
  WiFi.disconnect(false, false);

  setHealth(healthForWiFiFailure(reason, gotAssociated));

  publishLinkInfo();

  return false;
}

static void onWiFiConnected() {
  ctx.wifiFailures = 0;
  ctx.wasConnected = true;
  ctx.pollNow = true;

  {
    StateLock lock;

    shared.wifiConnectedSince = millis();
  }

  Serial.print("Wi-Fi connected: ");
  Serial.print(WiFi.localIP());
  Serial.print("  RSSI ");
  Serial.println(WiFi.RSSI());

  if (!isValidIp(bridgeIp)) {
    setHealth(Health::BridgeUnreachable);
  } else if (!isHealthOk(currentHealth())) {
    // Wi-Fi is fine; Hue state is unknown until the next poll.
    setHealth(Health::WifiConnecting);
  }
}

static unsigned long wifiBackoffMs(int failures) {
  unsigned long backoff = WIFI_RETRY_MIN_MS << min(failures, 4);

  return min(backoff, WIFI_RETRY_MAX_MS);
}

// ============================================================
// SELF-TEST
// ============================================================

static void setStep(TestStepId id, TestStatus status, const char *detail) {
  StateLock lock;

  shared.testSteps[id].status = status;
  strlcpy(shared.testSteps[id].detail, detail,
          sizeof(shared.testSteps[id].detail));
}

static void runSelfTest() {
  {
    StateLock lock;

    shared.selfTestRunning = true;

    for (int i = 0; i < TEST_STEP_COUNT; i++) {
      shared.testSteps[i].status = TestStatus::Pending;
      shared.testSteps[i].detail[0] = '\0';
    }
  }

  setCommandStatus("Self-test running");

  char detail[24];

  // ---------- Config ----------

  setStep(TEST_CONFIG, TestStatus::Running, "");

  if (strlen(WIFI_SSID) == 0) {
    setStep(TEST_CONFIG, TestStatus::Fail, "No WIFI_SSID");
  } else if (strlen(HUE_USERNAME) == 0) {
    setStep(TEST_CONFIG, TestStatus::Fail, "No HUE_USERNAME");
  } else if (strlen(HUE_GROUP_ID) == 0) {
    setStep(TEST_CONFIG, TestStatus::Fail, "No HUE_GROUP_ID");
  } else if (!isValidIp(HUE_BRIDGE_IP)) {
    setStep(TEST_CONFIG, TestStatus::Warn, "Bad bridge IP");
  } else {
    setStep(TEST_CONFIG, TestStatus::Pass, ".env loaded");
  }

  // ---------- Wi-Fi ----------

  setStep(TEST_WIFI, TestStatus::Running, "");

  if (WiFi.status() != WL_CONNECTED && strlen(WIFI_SSID) > 0) {
    if (connectWiFi(false)) {
      onWiFiConnected();
    }
  }

  bool wifiUp = WiFi.status() == WL_CONNECTED;

  if (!wifiUp) {
    setStep(TEST_WIFI, TestStatus::Fail, wifiReasonName(lastDisconnectReason));
  } else {
    int rssi = WiFi.RSSI();

    if (rssi >= RSSI_GOOD_DBM) {
      snprintf(detail, sizeof(detail), "%d dBm good", rssi);
      setStep(TEST_WIFI, TestStatus::Pass, detail);
    } else if (rssi >= RSSI_WEAK_DBM) {
      snprintf(detail, sizeof(detail), "%d dBm fair", rssi);
      setStep(TEST_WIFI, TestStatus::Pass, detail);
    } else {
      snprintf(detail, sizeof(detail), "%d dBm weak", rssi);
      setStep(TEST_WIFI, TestStatus::Warn, detail);
    }
  }

  // ---------- Bridge reachable ----------

  bool bridgeUp = false;

  if (!wifiUp) {
    setStep(TEST_BRIDGE, TestStatus::Skip, "");
  } else {
    setStep(TEST_BRIDGE, TestStatus::Running, "");

    if (isValidIp(bridgeIp)) {
      WiFiClient tcp;
      IPAddress address;
      address.fromString(bridgeIp);

      bridgeUp = tcp.connect(address, 443, 2000);
      tcp.stop();
    }

    if (bridgeUp) {
      setStep(TEST_BRIDGE, TestStatus::Pass, bridgeIp);
    } else {
      // Maybe it moved. Look for it.
      DiscoveryResult found = discoverBridge(true);

      if (found == DiscoveryResult::Moved || found == DiscoveryResult::SameIp) {
        bridgeUp = true;
        snprintf(detail, sizeof(detail), "Now %s", bridgeIp);
        setStep(TEST_BRIDGE, TestStatus::Warn, detail);
      } else if (found == DiscoveryResult::Rejected) {
        setStep(TEST_BRIDGE, TestStatus::Fail, "Other bridge");
      } else {
        setStep(TEST_BRIDGE, TestStatus::Fail, "No reply");
      }
    }
  }

  // ---------- API key + room ----------

  if (!bridgeUp) {
    setStep(TEST_HUE, TestStatus::Skip, "");
  } else {
    setStep(TEST_HUE, TestStatus::Running, "");

    HueResult result = hueRequest("GET", groupPath());

    recordHueResult(result);

    Health health = healthForResult(result);

    if (!result.transportOk()) {
      setStep(TEST_HUE, TestStatus::Fail, "No HTTPS reply");
    } else if (health == Health::HueUnauthorized) {
      setStep(TEST_HUE, TestStatus::Fail, "Key rejected");
    } else if (health == Health::HueGroupMissing) {
      snprintf(detail, sizeof(detail), "Room %s missing", HUE_GROUP_ID);
      setStep(TEST_HUE, TestStatus::Fail, detail);
    } else if (result.ok()) {
      char name[24] = "";
      jsonString(result.body, "name", 0, name, sizeof(name));

      setStep(TEST_HUE, TestStatus::Pass, name[0] ? name : "Room found");

      ctx.pollNow = true;
    } else {
      snprintf(detail, sizeof(detail), "HTTP %d / %d", result.httpCode,
               result.hueErrorType);
      setStep(TEST_HUE, TestStatus::Warn, detail);
    }
  }

  // ---------- Memory ----------

  uint32_t largest = ESP.getMaxAllocHeap();

  snprintf(detail, sizeof(detail), "%uk free",
           static_cast<unsigned>(ESP.getFreeHeap() / 1024));

  setStep(TEST_MEMORY,
          largest >= MIN_TLS_HEAP_BYTES ? TestStatus::Pass : TestStatus::Fail,
          detail);

  {
    StateLock lock;

    shared.selfTestRunning = false;
  }

  setCommandStatus("Self-test done");
}

// ============================================================
// SETTINGS COMMANDS
// ============================================================

static void runPendingCommand() {
  NetCommand command;

  {
    StateLock lock;

    command = shared.pendingCommand;
    shared.pendingCommand = NetCommand::None;

    if (command != NetCommand::None) {
      shared.commandBusy = true;
    }
  }

  if (command == NetCommand::None) {
    return;
  }

  bool wifiUp = WiFi.status() == WL_CONNECTED;

  switch (command) {
  case NetCommand::ReconnectWiFi:
    setCommandStatus("Resetting Wi-Fi...");

    ctx.wasConnected = false;

    closeBridgeConnection();

    if (connectWiFi(true)) {
      onWiFiConnected();
      setCommandStatus("Wi-Fi connected");
    } else {
      ctx.wifiFailures++;
      ctx.nextWiFiAttemptAt = millis() + wifiBackoffMs(ctx.wifiFailures);
      setCommandStatus(describeHealth(currentHealth()).message);
    }
    break;

  case NetCommand::FindBridge:
    if (!wifiUp) {
      setCommandStatus("No Wi-Fi");
      break;
    }

    switch (discoverBridge(true)) {
    case DiscoveryResult::Moved:
      setCommandStatus("Found new bridge IP");
      break;
    case DiscoveryResult::SameIp:
      setCommandStatus("Bridge OK, same IP");
      break;
    case DiscoveryResult::Rejected:
      setCommandStatus("Bridge rejects key");
      break;
    case DiscoveryResult::NotFound:
      setCommandStatus("No bridge found");
      break;
    }
    break;

  case NetCommand::FlashLights:
    if (!wifiUp) {
      setCommandStatus("No Wi-Fi");
      break;
    }

    setCommandStatus("Flashing...");

    if (recordHueResult(hueRequest("PUT", actionPath(),
                                   "{\"alert\":\"select\"}"))) {
      setCommandStatus("Lights flashed");
    } else {
      setCommandStatus(describeHealth(currentHealth()).message);
    }
    break;

  case NetCommand::SelfTest:
    runSelfTest();
    break;

  case NetCommand::None:
    break;
  }

  StateLock lock;

  shared.commandBusy = false;
}

// ============================================================
// POWER / BRIGHTNESS
// ============================================================

// Returns true if it did network work.
static bool processToggle() {
  uint32_t requested;
  uint32_t processed;

  {
    StateLock lock;

    requested = shared.requestedToggleGeneration;
    processed = shared.processedToggleGeneration;
  }

  if (requested == processed) {
    return false;
  }

  bool known;

  {
    StateLock lock;

    known = shared.roomStateKnown;
  }

  // Never toggle blind: find out whether it's on first.
  if (!known) {
    pollGroupState();
  }

  bool currentRoomState;
  int brightness;

  {
    StateLock lock;

    currentRoomState = shared.roomOn;
    brightness = shared.brightness;
  }

  bool requestedState = !currentRoomState;

  String body;

  if (requestedState) {
    body = "{\"on\":true,\"bri\":" + String(brightness) +
           ",\"transitiontime\":" + String(POWER_TRANSITION_TIME) + "}";
  } else {
    body = "{\"on\":false,\"transitiontime\":" +
           String(POWER_TRANSITION_TIME) + "}";
  }

  if (recordHueResult(hueRequest("PUT", actionPath(), body))) {
    ctx.toggleAttempts = 0;

    StateLock lock;

    shared.roomOn = requestedState;
    shared.roomStateKnown = true;
    shared.processedToggleGeneration++;

    if (requestedState) {
      shared.lastSentBrightness = brightness;

      // Only clear dirty if the desired brightness hasn't
      // changed while HTTPS was running.
      if (shared.brightness == brightness) {
        shared.brightnessDirty = false;
      }
    }

    return true;
  }

  ctx.toggleAttempts++;

  if (ctx.toggleAttempts >= TOGGLE_MAX_ATTEMPTS) {
    // Give up rather than flipping the lights unexpectedly
    // minutes later when the bridge comes back.
    Serial.println("Toggle dropped after retries");

    ctx.toggleAttempts = 0;

    StateLock lock;

    shared.processedToggleGeneration = shared.requestedToggleGeneration;

    return true;
  }

  waitMs(300 * ctx.toggleAttempts);

  return true;
}

static bool processBrightness(unsigned long now) {
  bool dirty;
  bool isOn;
  int brightness;

  {
    StateLock lock;

    dirty = shared.brightnessDirty;
    isOn = shared.roomOn;
    brightness = shared.brightness;
  }

  unsigned long interval = ctx.lastBrightnessSendFailed
                               ? HUE_POLL_INTERVAL_UNHEALTHY_MS / 2
                               : BRIGHTNESS_SEND_INTERVAL_MS;

  if (!dirty || !isOn || now - ctx.lastBrightnessSend < interval) {
    return false;
  }

  String body = "{\"bri\":" + String(brightness) + ",\"transitiontime\":" +
                String(BRIGHTNESS_TRANSITION_TIME) + "}";

  bool success = recordHueResult(hueRequest("PUT", actionPath(), body));

  ctx.lastBrightnessSend = millis();
  ctx.lastBrightnessSendFailed = !success;

  if (success) {
    StateLock lock;

    shared.lastSentBrightness = brightness;

    // If the user kept turning during the HTTP request, the
    // target changed and dirty stays true for the next cycle.
    if (shared.brightness == brightness) {
      shared.brightnessDirty = false;
    }
  }

  return true;
}

// ============================================================
// NETWORK TASK
// ============================================================
//
// HTTPS and Wi-Fi reconnects can block this task for seconds.
// They can never block the encoder, button, or OLED.

static void handleWiFiDown(unsigned long now) {
  if (ctx.wasConnected) {
    ctx.wasConnected = false;
    ctx.wifiDownSince = now;
    ctx.nextWiFiAttemptAt = now;

    Serial.print("Wi-Fi lost: reason ");
    Serial.print(lastDisconnectReason);
    Serial.print(" (");
    Serial.print(wifiReasonName(lastDisconnectReason));
    Serial.println(")");

    setHealth(Health::WifiLost);
    publishLinkInfo();

    closeBridgeConnection();
  }

  // Presses while offline can't be delivered; drop them so the
  // light doesn't flip on its own later.
  {
    StateLock lock;

    shared.processedToggleGeneration = shared.requestedToggleGeneration;
  }

  if (timeReached(now, ctx.nextWiFiAttemptAt)) {
    bool hardReset = ctx.wifiFailures > 0 &&
                     ctx.wifiFailures % WIFI_HARD_RESET_EVERY == 0;

    if (connectWiFi(hardReset)) {
      onWiFiConnected();
      return;
    }

    ctx.wifiFailures++;
    ctx.nextWiFiAttemptAt = millis() + wifiBackoffMs(ctx.wifiFailures);
  }

  bool settingsOpen;

  {
    StateLock lock;

    settingsOpen = shared.settingsOpen;
  }

  // Last resort: a full reboot clears any driver state we
  // can't reach. Never while the user is in Settings.
  if (!settingsOpen && millis() - ctx.wifiDownSince >= WIFI_REBOOT_AFTER_MS) {
    Serial.println("Wi-Fi down too long; rebooting");

    recordRestartNote("Wi-Fi down 20 min");

    delay(100);

    ESP.restart();
  }
}

static void checkLowMemory(unsigned long now) {
  if (currentHealth() != Health::LowMemory) {
    ctx.lowMemorySince = 0;
    return;
  }

  if (ctx.lowMemorySince == 0) {
    ctx.lowMemorySince = now;
    return;
  }

  if (now - ctx.lowMemorySince >= 120000) {
    Serial.println("Low memory too long; rebooting");

    recordRestartNote("Low memory");

    delay(100);

    ESP.restart();
  }
}

static void networkTask(void *parameter) {
  ctx.wifiDownSince = millis();

  if (!configComplete()) {
    setHealth(Health::ConfigMissing);
  }

  for (;;) {
    unsigned long now = millis();

    heartbeat();

    if (now - ctx.lastStackCheck >= 5000) {
      ctx.lastStackCheck = now;

      StateLock lock;

      shared.networkStackFree = uxTaskGetStackHighWaterMark(nullptr);
    }

    runPendingCommand();

    if (!configComplete()) {
      waitMs(100);
      continue;
    }

    // ========================================================
    // WI-FI RECOVERY
    // ========================================================

    if (WiFi.status() != WL_CONNECTED) {
      handleWiFiDown(now);
      waitMs(20);
      continue;
    }

    if (!ctx.wasConnected) {
      onWiFiConnected();
    }

    if (now - ctx.lastLinkPublish >= 1000) {
      ctx.lastLinkPublish = now;
      publishLinkInfo();
    }

    checkLowMemory(now);

    // ========================================================
    // BRIDGE RECOVERY
    // ========================================================

    if (!isValidIp(bridgeIp) ||
        (currentHealth() == Health::BridgeUnreachable &&
         hueFailureStreak() >= BRIDGE_FAILURES_BEFORE_DISCOVERY)) {

      // Rate limited inside; usually returns immediately.
      unsigned long sinceMdns = now - ctx.lastMdnsDiscovery;

      if (!ctx.mdnsTried || sinceMdns >= MDNS_DISCOVERY_INTERVAL_MS) {
        Serial.println("Bridge unreachable; searching for it");

        DiscoveryResult result = discoverBridge(false);

        if (result == DiscoveryResult::NotFound && !isValidIp(bridgeIp)) {
          setHealth(Health::BridgeUnreachable);
        }

        continue;
      }
    }

    // ========================================================
    // POWER COMMANDS HAVE PRIORITY
    // ========================================================

    if (processToggle()) {
      continue;
    }

    // ========================================================
    // BRIGHTNESS
    // ========================================================

    if (processBrightness(now)) {
      continue;
    }

    // ========================================================
    // KEEP THE BRIDGE CONNECTION WARM
    // ========================================================
    //
    // If the bridge closed our idle connection, reopen it now (by
    // polling) so the user's next press doesn't pay the ~2 s TLS
    // handshake.

    if (now - ctx.lastWarmCheck >= 500) {
      ctx.lastWarmCheck = now;

      if (bridgeKeepsAlive && isHealthOk(currentHealth()) &&
          now - lastBridgeRequestAt >= 1000 &&
          now - ctx.lastWarmReconnect >= BRIDGE_REWARM_MIN_INTERVAL_MS &&
          !bridgeClient.connected()) {

        Serial.println("Bridge closed idle connection; reopening");

        ctx.lastWarmReconnect = now;
        ctx.pollNow = true;
      }
    }

    // ========================================================
    // BACKGROUND HUE POLL
    // ========================================================

    bool dirty;
    unsigned long lastUserAction;

    {
      StateLock lock;

      dirty = shared.brightnessDirty;
      lastUserAction = shared.lastUserAction;
    }

    unsigned long pollInterval = isHealthOk(currentHealth())
                                     ? HUE_POLL_INTERVAL_MS
                                     : HUE_POLL_INTERVAL_UNHEALTHY_MS;

    bool pollDue = ctx.pollNow || now - ctx.lastHuePoll >= pollInterval;

    if (pollDue && !dirty &&
        (ctx.pollNow || now - lastUserAction >= USER_ACTION_POLL_GUARD_MS)) {

      ctx.pollNow = false;

      pollGroupState();

      ctx.lastHuePoll = millis();

      continue;
    }

    // Yield without slowing the main UI loop.
    waitMs(5);
  }
}

// ============================================================
// PUBLIC API
// ============================================================

void startNetwork() {
  // Bridge IP: a previously discovered one wins over .env.
  if (isValidIp(settings.bridgeIpOverride)) {
    strlcpy(bridgeIp, settings.bridgeIpOverride, sizeof(bridgeIp));
    publishBridgeIp(true);
  } else {
    strlcpy(bridgeIp, isValidIp(HUE_BRIDGE_IP) ? HUE_BRIDGE_IP : "",
            sizeof(bridgeIp));
    publishBridgeIp(false);
  }

  // Credentials come from .env; don't let the SDK cache stale
  // ones in flash, and do our own reconnects so the reason for
  // every failure is captured.
  WiFi.persistent(false);
  WiFi.setAutoReconnect(false);
  WiFi.onEvent(onWiFiEvent);

  WiFi.setHostname(DEVICE_HOSTNAME);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);

  heartbeat();

  // Arduino loop() runs on core 1. Blocking network work goes
  // on core 0.
  xTaskCreatePinnedToCore(networkTask,        // Function
                          "HueNetwork",       // Name
                          16384,              // Stack size
                          nullptr,            // Parameter
                          1,                  // Priority
                          &networkTaskHandle, // Handle
                          0                   // Core 0
  );
}

void requestToggle() {
  StateLock lock;

  shared.requestedToggleGeneration++;
  shared.lastUserAction = millis();
}

void adjustBrightness(int detents) {
  StateLock lock;

  shared.brightness =
      constrain(shared.brightness + detents * BRIGHTNESS_STEP, 1, 254);

  // Mark latest desired target as unsent.
  shared.brightnessDirty = true;
  shared.lastUserAction = millis();
}

bool requestNetCommand(NetCommand command) {
  StateLock lock;

  if (shared.commandBusy || shared.pendingCommand != NetCommand::None) {
    return false;
  }

  shared.pendingCommand = command;
  shared.commandStatus[0] = '\0';

  // Show something immediately for the self-test screen.
  if (command == NetCommand::SelfTest) {
    shared.selfTestRunning = true;

    for (int i = 0; i < TEST_STEP_COUNT; i++) {
      shared.testSteps[i].status = TestStatus::Pending;
      shared.testSteps[i].detail[0] = '\0';
    }
  }

  return true;
}

void setSettingsOpen(bool open) {
  StateLock lock;

  shared.settingsOpen = open;
}

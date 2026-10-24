#include "input.h"

#include "config.h"

// ============================================================
// BUTTON STATE
// ============================================================

static bool rawButtonState = HIGH;
static bool stableButtonState = HIGH;

// Debounce integrator: rises while the pin reads pressed, falls
// while it reads released (0 .. BUTTON_DEBOUNCE_MS). A noise
// spike only nudges it instead of restarting the timer, so a
// slightly noisy line can't hide a release.
static long pressLevel = 0;
static unsigned long lastSampleAt = 0;

// One sample can count for at most this long, so a single read
// after a slow screen refresh can't flip the state on its own.
static const unsigned long MAX_SAMPLE_WEIGHT_MS = 10;

static unsigned long pressStartedAt = 0;
static unsigned long lastPressDuration = 0;

// Swallow the hold that was in progress at power-on.
static bool ignoreUntilRelease = false;

static bool heldAtBoot = false;

static bool stuck = false;

// ============================================================
// ENCODER STATE
// ============================================================

static uint8_t previousEncoderState = 0;

static int encoderAccumulator = 0;

static InputDiagnostics diagnostics = {};

// ============================================================
// SETUP
// ============================================================

void initInput() {
  pinMode(ENCODER_BUTTON_PIN, INPUT_PULLUP);
  pinMode(ENCODER_CLOCK_PIN, INPUT_PULLUP);
  pinMode(ENCODER_DATA_PIN, INPUT_PULLUP);

  // Let pull-ups settle before sampling.
  delay(5);

  rawButtonState = digitalRead(ENCODER_BUTTON_PIN);
  stableButtonState = rawButtonState;
  pressLevel = stableButtonState == LOW ? BUTTON_DEBOUNCE_MS : 0;
  lastSampleAt = millis();

  heldAtBoot = stableButtonState == LOW;
  ignoreUntilRelease = heldAtBoot;
  pressStartedAt = millis();

  uint8_t clockState = digitalRead(ENCODER_CLOCK_PIN);
  uint8_t dataState = digitalRead(ENCODER_DATA_PIN);

  previousEncoderState = (clockState << 1) | dataState;
}

// ============================================================
// BUTTON
// ============================================================

ButtonEvent readButton() {
  bool reading = digitalRead(ENCODER_BUTTON_PIN);

  unsigned long now = millis();

  // Raw electrical state changed.
  if (reading != rawButtonState) {
    rawButtonState = reading;
    diagnostics.buttonEdges++;
  }

  // No real press lasts this long: the pin is shorted to GND or
  // the wire is loose. Stop treating it as a hold.
  if (stableButtonState == LOW && !stuck &&
      now - pressStartedAt >= BUTTON_STUCK_MS) {
    stuck = true;

    Serial.printf("BTN stuck: GPIO %d has read pressed for %lu s. "
                  "Check the SW wire / encoder module.\n",
                  ENCODER_BUTTON_PIN, (now - pressStartedAt) / 1000);
  }

  long weight = min(now - lastSampleAt, MAX_SAMPLE_WEIGHT_MS);
  lastSampleAt = now;

  if (reading == LOW) {
    pressLevel = min(pressLevel + weight, static_cast<long>(BUTTON_DEBOUNCE_MS));
  } else {
    pressLevel = max(pressLevel - weight, 0L);
  }

  if (stableButtonState == HIGH &&
      pressLevel >= static_cast<long>(BUTTON_DEBOUNCE_MS)) {
    stableButtonState = LOW;
  } else if (stableButtonState == LOW && pressLevel <= 0) {
    stableButtonState = HIGH;
  } else {
    return ButtonEvent::None;
  }

  // Confirmed HIGH -> LOW press.
  if (stableButtonState == LOW) {
    pressStartedAt = now;
    diagnostics.buttonPresses++;
    return ButtonEvent::Pressed;
  }

  // Confirmed LOW -> HIGH release.
  if (stuck) {
    stuck = false;
    ignoreUntilRelease = false;
    Serial.println("BTN released after being stuck; back to normal");
    return ButtonEvent::None;
  }

  if (ignoreUntilRelease) {
    ignoreUntilRelease = false;
    return ButtonEvent::None;
  }

  lastPressDuration = now - pressStartedAt;

  return ButtonEvent::Released;
}

unsigned long lastPressDurationMs() { return lastPressDuration; }

unsigned long buttonHeldMs() {
  if (stableButtonState != LOW || ignoreUntilRelease || stuck) {
    return 0;
  }

  return millis() - pressStartedAt;
}

bool buttonIsDown() { return stableButtonState == LOW && !stuck; }

bool buttonStuck() { return stuck; }

bool buttonHeldAtBoot() { return heldAtBoot; }

// ============================================================
// QUADRATURE ENCODER
// ============================================================

int readEncoderDetents() {
  uint8_t clockState = digitalRead(ENCODER_CLOCK_PIN);
  uint8_t dataState = digitalRead(ENCODER_DATA_PIN);

  uint8_t currentState = (clockState << 1) | dataState;

  if (currentState == previousEncoderState) {
    return 0;
  }

  // Valid quadrature transitions are +/- 1.
  // Invalid/bounce transitions are 0.

  static const int8_t transitionTable[16] = {0,  -1, 1, 0, 1, 0, 0,  -1,
                                             -1, 0,  0, 1, 0, 1, -1, 0};

  uint8_t transition = (previousEncoderState << 2) | currentState;

  int8_t movement = transitionTable[transition];

  previousEncoderState = currentState;

  if (movement == 0) {
    diagnostics.invalidTransitions++;
    return 0;
  }

  diagnostics.validTransitions++;

  encoderAccumulator -= movement;

  if (encoderAccumulator >= TRANSITIONS_PER_DETENT) {
    encoderAccumulator = 0;
    diagnostics.position++;
    return 1;
  }

  if (encoderAccumulator <= -TRANSITIONS_PER_DETENT) {
    encoderAccumulator = 0;
    diagnostics.position--;
    return -1;
  }

  return 0;
}

InputDiagnostics inputDiagnostics() { return diagnostics; }

void readRawPins(bool &button, bool &clock, bool &data) {
  button = digitalRead(ENCODER_BUTTON_PIN);
  clock = digitalRead(ENCODER_CLOCK_PIN);
  data = digitalRead(ENCODER_DATA_PIN);
}

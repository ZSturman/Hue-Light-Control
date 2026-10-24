#pragma once

#include <Arduino.h>

enum class ButtonEvent : uint8_t {
  None,
  Pressed,
  // lastPressDurationMs() says how long it was held.
  Released,
};

struct InputDiagnostics {
  uint32_t validTransitions;
  uint32_t invalidTransitions;
  uint32_t buttonPresses;
  uint32_t buttonEdges;
  long position;
};

void initInput();

// Call every loop. Returns whole detents turned since last call
// (+ clockwise, - counterclockwise).
int readEncoderDetents();

// Call every loop.
ButtonEvent readButton();

// Duration of the press that just produced Released.
unsigned long lastPressDurationMs();

// 0 when released, otherwise how long it has been held.
unsigned long buttonHeldMs();

bool buttonIsDown();

// True while the button has read "pressed" for BUTTON_STUCK_MS or
// more (shorted or loose wire). It is ignored while stuck.
bool buttonStuck();

// True if the button was held when power was applied. That hold
// produces no Pressed/Released events.
bool buttonHeldAtBoot();

InputDiagnostics inputDiagnostics();

// Raw pin levels for the knob test screen.
void readRawPins(bool &button, bool &clock, bool &data);

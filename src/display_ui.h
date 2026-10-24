#pragma once

#include "input.h"

// Finds and initialises the OLED. Safe if none is connected.
void initDisplay();

// Splash with version and any reset warning. Blocks briefly.
void showBootScreen();

void openSettings();

// Call every loop.
void uiHandleInput(int detents, ButtonEvent event);
void uiUpdate();

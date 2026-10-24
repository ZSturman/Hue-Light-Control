#pragma once

#include "app_state.h"

// Configures Wi-Fi and starts the network task on core 0.
void startNetwork();

// UI-side requests. None of these block.
void requestToggle();
void adjustBrightness(int detents);

// Returns false if another command is still running.
bool requestNetCommand(NetCommand command);

void setSettingsOpen(bool open);

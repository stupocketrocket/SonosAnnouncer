#pragma once
#include <Arduino.h>

void displaySetup();

// Full-screen "now announcing" banner with the given text.
void displayShowAnnouncing(const String& text);

// Redraws the idle dashboard (status + any sensor key/values received over
// MQTT). Call sparingly - this panel is tri-color, so every redraw is a slow
// (~15-20s), visibly flashing full refresh, not a fast partial update.
void displayShowDashboard();

// Stores/updates a key/value to be shown on the next dashboard redraw. Does
// NOT trigger a redraw itself - the main sketch throttles that separately.
void displaySetSensorValue(const String& key, const String& value);

#pragma once
#include <Arduino.h>

// Requests speech synthesis from a local Piper TTS server (Config.h:
// TTS_HOST/TTS_PORT) and streams the resulting wav directly to LittleFS
// (Config.h: AUDIO_FILE_PATH) in small chunks, so RAM use stays flat
// regardless of how long the announcement text is - this matters on a plain
// ESP32-WROOM with no PSRAM. Returns true on success.
bool ttsFetchToFile(const String& text);

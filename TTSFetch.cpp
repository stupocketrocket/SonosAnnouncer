#include "TTSFetch.h"
#include "Config.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

bool ttsFetchToFile(const String& text) {
  // Plain HTTP to a local Piper TTS server (e.g. a Proxmox LXC on your own
  // LAN) - no TLS, no API key. This is also why the sketch no longer needs
  // WiFiClientSecure, which was by far the biggest single consumer of flash
  // and RAM in the old ElevenLabs version.
  WiFiClient client;
  HTTPClient http;
  http.setTimeout(20000);   // synthesis + download of a few seconds of audio

  String url = "http://" + String(TTS_HOST) + ":" + String(TTS_PORT) + "/synthesize";
  if (!http.begin(client, url)) {
    Serial.println("[TTS] http.begin failed");
    return false;
  }
  http.addHeader("Content-Type", "application/json");

  JsonDocument body;
  body["text"] = text;

  String payload;
  serializeJson(body, payload);

  int code = http.POST(payload);
  if (code != 200) {
    Serial.printf("[TTS] Piper returned HTTP %d\n", code);
    Serial.println(http.getString());   // error responses are small, safe to buffer
    http.end();
    return false;
  }

  // Stream the response body straight to flash instead of buffering it in RAM -
  // this is the important part on a board with no PSRAM.
  if (LittleFS.exists(AUDIO_FILE_PATH)) LittleFS.remove(AUDIO_FILE_PATH);
  File f = LittleFS.open(AUDIO_FILE_PATH, "w");
  if (!f) {
    Serial.println("[TTS] Failed to open LittleFS file for writing");
    http.end();
    return false;
  }

  WiFiClient* stream = http.getStreamPtr();
  int contentLen = http.getSize();   // -1 if chunked/unknown
  uint8_t buf[512];
  int written = 0;
  unsigned long lastData = millis();

  while (http.connected() && (contentLen < 0 || written < contentLen)) {
    size_t avail = stream->available();
    if (avail == 0) {
      if (millis() - lastData > 8000) break;   // stream stalled, bail out
      delay(5);
      continue;
    }
    size_t toRead = avail > sizeof(buf) ? sizeof(buf) : avail;
    int n = stream->readBytes(buf, toRead);
    if (n > 0) {
      f.write(buf, n);
      written += n;
      lastData = millis();
    }
  }

  f.close();
  http.end();

  Serial.printf("[TTS] Wrote %d bytes to %s\n", written, AUDIO_FILE_PATH);
  return written > 0;
}

#include "AudioServer.h"
#include "Config.h"
#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>

static WebServer server(AUDIO_SERVER_PORT);

static void handleAudioFile() {
  if (!LittleFS.exists(AUDIO_FILE_PATH)) {
    server.send(404, "text/plain", "no announcement audio available");
    return;
  }
  File f = LittleFS.open(AUDIO_FILE_PATH, "r");
  server.streamFile(f, "audio/wav");
  f.close();
}

void audioServerSetup() {
  server.on(AUDIO_FILE_PATH, HTTP_GET, handleAudioFile);
  server.begin();
  Serial.println("[AudioServer] Listening for Sonos to fetch the announcement clip");
}

void audioServerLoop() {
  server.handleClient();
}

String audioServerUrl() {
  return "http://" + WiFi.localIP().toString() + ":" + String(AUDIO_SERVER_PORT) + AUDIO_FILE_PATH;
}

#include "MQTTHandler.h"
#include "DisplayUI.h"
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

static WiFiClient wifiClient;
static PubSubClient mqttClient(wifiClient);
static AnnounceCallback announceCallback = nullptr;

// Expected JSON on TOPIC_ANNOUNCE_CMD, either:
//   {"text": "Someone is at the front door", "volume": 40, "speakers": "all"}
//   {"text": "...", "volume": 40, "speakers": ["Kitchen", "Office"]}
// "volume" and "speakers" are both optional (defaults: SONOS_DEFAULT_VOLUME, "all").
static void handleAnnounceMessage(const String& payload) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.printf("[MQTT] Failed to parse announce JSON: %s\n", err.c_str());
    mqttPublishStatus("error", "", "bad_json");
    return;
  }

  AnnounceCommand cmd;
  cmd.text = doc["text"].as<String>();
  cmd.volume = doc["volume"].is<int>() ? doc["volume"].as<int>() : -1;
  cmd.speakerCount = 0;
  cmd.targetAll = true;

  if (doc["speakers"].is<JsonArray>()) {
    JsonArray arr = doc["speakers"].as<JsonArray>();
    cmd.targetAll = false;
    for (JsonVariant v : arr) {
      if (cmd.speakerCount >= SONOS_MAX_SPEAKERS) break;
      cmd.speakerNames[cmd.speakerCount++] = v.as<String>();
    }
  } else if (doc["speakers"].is<const char*>()) {
    String s = doc["speakers"].as<String>();
    cmd.targetAll = s.equalsIgnoreCase("all");
  }

  if (cmd.text.length() == 0) {
    mqttPublishStatus("error", "", "empty_text");
    return;
  }

  if (announceCallback) announceCallback(cmd);
}

// Expected JSON on TOPIC_DASHBOARD_CMD: {"key": "Outside Temp", "value": "14C"}
// Stores the value only - the main sketch decides when to actually redraw.
static void handleDashboardMessage(const String& payload) {
  JsonDocument doc;
  if (deserializeJson(doc, payload)) return;
  String key = doc["key"].as<String>();
  String value = doc["value"].as<String>();
  if (key.length() > 0) displaySetSensorValue(key, value);
}

static void mqttMessageReceived(char* topic, byte* payloadRaw, unsigned int length) {
  String payload;
  payload.reserve(length + 1);
  for (unsigned int i = 0; i < length; i++) payload += (char)payloadRaw[i];

  if (strcmp(topic, TOPIC_ANNOUNCE_CMD) == 0) {
    handleAnnounceMessage(payload);
  } else if (strcmp(topic, TOPIC_DASHBOARD_CMD) == 0) {
    handleDashboardMessage(payload);
  }
}

static void mqttReconnect() {
  if (mqttClient.connected()) return;
  Serial.println("[MQTT] Connecting...");
  bool ok;
  if (strlen(MQTT_USERNAME) > 0) {
    ok = mqttClient.connect(MQTT_CLIENT_ID, MQTT_USERNAME, MQTT_PASSWORD,
                             TOPIC_AVAILABILITY, 0, true, "offline");
  } else {
    ok = mqttClient.connect(MQTT_CLIENT_ID, nullptr, nullptr,
                             TOPIC_AVAILABILITY, 0, true, "offline");
  }
  if (ok) {
    Serial.println("[MQTT] Connected");
    mqttClient.publish(TOPIC_AVAILABILITY, "online", true);
    mqttClient.subscribe(TOPIC_ANNOUNCE_CMD);
    mqttClient.subscribe(TOPIC_DASHBOARD_CMD);
  } else {
    Serial.printf("[MQTT] Connect failed, rc=%d\n", mqttClient.state());
  }
}

void mqttSetup(AnnounceCallback onAnnounce) {
  announceCallback = onAnnounce;
  mqttClient.setServer(MQTT_BROKER_HOST, MQTT_BROKER_PORT);
  mqttClient.setBufferSize(1024);       // default 256B is too tight for our JSON payloads
  mqttClient.setKeepAlive(MQTT_KEEPALIVE_SEC);  // generous - see README "blocking phases" note
  mqttClient.setCallback(mqttMessageReceived);
  mqttReconnect();
}

void mqttLoop() {
  if (!mqttClient.connected()) {
    static unsigned long lastAttempt = 0;
    if (millis() - lastAttempt > 5000) {
      lastAttempt = millis();
      mqttReconnect();
    }
  }
  mqttClient.loop();
}

bool mqttIsConnected() {
  return mqttClient.connected();
}

void mqttPublishStatus(const char* state, const char* text, const char* detail) {
  JsonDocument doc;
  doc["state"] = state;
  doc["text"] = text;
  doc["detail"] = detail;
  char buf[512];
  size_t n = serializeJson(doc, buf, sizeof(buf));
  mqttClient.publish(TOPIC_ANNOUNCE_STATUS, (const uint8_t*)buf, n, false);
}

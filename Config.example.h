#pragma once
// =============================================================================
// Config.h - all the values you need to edit for your own network/accounts.
// Nothing else in the sketch should need touching to get a first boot working,
// aside from picking the right e-paper panel class in DisplayUI.cpp (see the
// big comment at the top of that file - this one thing genuinely needs you to
// check it against your exact board).
//
// Copy this file to Config.h and fill in your own values. Config.h is
// git-ignored on purpose - it holds your WiFi password and MQTT credentials,
// and should never be committed.
// =============================================================================

// ======================= WiFi =======================
#define WIFI_SSID       "your-wifi-ssid"
#define WIFI_PASSWORD   "your-wifi-password"

// ======================= MQTT ========================
#define MQTT_BROKER_HOST   "192.168.1.10"   // your broker IP (e.g. HA's Mosquitto add-on)
#define MQTT_BROKER_PORT   1883
#define MQTT_USERNAME       "mqtt-user"
#define MQTT_PASSWORD       "mqtt-password"
#define MQTT_CLIENT_ID      "sonos-announcer"
#define MQTT_KEEPALIVE_SEC  60              // generous, see README "blocking phases" note

// Topics
#define TOPIC_ANNOUNCE_CMD     "sonosannouncer/announce/cmd"     // HA -> ESP32 (JSON, see README)
#define TOPIC_ANNOUNCE_STATUS  "sonosannouncer/announce/status"  // ESP32 -> HA  (JSON)
#define TOPIC_DASHBOARD_CMD    "sonosannouncer/dashboard/set"    // HA -> ESP32 (JSON {"key","value"})
#define TOPIC_AVAILABILITY     "sonosannouncer/status"           // LWT: "online" / "offline"

// ======================= Local TTS (Piper) ===========
// Self-hosted Piper TTS HTTP server, e.g. a Proxmox LXC on your own LAN.
// No API key, no TLS - this is plain HTTP, which is also why this sketch no
// longer needs WiFiClientSecure (saves a good chunk of flash/RAM).
#define TTS_HOST   "192.168.1.20"
#define TTS_PORT   5000

// ======================= Local audio server ==========
#define AUDIO_SERVER_PORT   8080
#define AUDIO_FILE_PATH     "/announce.wav"   // stored on LittleFS, streamed to Sonos from here

// ======================= Sonos ========================
#define SONOS_DEFAULT_VOLUME        35     // used if the MQTT payload omits "volume"
#define SONOS_SSDP_TIMEOUT_MS       4000   // how long to listen for SSDP replies at boot
#define SONOS_HTTP_TIMEOUT_MS       4000   // per-SOAP-call timeout
#define SONOS_MAX_SPEAKERS          12     // cap on how many speakers we track at once
#define SONOS_ANNOUNCE_MAX_WAIT_MS  30000  // hard ceiling before we force a restore regardless

// Re-run SSDP discovery + group topology on this cadence while idle (not just
// once at boot). This is what picks up: (a) a speaker that was still asleep/
// reconnecting to WiFi at boot and so got missed entirely by the one-shot
// discovery, and (b) a bonded/grouped zone whose coordinator changed since
// boot (e.g. you re-paired or re-grouped something in the Sonos app) - both
// of these otherwise silently stay wrong until the ESP32 itself reboots.
#define SONOS_REDISCOVER_INTERVAL_MS (10UL * 60UL * 1000UL)

// ======================= E-paper wiring ===============
// VSPI defaults on most ESP32 DevKits: SCK=GPIO18, MOSI=GPIO23 (MISO unused by e-paper).
// Change these to match how you actually wired CS/DC/RST/BUSY.
#define EPD_CS_PIN     5
#define EPD_DC_PIN     17
#define EPD_RST_PIN    16
#define EPD_BUSY_PIN   4

// How often the idle dashboard face is allowed to redraw itself. This panel is
// tri-color, so every redraw is a slow (~15-20s), visibly flashing full refresh -
// don't set this too low.
#define DASHBOARD_REFRESH_INTERVAL_MS (5UL * 60UL * 1000UL)

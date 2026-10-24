#pragma once
#include <Arduino.h>
#include "Config.h"

struct AnnounceCommand {
  String text;
  int volume;                              // -1 = use SONOS_DEFAULT_VOLUME
  bool targetAll;
  String speakerNames[SONOS_MAX_SPEAKERS];  // ignored if targetAll
  int speakerCount;
};

typedef void (*AnnounceCallback)(const AnnounceCommand& cmd);

void mqttSetup(AnnounceCallback onAnnounce);
void mqttLoop();
void mqttPublishStatus(const char* state, const char* text, const char* detail);
bool mqttIsConnected();

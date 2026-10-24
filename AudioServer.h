#pragma once
#include <Arduino.h>

void audioServerSetup();
void audioServerLoop();

// The URL Sonos should fetch, e.g. "http://192.168.1.42:8080/announce.mp3"
String audioServerUrl();

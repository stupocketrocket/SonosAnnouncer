#pragma once
#include <Arduino.h>
#include "Config.h"

struct SonosSpeaker {
  String name;   // room/zone name pulled from the device's SSDP description XML
  String ip;
  bool valid;
  // IP of this speaker's current group coordinator (set by sonosRefreshTopology()).
  // Empty means "unknown / topology fetch failed" - callers should then fall
  // back to controlling this speaker's own ip directly. For a standalone
  // zone this ends up equal to ip itself (it's its own coordinator).
  String coordinatorIp;
};

struct SonosSnapshot {
  bool valid;
  String transportState;      // PLAYING / PAUSED_PLAYBACK / STOPPED / ...
  String currentURI;          // kept exactly as returned (see .cpp note on why)
  String currentURIMetaData;  // kept exactly as returned - already XML-escaped DIDL-Lite
  String relTime;              // HH:MM:SS
  int volume;
  bool mute;
};

// Discover Sonos speakers on the LAN via SSDP. Populates the internal speaker
// list, then refreshes group-coordinator info for all of them (see
// sonosRefreshTopology() below). Called once in setup(), and defensively
// again from the main sketch if a control call fails against every cached IP
// (a speaker may have picked up a new DHCP lease since boot).
int sonosDiscover();

int sonosSpeakerCount();
const SonosSpeaker& sonosSpeakerAt(int index);

// Queries any one reachable speaker's ZoneGroupTopology service for the
// household's current grouping (which zones are bonded/grouped together and
// who the coordinator of each group is), and stamps coordinatorIp onto every
// entry in the internal speaker list. Sonos only accepts direct AVTransport
// control (SetAVTransportURI/Play/...) on a group's coordinator - sending it
// straight to a bonded satellite or non-coordinator group member fails (HTTP
// 500). Called automatically at the end of sonosDiscover(); exposed here too
// in case you want to force a refresh without a full SSDP re-scan.
void sonosRefreshTopology();

// Resolve "all speakers" or a list of room names (case-insensitive exact match
// against the discovered room name) into a list of IPs to act on. Each match
// is resolved through its group coordinator (falling back to its own IP if
// topology info isn't available) and de-duplicated, so a bonded/grouped set
// of N physical units collapses to the single coordinator IP that actually
// accepts transport commands for the whole group.
int sonosResolveTargets(bool all, const String names[], int nameCount,
                         String outIps[], int maxOut);

// Per-speaker control primitives (raw SOAP over HTTP to port 1400, no TLS -
// this is plain LAN traffic). Each returns true on success; treat false as
// "skip this one speaker, don't let it block the others" rather than a fatal
// error for the whole announcement.
bool sonosSnapshot(const String& ip, SonosSnapshot& out);
bool sonosRestore(const String& ip, const SonosSnapshot& snap);
bool sonosSetVolume(const String& ip, int volume);
bool sonosPlayUri(const String& ip, const String& uri);
bool sonosStop(const String& ip);
bool sonosGetTransportState(const String& ip, String& outState);

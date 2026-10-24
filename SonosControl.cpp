#include "SonosControl.h"
#include <WiFi.h>
#include <WiFiUdp.h>
#include <HTTPClient.h>

static SonosSpeaker speakers[SONOS_MAX_SPEAKERS];
static int speakerCount = 0;

// Forward-declared: defined further down in the "SOAP plumbing" section, but
// sonosRefreshTopology() (just after discovery, further up the file) needs
// to call it too.
static bool soapCall(const String& ip, const String& service, const String& action,
                      const String& bodyInner, String& outResponse);

// ---------------------------------------------------------------------------
// We deliberately avoid pulling in a full XML parser to save RAM: Sonos SOAP
// responses are flat and predictable enough that substring extraction works
// reliably in practice (this is the same approach other lightweight Sonos
// clients use). Two flavours:
//   - extractTagRaw: returns the exact bytes between the tags, unescaped.
//     Use this for anything we're going to hand straight back to Sonos later
//     (CurrentURI, CurrentURIMetaData) - re-escaping it ourselves would risk
//     mismatching Sonos's own escaping and breaking the SOAP XML.
//   - extractTag: unescapes basic XML entities. Use this only for values we
//     actually read ourselves (states, numbers, room names).
// ---------------------------------------------------------------------------

static String extractTagRaw(const String& xml, const String& tag) {
  String open = "<" + tag + ">";
  String close = "</" + tag + ">";
  int i = xml.indexOf(open);
  if (i < 0) return "";
  int start = i + open.length();
  int j = xml.indexOf(close, start);
  if (j < 0) return "";
  return xml.substring(start, j);
}

static String xmlUnescape(String s) {
  s.replace("&lt;", "<");
  s.replace("&gt;", ">");
  s.replace("&quot;", "\"");
  s.replace("&apos;", "'");
  s.replace("&amp;", "&");
  return s;
}

static String extractTag(const String& xml, const String& tag) {
  return xmlUnescape(extractTagRaw(xml, tag));
}

// Same idea as extractTagRaw, but for a quoted XML *attribute* (name="value"),
// and bounded to [from, to) so a caller can scan one <ZoneGroup>...</ZoneGroup>
// block at a time without accidentally matching into a sibling block.
static String extractAttrInRange(const String& xml, int from, int to, const String& attr) {
  String needle = attr + "=\"";
  int i = xml.indexOf(needle, from);
  if (i < 0 || i >= to) return "";
  int start = i + needle.length();
  int j = xml.indexOf('"', start);
  if (j < 0 || j > to) return "";
  return xml.substring(start, j);
}

// "http://192.168.0.63:1400/xml/device_description.xml" -> "192.168.0.63"
static String ipFromLocation(const String& location) {
  int schemeEnd = location.indexOf("://");
  int start = (schemeEnd >= 0) ? schemeEnd + 3 : 0;
  int colon = location.indexOf(':', start);
  int slash = location.indexOf('/', start);
  int end = colon;
  if (end < 0 || (slash >= 0 && slash < end)) end = slash;
  if (end < 0) end = location.length();
  return location.substring(start, end);
}

// ---------------------------------------------------------------------------
// SSDP discovery
// ---------------------------------------------------------------------------

int sonosDiscover() {
  speakerCount = 0;
  WiFiUDP udp;
  IPAddress mcastAddr(239, 255, 255, 250);
  const uint16_t mcastPort = 1900;

  udp.begin(1901);   // fixed local port for sending + collecting unicast replies

  const char* msearch =
    "M-SEARCH * HTTP/1.1\r\n"
    "HOST: 239.255.255.250:1900\r\n"
    "MAN: \"ssdp:discover\"\r\n"
    "MX: 3\r\n"
    "ST: urn:schemas-upnp-org:device:ZonePlayer:1\r\n"
    "\r\n";

  udp.beginPacket(mcastAddr, mcastPort);
  udp.write((const uint8_t*)msearch, strlen(msearch));
  udp.endPacket();

  unsigned long start = millis();
  String seenIps[SONOS_MAX_SPEAKERS];
  int seenCount = 0;

  while (millis() - start < SONOS_SSDP_TIMEOUT_MS && speakerCount < SONOS_MAX_SPEAKERS) {
    int packetSize = udp.parsePacket();
    if (packetSize <= 0) { delay(20); continue; }

    char buf[1024];
    int len = udp.read(buf, packetSize > (int)sizeof(buf) - 1 ? (int)sizeof(buf) - 1 : packetSize);
    if (len <= 0) continue;
    buf[len] = '\0';
    String resp(buf);

    String ipStr = udp.remoteIP().toString();

    bool already = false;
    for (int i = 0; i < seenCount; i++) if (seenIps[i] == ipStr) { already = true; break; }
    if (already) continue;

    int locIdx = resp.indexOf("LOCATION:");
    if (locIdx < 0) locIdx = resp.indexOf("Location:");
    if (locIdx < 0) continue;
    int lineEnd = resp.indexOf("\r\n", locIdx);
    String location = resp.substring(locIdx + 9, lineEnd < 0 ? resp.length() : lineEnd);
    location.trim();
    if (location.length() == 0) continue;

    seenIps[seenCount++] = ipStr;

    // Fetch the device description XML so we get a human room name instead
    // of just an IP - makes the "speakers": ["Kitchen","Office"] MQTT payload
    // possible.
    HTTPClient http;
    http.setTimeout(SONOS_HTTP_TIMEOUT_MS);
    String roomName = ipStr;   // fallback if this fails
    if (http.begin(location)) {
      int code = http.GET();
      if (code == 200) {
        String body = http.getString();
        String rn = extractTag(body, "roomName");
        if (rn.length() > 0) roomName = rn;
      }
      http.end();
    }

    speakers[speakerCount].name = roomName;
    speakers[speakerCount].ip = ipStr;
    speakers[speakerCount].valid = true;
    speakers[speakerCount].coordinatorIp = "";
    speakerCount++;

    Serial.printf("[Sonos] Discovered '%s' at %s\n", roomName.c_str(), ipStr.c_str());
  }

  udp.stop();
  Serial.printf("[Sonos] Discovery complete: %d speaker(s)\n", speakerCount);

  sonosRefreshTopology();
  return speakerCount;
}

int sonosSpeakerCount() { return speakerCount; }

const SonosSpeaker& sonosSpeakerAt(int index) { return speakers[index]; }

// ---------------------------------------------------------------------------
// Group topology
//
// Sonos only accepts direct AVTransport control (SetAVTransportURI, Play,
// GetTransportInfo, ...) on a group's *coordinator*. Send it to a bonded
// satellite/sub or a non-coordinator group member instead and you get an
// HTTP 500 - this is what broke "speakers": "all" against a household with
// any stereo pair, home theater (soundbar+surrounds+sub) or regular
// multi-room group in it. Any single Sonos device can answer
// ZoneGroupTopology::GetZoneGroupState for the whole household, so we only
// need one successful call.
//
// The response wraps a full nested XML document (ZoneGroups/ZoneGroup/
// ZoneGroupMember) as HTML-entity-escaped text inside a single
// <ZoneGroupState> tag. We unescape it once into a single String (a few KB
// for a household this size - acceptable as a one-off, immediately-freed
// allocation done once at discovery time, not per announcement), then scan
// it with the same bounded substring approach used elsewhere in this file -
// no XML parser library pulled in, so no extra flash for something we only
// need a few fields out of.
// ---------------------------------------------------------------------------

// A bonded/stereo-pair or home theater (soundbar+surrounds+sub) group is NOT
// represented as several sibling <ZoneGroupMember> tags - Sonos nests the
// extra units as <Satellite> child elements INSIDE the primary member's own
// <ZoneGroupMember>...</ZoneGroupMember> tag, e.g.:
//   <ZoneGroupMember UUID="RINCON_PRIMARY" Location="...192.168.0.109...">
//     <Satellite UUID="RINCON_SUB" Location="...192.168.0.63..." .../>
//     <Satellite UUID="RINCON_SURROUND_R" Location="...192.168.0.141..." .../>
//   </ZoneGroupMember>
// Both tags carry the same UUID/Location attributes, so the same bounded
// substring scan works for either - this just lets the caller pick which
// tag name to scan for, so a satellite's IP gets found too instead of
// silently never matching anything and falling back to being targeted
// directly (which is what was still breaking the bonded "Living Room" group:
// its satellites answer SSDP independently and end up in our speakers[] list,
// but were invisible to this scan until now).
static String findMemberLocation(const String& topo, int from, int to,
                                  const String& tag, const String& wantUuid) {
  int mp = from;
  while (true) {
    int m = topo.indexOf(tag, mp);
    if (m < 0 || m >= to) return "";
    int mEnd = topo.indexOf('>', m);
    if (mEnd < 0 || mEnd > to) mEnd = to;
    if (extractAttrInRange(topo, m, mEnd, "UUID") == wantUuid) {
      return ipFromLocation(extractAttrInRange(topo, m, mEnd, "Location"));
    }
    mp = mEnd + 1;
  }
}

static void stampMembers(const String& topo, int from, int to, const String& tag,
                          const String& coordIp, String& memberNames) {
  int mp = from;
  while (true) {
    int m = topo.indexOf(tag, mp);
    if (m < 0 || m >= to) break;
    int mEnd = topo.indexOf('>', m);
    if (mEnd < 0 || mEnd > to) mEnd = to;
    String ip = ipFromLocation(extractAttrInRange(topo, m, mEnd, "Location"));
    for (int j = 0; j < speakerCount; j++) {
      if (speakers[j].ip == ip) {
        speakers[j].coordinatorIp = coordIp;
        memberNames += " " + speakers[j].name + "(" + ip + ")";
        break;
      }
    }
    mp = mEnd + 1;
  }
}

void sonosRefreshTopology() {
  for (int i = 0; i < speakerCount; i++) speakers[i].coordinatorIp = "";

  bool gotTopology = false;
  for (int i = 0; i < speakerCount && !gotTopology; i++) {
    String resp;
    if (!soapCall(speakers[i].ip, "ZoneGroupTopology", "GetZoneGroupState", "", resp)) continue;

    String topo = xmlUnescape(extractTagRaw(resp, "ZoneGroupState"));
    resp = String();   // done with the outer envelope copy - free it before parsing
    if (topo.length() == 0) continue;

    int pos = 0;
    while (true) {
      int g = topo.indexOf("<ZoneGroup ", pos);
      if (g < 0) break;
      int gEnd = topo.indexOf("</ZoneGroup>", g);
      if (gEnd < 0) gEnd = topo.length();
      pos = gEnd + 1;

      String coordUuid = extractAttrInRange(topo, g, gEnd, "Coordinator");
      if (coordUuid.length() == 0) continue;

      // Pass 1: find the coordinator member's own IP within this group. The
      // coordinator is practically always the primary (non-satellite) unit,
      // but check <Satellite> too so an unusual setup doesn't silently skip
      // the whole group.
      String coordIp = findMemberLocation(topo, g, gEnd, "<ZoneGroupMember ", coordUuid);
      if (coordIp.length() == 0) coordIp = findMemberLocation(topo, g, gEnd, "<Satellite ", coordUuid);
      if (coordIp.length() == 0) continue;   // malformed/unexpected - skip this group

      // Pass 2: stamp every member of this group - coordinator, any other
      // plain ZoneGroupMember, AND any bonded <Satellite> (sub/surround/
      // stereo partner) - with that coordinator IP.
      String memberNames = "";
      stampMembers(topo, g, gEnd, "<ZoneGroupMember ", coordIp, memberNames);
      stampMembers(topo, g, gEnd, "<Satellite ", coordIp, memberNames);
      // One line per group: makes it obvious at a glance whether a given
      // speaker landed in the group you expected, and which IP is actually
      // the coordinator everything in that group gets routed to.
      Serial.printf("[Sonos] Zone group, coordinator %s:%s\n", coordIp.c_str(), memberNames.c_str());
    }
    gotTopology = true;
  }

  if (!gotTopology) {
    Serial.println("[Sonos] Could not fetch group topology - falling back to "
                    "per-speaker control (bonded/grouped zones may not play)");
  }
}

// Resolves one speaker index to the IP that should actually receive
// transport commands (its group coordinator, or itself if topology info
// isn't available), and appends it to outIps unless it's already there -
// several matched speakers commonly share one coordinator (a bonded/grouped
// set), and we only want to send each SOAP call once.
static void addResolvedTarget(int speakerIndex, String outIps[], int& n, int maxOut) {
  if (n >= maxOut) return;
  const String& ip = speakers[speakerIndex].coordinatorIp.length() > 0
                        ? speakers[speakerIndex].coordinatorIp
                        : speakers[speakerIndex].ip;
  for (int k = 0; k < n; k++) if (outIps[k] == ip) return;
  outIps[n++] = ip;
}

int sonosResolveTargets(bool all, const String names[], int nameCount,
                         String outIps[], int maxOut) {
  int n = 0;
  if (all) {
    for (int i = 0; i < speakerCount && n < maxOut; i++) addResolvedTarget(i, outIps, n, maxOut);
    return n;
  }
  for (int i = 0; i < nameCount; i++) {
    for (int j = 0; j < speakerCount; j++) {
      if (speakers[j].name.equalsIgnoreCase(names[i])) {
        addResolvedTarget(j, outIps, n, maxOut);
        break;
      }
    }
  }
  return n;
}

// ---------------------------------------------------------------------------
// SOAP plumbing
// ---------------------------------------------------------------------------

static bool soapCall(const String& ip, const String& service, const String& action,
                      const String& bodyInner, String& outResponse) {
  String path;
  if (service == "AVTransport") path = "/MediaRenderer/AVTransport/Control";
  else if (service == "RenderingControl") path = "/MediaRenderer/RenderingControl/Control";
  else path = "/ZoneGroupTopology/Control";   // service == "ZoneGroupTopology"
  String url = "http://" + ip + ":1400" + path;

  String soapAction = "urn:schemas-upnp-org:service:" + service + ":1#" + action;
  String envelope =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
    "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
    "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
    "<s:Body>"
    "<u:" + action + " xmlns:u=\"urn:schemas-upnp-org:service:" + service + ":1\">"
    + bodyInner +
    "</u:" + action + ">"
    "</s:Body></s:Envelope>";

  HTTPClient http;
  http.setTimeout(SONOS_HTTP_TIMEOUT_MS);
  if (!http.begin(url)) return false;
  http.addHeader("Content-Type", "text/xml; charset=\"utf-8\"");
  http.addHeader("SOAPACTION", "\"" + soapAction + "\"");

  int code = http.POST(envelope);
  outResponse = http.getString();
  http.end();

  if (code != 200) {
    Serial.printf("[Sonos] SOAP %s/%s to %s failed, HTTP %d\n",
                  service.c_str(), action.c_str(), ip.c_str(), code);
    return false;
  }
  return true;
}

bool sonosGetTransportState(const String& ip, String& outState) {
  String resp;
  if (!soapCall(ip, "AVTransport", "GetTransportInfo", "<InstanceID>0</InstanceID>", resp)) return false;
  outState = extractTag(resp, "CurrentTransportState");
  return outState.length() > 0;
}

bool sonosSnapshot(const String& ip, SonosSnapshot& out) {
  out.valid = false;
  String resp;

  if (!soapCall(ip, "AVTransport", "GetTransportInfo", "<InstanceID>0</InstanceID>", resp)) return false;
  out.transportState = extractTag(resp, "CurrentTransportState");

  // GetPositionInfo's response already carries TrackURI/TrackMetaData alongside
  // RelTime - for the direct-URI playback this announcer deals with, that's
  // the same thing GetMediaInfo's CurrentURI/CurrentURIMetaData would have
  // given us, so we can skip the separate GetMediaInfo call entirely. That's
  // one fewer blocking SOAP round-trip per target in the snapshot phase
  // (previously 5, now 4), which matters since every target is done serially.
  if (!soapCall(ip, "AVTransport", "GetPositionInfo", "<InstanceID>0</InstanceID>", resp)) return false;
  out.currentURI = extractTagRaw(resp, "TrackURI");
  out.currentURIMetaData = extractTagRaw(resp, "TrackMetaData");
  out.relTime = extractTag(resp, "RelTime");

  if (soapCall(ip, "RenderingControl", "GetVolume",
               "<InstanceID>0</InstanceID><Channel>Master</Channel>", resp)) {
    String v = extractTag(resp, "CurrentVolume");
    out.volume = v.length() ? v.toInt() : SONOS_DEFAULT_VOLUME;
  } else {
    out.volume = SONOS_DEFAULT_VOLUME;
  }

  if (soapCall(ip, "RenderingControl", "GetMute",
               "<InstanceID>0</InstanceID><Channel>Master</Channel>", resp)) {
    out.mute = extractTag(resp, "CurrentMute") == "1";
  } else {
    out.mute = false;
  }

  out.valid = true;
  return true;
}

bool sonosSetVolume(const String& ip, int volume) {
  String resp;
  String body = "<InstanceID>0</InstanceID><Channel>Master</Channel><DesiredVolume>"
                + String(volume) + "</DesiredVolume>";
  return soapCall(ip, "RenderingControl", "SetVolume", body, resp);
}

static bool sonosSetMute(const String& ip, bool mute) {
  String resp;
  String body = "<InstanceID>0</InstanceID><Channel>Master</Channel><DesiredMute>"
                + String(mute ? 1 : 0) + "</DesiredMute>";
  return soapCall(ip, "RenderingControl", "SetMute", body, resp);
}

bool sonosPlayUri(const String& ip, const String& uri) {
  String resp;
  String setUriBody = "<InstanceID>0</InstanceID><CurrentURI>" + uri
                       + "</CurrentURI><CurrentURIMetaData></CurrentURIMetaData>";
  if (!soapCall(ip, "AVTransport", "SetAVTransportURI", setUriBody, resp)) return false;
  return soapCall(ip, "AVTransport", "Play", "<InstanceID>0</InstanceID><Speed>1</Speed>", resp);
}

bool sonosStop(const String& ip) {
  String resp;
  return soapCall(ip, "AVTransport", "Stop", "<InstanceID>0</InstanceID>", resp);
}

bool sonosRestore(const String& ip, const SonosSnapshot& snap) {
  if (!snap.valid) return false;
  String resp;

  if (snap.currentURI.length() > 0) {
    String body = "<InstanceID>0</InstanceID><CurrentURI>" + snap.currentURI
                  + "</CurrentURI><CurrentURIMetaData>" + snap.currentURIMetaData
                  + "</CurrentURIMetaData>";
    soapCall(ip, "AVTransport", "SetAVTransportURI", body, resp);   // best-effort

    if (snap.relTime.length() > 0) {
      // Best-effort: some sources (radio streams, some queues) can't seek and
      // will fault here - that's fine, we don't treat it as fatal.
      String seekBody = "<InstanceID>0</InstanceID><Unit>REL_TIME</Unit><Target>"
                         + snap.relTime + "</Target>";
      soapCall(ip, "AVTransport", "Seek", seekBody, resp);
    }
  }

  sonosSetVolume(ip, snap.volume);
  sonosSetMute(ip, snap.mute);

  if (snap.transportState == "PLAYING") {
    soapCall(ip, "AVTransport", "Play", "<InstanceID>0</InstanceID><Speed>1</Speed>", resp);
  } else if (snap.transportState == "PAUSED_PLAYBACK") {
    soapCall(ip, "AVTransport", "Pause", "<InstanceID>0</InstanceID>", resp);
  } else {
    soapCall(ip, "AVTransport", "Stop", "<InstanceID>0</InstanceID>", resp);
  }

  return true;
}

// =============================================================================
// SonosAnnouncer
//
// Receives an announcement request over MQTT, converts it to speech via a
// local Piper TTS server, and drives one or more Sonos speakers directly over
// the LAN
// (SOAP/UPnP) to: snapshot their current state, set volume, play the
// announcement, then restore what they were doing before - all from this
// one ESP32, no Home Assistant automation logic involved. The e-paper panel
// shows the text while it's being spoken, and a small sensor dashboard the
// rest of the time.
//
// See README.md for required libraries, wiring, and the MQTT payload schema.
// See DisplayUI.cpp for the one thing you MUST verify against your exact
// e-paper board before this will show anything sensible.
// =============================================================================

#include <WiFi.h>
#include <LittleFS.h>
#include "Config.h"
#include "MQTTHandler.h"
#include "SonosControl.h"
#include "TTSFetch.h"
#include "AudioServer.h"
#include "DisplayUI.h"

enum AnnounceState {
  STATE_IDLE,
  STATE_SNAPSHOT,
  STATE_FETCH_TTS,
  STATE_PLAY,
  STATE_WAIT_FINISH,
  STATE_RESTORE
};

static AnnounceState state = STATE_IDLE;
static AnnounceCommand pendingCmd;
static bool cmdPending = false;

static String targetIps[SONOS_MAX_SPEAKERS];
static int targetCount = 0;
static SonosSnapshot snapshots[SONOS_MAX_SPEAKERS];
static unsigned long stateEnteredAt = 0;
static unsigned long jobStartedAt = 0;
static unsigned long lastDashboardRefresh = 0;
static unsigned long lastTopologyRefresh = 0;

static void enterState(AnnounceState s) {
  state = s;
  stateEnteredAt = millis();
}

// Called from the MQTT layer whenever a well-formed announce command arrives.
static void onAnnounceReceived(const AnnounceCommand& cmd) {
  if (state != STATE_IDLE) {
    Serial.println("[Main] Announce received while busy - dropping it (v2: could queue instead)");
    mqttPublishStatus("error", cmd.text.c_str(), "busy");
    return;
  }
  pendingCmd = cmd;
  cmdPending = true;
}

void setup() {
  Serial.begin(115200);
  delay(200);

  LittleFS.begin(true);   // format on first boot if the filesystem isn't there yet

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("[Main] Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("[Main] IP address: ");
  Serial.println(WiFi.localIP());

  displaySetup();
  displayShowDashboard();

  audioServerSetup();
  mqttSetup(onAnnounceReceived);

  int found = sonosDiscover();
  Serial.printf("[Main] Found %d Sonos speaker(s) at boot via SSDP\n", found);

  lastDashboardRefresh = millis();
  lastTopologyRefresh = millis();
}

// ---------------------------------------------------------------------------
// Each of the functions below runs to completion within a single loop() call
// before handing back control - this is a coarse-grained state machine, not
// a fully cooperative one. Each phase is bounded by SONOS_HTTP_TIMEOUT_MS per
// SOAP call (or the TTS HTTP timeout), so nothing hangs forever, but a phase
// with several speakers or a slow network can still block loop() for a few
// seconds at a time. MQTT_KEEPALIVE_SEC is set generously in Config.h to
// tolerate that; see README for the details and what a stricter
// (task-per-core) design would look like if you want to remove this
// simplification later.
// ---------------------------------------------------------------------------

static void startJob() {
  targetCount = sonosResolveTargets(pendingCmd.targetAll, pendingCmd.speakerNames,
                                     pendingCmd.speakerCount, targetIps, SONOS_MAX_SPEAKERS);

  if (targetCount == 0) {
    // Maybe a speaker's DHCP lease changed since boot - try one re-discovery
    // before giving up.
    Serial.println("[Main] No matching speakers found - re-running SSDP discovery");
    sonosDiscover();
    targetCount = sonosResolveTargets(pendingCmd.targetAll, pendingCmd.speakerNames,
                                       pendingCmd.speakerCount, targetIps, SONOS_MAX_SPEAKERS);
  }

  if (targetCount == 0) {
    Serial.println("[Main] Still no matching speakers - aborting this announcement");
    mqttPublishStatus("error", pendingCmd.text.c_str(), "no_speakers_found");
    cmdPending = false;
    enterState(STATE_IDLE);
    return;
  }

  // Log exactly which IPs this job resolved to - cheap, and the single most
  // useful line for diagnosing a "some speaker didn't play" report after the
  // fact (was it even targeted, and if so, was it a sensible coordinator IP).
  Serial.printf("[Main] Resolved %d target(s):", targetCount);
  for (int i = 0; i < targetCount; i++) Serial.printf(" %s", targetIps[i].c_str());
  Serial.println();

  displayShowAnnouncing(pendingCmd.text);
  mqttPublishStatus("speaking", pendingCmd.text.c_str(), "");
  jobStartedAt = millis();
  enterState(STATE_SNAPSHOT);
}

static void doSnapshot() {
  for (int i = 0; i < targetCount; i++) {
    sonosSnapshot(targetIps[i], snapshots[i]);   // best-effort per speaker
  }
  enterState(STATE_FETCH_TTS);
}

static void doFetchTts() {
  bool ok = ttsFetchToFile(pendingCmd.text);
  if (!ok) {
    Serial.println("[Main] TTS fetch failed - skipping playback, restoring speakers");
    mqttPublishStatus("error", pendingCmd.text.c_str(), "tts_failed");
    enterState(STATE_RESTORE);
    return;
  }
  enterState(STATE_PLAY);
}

static void doPlay() {
  String url = audioServerUrl();
  int vol = pendingCmd.volume >= 0 ? pendingCmd.volume : SONOS_DEFAULT_VOLUME;
  for (int i = 0; i < targetCount; i++) {
    sonosSetVolume(targetIps[i], vol);
    sonosPlayUri(targetIps[i], url);

    // The instant the FIRST target gets its Play command, Sonos tries to
    // fetch the clip back from our own audioServer (see AudioServer.cpp) -
    // but that only happens when audioServerLoop() gets a chance to run.
    // Without this, we'd stay fully blocked here issuing SOAP calls to the
    // remaining targets before ever servicing that fetch request, which is
    // what was causing the announcement to take anywhere from ~7s to ~22s
    // depending on whether Sonos's request happened to land in a gap or not.
    // Calling it after every target keeps the web server responsive
    // throughout this loop instead of only once the whole loop is done.
    audioServerLoop();
  }
  enterState(STATE_WAIT_FINISH);
}

static void doWaitFinish() {
  // Give playback a moment to actually start before polling for completion.
  if (millis() - stateEnteredAt < 1500) return;

  // Must wait for EVERY target, not just targetIps[0]: with "all speakers"
  // now correctly deduped to one call per group coordinator (see
  // sonosResolveTargets), different targets can be entirely different
  // physical setups - a single standalone speaker vs. a 4-unit bonded/synced
  // group - with very different start-up latency. Polling only index 0 meant
  // a fast single speaker finishing its clip would trigger doRestore() for
  // ALL targets, which stops/restores a slower group's coordinator before it
  // had even finished (sometimes before it was audibly playing at all) -
  // this is what was silencing the bonded "Living Room" group specifically.
  bool allFinished = true;
  for (int i = 0; i < targetCount; i++) {
    String txState;
    bool ok = sonosGetTransportState(targetIps[i], txState);
    bool finished = ok && txState != "PLAYING" && txState != "TRANSITIONING";
    audioServerLoop();   // keep servicing a late/retried clip fetch while we poll
    if (!finished) { allFinished = false; break; }
  }
  bool timedOut = (millis() - jobStartedAt) > SONOS_ANNOUNCE_MAX_WAIT_MS;

  if (allFinished || timedOut) {
    if (timedOut) Serial.println("[Main] Announce wait timed out - forcing restore anyway");
    enterState(STATE_RESTORE);
  }
}

static void doRestore() {
  // Unconditional: every speaker we snapshotted gets a restore attempt, even
  // if playback failed or timed out - we never want to leave a speaker stuck
  // quiet/muted because something upstream went wrong.
  for (int i = 0; i < targetCount; i++) {
    sonosRestore(targetIps[i], snapshots[i]);
  }
  mqttPublishStatus("idle", pendingCmd.text.c_str(), "");
  displayShowDashboard();
  lastDashboardRefresh = millis();
  cmdPending = false;
  enterState(STATE_IDLE);
}

void loop() {
  mqttLoop();
  audioServerLoop();

  if (state == STATE_IDLE && cmdPending) {
    startJob();
    return;
  }

  switch (state) {
    case STATE_SNAPSHOT:    doSnapshot();    break;
    case STATE_FETCH_TTS:   doFetchTts();    break;
    case STATE_PLAY:        doPlay();        break;
    case STATE_WAIT_FINISH: doWaitFinish();  break;
    case STATE_RESTORE:     doRestore();     break;
    default: break;
  }

  // Periodically redraw the idle dashboard so any sensor values pushed over
  // TOPIC_DASHBOARD_CMD actually show up - throttled because this panel's
  // full refresh is slow and visibly flashes.
  if (state == STATE_IDLE && !cmdPending &&
      millis() - lastDashboardRefresh > DASHBOARD_REFRESH_INTERVAL_MS) {
    displayShowDashboard();
    lastDashboardRefresh = millis();
  }

  // Periodically re-run SSDP discovery + group topology while idle. Boot-time
  // discovery is a single snapshot: a speaker that was still asleep/
  // reconnecting at that exact moment is missed and stays invisible to every
  // "all speakers" announce forever (targetCount is never 0 once at least one
  // other speaker resolves, so the existing zero-targets rediscovery in
  // startJob() never kicks in to catch it) - and a bonded/grouped zone whose
  // coordinator changes later (re-paired or re-grouped via the Sonos app)
  // stays pointed at the old, now-wrong coordinator IP the same way. Only
  // runs while idle so it never adds latency to an in-progress announcement.
  if (state == STATE_IDLE && !cmdPending &&
      millis() - lastTopologyRefresh > SONOS_REDISCOVER_INTERVAL_MS) {
    Serial.println("[Main] Periodic refresh: re-running Sonos discovery + group topology");
    sonosDiscover();
    lastTopologyRefresh = millis();
  }

  // TODO (optional): if you wire an onboard sensor (DHT22/BME280/etc.) to
  // this board, read it here on its own timer, call
  // displaySetSensorValue("Temp", ...) and mqttClient-style publish it out
  // too. Left as a hook rather than guessed at, since no sensor was specified.
}

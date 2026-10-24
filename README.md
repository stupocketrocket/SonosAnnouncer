# SonosAnnouncer

An ESP32 firmware project that turns a single MQTT message into a spoken
announcement on one or more Sonos speakers: it converts text to speech via a
self-hosted Piper TTS server, drives the Sonos speakers directly over the LAN
(SOAP/UPnP) to snapshot what they were doing, set volume, play the
announcement, and restore their previous state, and shows the announcement
text (plus a small idle sensor dashboard the rest of the time) on an e-paper
display. All of the control logic lives on the ESP32 itself - Home Assistant,
or anything else that can publish one MQTT message, is enough to trigger an
announcement. This repo also includes a ready-made Home Assistant dashboard
card and script so you can trigger announcements with a button press instead
of writing an automation by hand.

Built for a plain ESP32-WROOM DevKit (no PSRAM), which is why several design
choices below exist specifically to keep RAM use flat.

```
repo root
├── SonosAnnouncer.ino          main state machine / loop()
├── Config.example.h            copy to Config.h and fill in your secrets
├── SonosControl.cpp / .h       SSDP discovery, group topology, SOAP control
├── TTSFetch.cpp / .h           Piper HTTP client, streams WAV to LittleFS
├── AudioServer.cpp / .h        tiny web server that serves the WAV to Sonos
├── MQTTHandler.cpp / .h        MQTT command parsing + status publishing
├── DisplayUI.cpp / .h          e-paper "announcing" banner + idle dashboard
└── homeassistant/
    ├── helpers.yaml                         input_text/number/boolean helpers
    ├── script_send_sonos_announcement.yaml  the script the dashboard button calls
    └── dashboard_section_sonos_announce.yaml the Lovelace card itself
```

---

## Part 1 - The plan

This project went through three stages, each driven by a real problem rather
than a feature wishlist:

1. **Get a working announcer.** Receive an MQTT command, speak it on Sonos via
   a local Piper TTS server, and put something readable on an e-paper display
   while it's happening - without needing Home Assistant to know anything
   about Sonos itself.
2. **Make it actually reliable on this household's real network**, once it was
   running day-to-day and specific speakers started misbehaving in specific,
   reproducible ways (see Part 2's "Debugging journey" - every item there was
   a real symptom, not a hypothetical).
3. **Make it usable without opening the Serial monitor or hand-writing MQTT
   payloads** - add a Home Assistant dashboard card so anyone in the house can
   pick a speaker (or all of them), type a message, and press one button.

Every section below maps to one of these three stages.

---

## Part 2 - Implementation and architecture

### High-level flow

```
MQTT "announce" message
        |
        v
  [state machine in SonosAnnouncer.ino]
        |
        +--> resolve target IP(s)        (SonosControl: SSDP cache + group topology)
        +--> snapshot each speaker        (SOAP: GetTransportInfo/PositionInfo/Volume/Mute)
        +--> fetch TTS audio              (TTSFetch: POST text -> Piper -> stream to LittleFS)
        +--> point speakers at this board (SOAP: SetAVTransportURI -> http://esp32:8080/announce.wav)
        +--> Play                        (AudioServer serves the file back to Sonos)
        +--> poll until not PLAYING, or 30s ceiling
        +--> restore every speaker        (SOAP: SetAVTransportURI/Seek/Volume/Mute/transport state)
        |
        v
  e-paper reverts to idle sensor dashboard
```

### The state machine

`SonosAnnouncer.ino` runs a coarse-grained, deliberately simple state machine:

```
STATE_IDLE -> STATE_SNAPSHOT -> STATE_FETCH_TTS -> STATE_PLAY
           -> STATE_WAIT_FINISH -> STATE_RESTORE -> STATE_IDLE
```

Each state's handler runs to completion within a single `loop()` call rather
than being split into non-blocking steps - `doSnapshot()` fires off every
SOAP call for every target speaker back-to-back, `doFetchTts()` blocks until
the whole WAV has streamed to flash, and so on. This is intentional: a fully
non-blocking (task-per-phase) design would be more "correct" in the abstract,
but far harder to reason about and debug on a single core with no RTOS
experience to lean on, for a device that only needs to handle one
announcement at a time. Each phase is still individually bounded -
`SONOS_HTTP_TIMEOUT_MS` per SOAP call, a dedicated TTS stall timeout, and a
hard `SONOS_ANNOUNCE_MAX_WAIT_MS` ceiling on the whole job - so nothing hangs
forever, and `MQTT_KEEPALIVE_SEC` is set generously (60s) in `Config.h`
specifically to tolerate the odd multi-second blocking phase without the
broker dropping the connection.

A command arriving while busy is simply dropped with an `error`/`busy`
status rather than queued - acceptable for a single-household announcer, and
left as a clearly marked `// v2` extension point if you ever want queuing.

### Sonos control and the bonded/grouped-speaker problem

Sonos speakers are controlled over plain HTTP SOAP/UPnP on port 1400 - the
same undocumented-but-stable interface every third-party Sonos tool
(python-soco, node-sonos-http-api, Home Assistant's own integration) relies
on. `SonosControl.cpp` implements this from scratch with lightweight
substring XML extraction (`extractTag`, `extractTagRaw`, `extractAttrInRange`)
rather than a full XML parser, to keep RAM use flat on a board with no PSRAM.

The one subtlety that isn't optional: **Sonos only accepts direct transport
control (`SetAVTransportURI`, `Play`, `GetTransportInfo`, ...) on a group's
*coordinator***. Sending those commands straight to a bonded satellite, a
stereo-pair's non-primary unit, or a non-coordinator member of a multi-room
group fails outright with an HTTP 500. `sonosDiscover()` therefore always
follows SSDP discovery with `sonosRefreshTopology()`, which queries any one
reachable speaker's `ZoneGroupTopology::GetZoneGroupState` for the whole
household's current grouping, scans each `<ZoneGroup>` block (including
nested `<Satellite>` tags, which is how a bonded pair or a home-theater
setup's surrounds/sub show up), and stamps every discovered speaker with its
coordinator's IP. `sonosResolveTargets()` then resolves `"all"` or a list of
room names through that coordinator mapping and de-duplicates, so a bonded
set of N physical units collapses to the single IP that actually accepts the
commands for the whole group. If the topology fetch itself fails (every
speaker briefly unreachable), control falls back to each speaker's own IP and
logs a warning rather than failing the whole announcement - standalone zones
are unaffected either way.

### TTS and audio delivery

`TTSFetch.cpp` POSTs `{"text": "..."}` to the local Piper server and streams
the WAV response **directly to LittleFS in 512-byte chunks** rather than
buffering it in RAM - on a board with no PSRAM, a longer sentence's full audio
clip would otherwise eat most of the free heap. `AudioServer.cpp` then serves
that file back over a tiny built-in `WebServer` on port 8080, which is the
URL every target speaker is pointed at via `SetAVTransportURI`.

### Display

`DisplayUI.cpp` drives a WeAct 2.9" black/white/red e-paper panel: a full
"Announcing: <text>" banner while a job is in flight, and a small idle
dashboard (populated via `sonosannouncer/dashboard/set`) the rest of the
time. WeAct has sold more than one controller chip under this exact product
name, so the top of `DisplayUI.cpp` has a comment block walking through how
to confirm the right `GxEPD2_290_*` class for your specific panel - this is
the one thing in the whole project that genuinely can't be inferred for you
and must be checked by hand before first flashing.

### MQTT topic/payload schema

**`sonosannouncer/announce/cmd`** - publish here to trigger an announcement.

```json
{"text": "Someone is at the front door", "volume": 40, "speakers": "all"}
```
```json
{"text": "Dinner's ready", "volume": 35, "speakers": ["Kitchen", "Living Room"]}
```

- `text` - required.
- `volume` - optional, 0-100. Defaults to `SONOS_DEFAULT_VOLUME` in `Config.h`.
- `speakers` - optional. `"all"` (or omit it) targets every speaker found at
  boot; otherwise pass an array of room names exactly as Sonos shows them
  (case-insensitive match).

**`sonosannouncer/announce/status`** - the ESP32 publishes progress here:
`{"state": "speaking"|"idle"|"error", "text": "...", "detail": "..."}`.

**`sonosannouncer/dashboard/set`** - push arbitrary key/value pairs to show on
the idle dashboard face: `{"key": "Outside Temp", "value": "14C"}`.

**`sonosannouncer/status`** - availability LWT (`online` / `offline`), retained.

### Debugging journey - real problems found and fixed

These were all discovered through actual day-to-day use, not anticipated in
advance, and each one left a comment at the relevant call site in the code so
the reasoning doesn't get lost later:

- **"All speakers" intermittently missed Garden.** Boot-time SSDP discovery
  is a one-shot snapshot; a speaker that was still asleep or reconnecting to
  WiFi at that exact moment was simply never seen, and - because at least one
  *other* speaker always resolved successfully - the existing "zero targets,
  try rediscovery" fallback in `startJob()` never triggered to catch it.
  **Fix:** a periodic rediscovery loop while idle (`SONOS_REDISCOVER_INTERVAL_MS`,
  every 10 minutes) that re-runs SSDP + topology refresh regardless of whether
  anything looked wrong, so a late-joining speaker is picked up within one
  cycle instead of staying invisible until the board next rebooted.
- **Living Room (a bonded/synced group) returned HTTP 500 on every
  announcement.** Sonos rejects direct transport control sent to anything
  other than a group's coordinator. The original implementation controlled
  speakers by their own discovered IP directly, which works fine for a
  standalone zone but fails for a bonded pair or multi-speaker group.
  **Fix:** the group-coordinator resolution described above
  (`sonosRefreshTopology()` / `sonosResolveTargets()`), including scanning
  nested `<Satellite>` tags so bonded pairs and home-theater setups resolve
  correctly too.
- **Announcements took anywhere from ~7 to ~22 seconds to actually start
  playing**, with no obvious pattern. The instant the first target receives
  its `Play` command, Sonos tries to fetch the clip back from this board's
  own `AudioServer` - but that fetch can only be serviced while
  `audioServerLoop()` gets a chance to run, and the original `doPlay()` issued
  every SOAP call back-to-back with no servicing in between, so Sonos's fetch
  sometimes had to wait out the entire remaining loop. **Fix:**
  `audioServerLoop()` is now called after every single target's `Play` call,
  keeping the web server responsive throughout instead of only once the
  whole phase finishes. Snapshotting was also trimmed from 5 SOAP calls to 4
  per speaker by reusing `GetPositionInfo`'s `TrackURI`/`TrackMetaData`
  instead of a separate call. Typical latency afterward: ~3 seconds.
- **A fast single speaker finishing early could cut off a slower bonded
  group mid-announcement.** The original completion check polled only the
  first target's transport state; once "all speakers" started correctly
  resolving to one call per group coordinator (see above), different targets
  could be genuinely different physical setups with very different playback
  start-up latency (a single standalone speaker vs. a 4-unit bonded/synced
  group) - and a fast one finishing would trigger `doRestore()` for *every*
  target, silencing a slower group before it had even started playing
  audibly. **Fix:** `doWaitFinish()` now polls every resolved target and only
  proceeds once all of them report a non-playing state (or the hard
  `SONOS_ANNOUNCE_MAX_WAIT_MS` ceiling is reached).
- **Main Bedroom discovery was intermittently flaky** - present at some boots,
  missing at others, with no code change in between. Diagnosed as ordinary
  SSDP/WiFi timing variance (the speaker occasionally didn't respond within
  the fixed `SONOS_SSDP_TIMEOUT_MS` window at that particular boot) rather
  than a logic bug - mitigated by the same periodic-rediscovery fix above,
  which means a missed speaker is only ever invisible for one rediscovery
  cycle rather than until the next reboot.
- **Kitchen and Main Bedroom occasionally log a failed `Seek` during
  restore.** `sonosRestore()` attempts a best-effort `Seek` back to the exact
  playback position a speaker was at before the announcement interrupted it.
  Some sources - internet radio streams, and some queue types - don't support
  seeking at all and will fault on this call. This is treated as non-fatal by
  design (the rest of the restore - URI, volume, mute, transport state - still
  completes), and the log line is expected/benign for those sources rather
  than a sign of a bug.

---

## Part 3 - Home Assistant dashboard integration

The firmware only needs one MQTT publish to trigger an announcement, but
hand-typing that JSON payload every time isn't something most people want to
do day-to-day. The `homeassistant/` folder in this repo has everything needed
to add a one-button "Sonos Announce" card to a Home Assistant dashboard:

| File | What it is |
|---|---|
| `helpers.yaml` | The `input_text`/`input_number`/`input_boolean` helper entities the card and script read from |
| `script_send_sonos_announcement.yaml` | `script.send_sonos_announcement` - builds and publishes the MQTT payload |
| `dashboard_section_sonos_announce.yaml` | The Lovelace "sections"-type dashboard card itself |

### How it works

The card exposes: a text field for the message, a number field for volume, an
"All Speakers" toggle, and one toggle per Sonos room, plus a "Send
Announcement" button. Pressing the button runs `script.send_sonos_announcement`,
which reads all of those helper states, builds the same JSON payload the
firmware expects (see Part 2's MQTT schema), and publishes it via the
`mqtt.publish` action - no direct Sonos integration needed on the HA side at
all, which keeps the "all control logic lives on the ESP32" design intact.

If "All Speakers" is on, the script ignores the individual room toggles and
sends `"speakers": "all"`. Otherwise it collects every room whose toggle is
on into an array (an empty selection sends an empty array, which the
firmware's `sonosResolveTargets()` resolves to zero targets and reports back
as an `error`/`no_speakers_found` status - so nothing plays, but nothing
crashes either).

**Usage note:** the toggles are ordinary `input_boolean` helpers, so they
persist their on/off state between uses - the card does not reset itself
after sending. If you leave "Kitchen" switched on from a previous
announcement, it stays on for the next one too. Turn off whichever toggles
you don't want included each time, or just leave "All Speakers" on if that's
your default use case.

### Installing it on your own Home Assistant

1. **Create the helpers.** Easiest via the UI: Settings -> Devices & Services
   -> Helpers -> "+ Create Helper", once each for a Text (`Announcement
   Message`), a Number (`Announcement Volume`, 0-100), and one Toggle per
   speaker/group you want individually selectable plus one more named
   something like `Announce All Speakers`. Or add the equivalent block from
   `helpers.yaml` to your `configuration.yaml` instead, if you prefer
   config-as-code - see the comments in that file for the exact entity-ID
   requirement (the toggle names must match what the script expects).
2. **Rename the room list to match your own household.** Open
   `script_send_sonos_announcement.yaml` and `helpers.yaml` and replace the
   example rooms (Garden, Kitchen, Living Room, Main Bedroom, Office,
   Charlotte's Bedroom) with your own Sonos zone names, exactly as the Sonos
   integration in HA shows them.
3. **Create the script.** Settings -> Automations & Scenes -> Scripts -> "+"
   -> use the "..." menu -> "Edit in YAML" -> paste in
   `script_send_sonos_announcement.yaml`'s content (minus the header comment)
   -> Save. Confirm it saves as `script.send_sonos_announcement` (or update
   the entity ID referenced in the dashboard card if you name it differently).
4. **Add the dashboard card.** This only works cleanly on a manually-authored
   "sections"-type Lovelace dashboard (Settings -> Dashboards -> your
   dashboard, where the three-dot menu offers "Raw configuration editor" -
   the newer auto-generated "Overview" strategy dashboard does not support
   arbitrary custom cards this way). Open that raw editor, find the
   `sections:` list under the view you want this on, and paste in the
   contents of `dashboard_section_sonos_announce.yaml` as one more list item,
   matching the indentation of the existing sibling sections exactly -
   Lovelace's YAML is whitespace-sensitive and will reject a bad indent.
5. **Test it.** Type a short message, pick a speaker (or leave "All
   Speakers" on), and press "Send Announcement". Check the script's trace
   (Settings -> Automations & Scenes -> Scripts -> Send Sonos Announcement ->
   the clock/trace icon) if nothing plays - it shows the exact MQTT payload
   that was published, which is the fastest way to tell whether the problem
   is on the HA side (wrong payload) or the firmware side (payload looks
   right but nothing happened - check the ESP32's Serial log and the
   `sonosannouncer/announce/status` topic).

---

## Part 4 - User guide

### 1. Libraries to install (Arduino IDE Library Manager)

| Library | Notes |
|---|---|
| PubSubClient (Nick O'Leary) | v2.8+ needed for `setKeepAlive()` |
| ArduinoJson (Benoit Blanchon) | v7.x - this sketch uses the v7 `JsonDocument` API |
| GxEPD2 (Jean-Marc Zingg) | e-paper driver |
| Adafruit GFX Library | GxEPD2 dependency |

Everything else (`WiFi`, `HTTPClient`, `WebServer`, `WiFiUdp`, `LittleFS`)
ships with the ESP32 Arduino core - just make sure you've added the ESP32
board package in Boards Manager and selected your board (e.g. "ESP32 Dev
Module") before compiling.

### 2. The one thing you must check yourself: the e-paper panel class

WeAct has sold more than one controller chip under the "2.9 inch B/W/R" name.
Open `DisplayUI.cpp` and read the comment block at the top - it walks through
how to confirm the right `GxEPD2_290_*` class for your exact board using the
part number printed on the panel and GxEPD2's own display-selection header.
The sketch ships with `GxEPD2_290_C90c` as a starting point; if the screen
shows nothing or garbage after flashing, this is the first thing to revisit.

### 3. Wiring

Default pins (edit in `Config.h` if you wired it differently):

| E-paper pin | ESP32 GPIO |
|---|---|
| CS | 5 |
| DC | 17 |
| RST | 16 |
| BUSY | 4 |
| CLK / SCK | 18 (VSPI default) |
| DIN / MOSI | 23 (VSPI default) |
| GND / VCC | GND / 3.3V |

### 4. Configure `Config.h`

Copy `Config.example.h` to `Config.h` and fill in your own values: WiFi
SSID/password, your MQTT broker's IP (e.g. the IP running HA's Mosquitto
add-on) and credentials if it has any, and `TTS_HOST`/`TTS_PORT` pointing at
your Piper TTS server (see section 7 below for how to stand one up on
Proxmox). **`Config.h` is git-ignored on purpose** - it holds your real WiFi
password and MQTT credentials and should never be committed; only the
placeholder `Config.example.h` is tracked in this repo.

### 5. Flashing - partition scheme

Flash usage on this project is high enough that the Arduino IDE's default
partition scheme doesn't leave enough headroom: the default "Default 4MB with
spiffs" scheme (two 1.25MB OTA app slots) hit 96% of the app partition before
the group-coordinator topology code was even added, and this sketch has no
OTA-update code, so the second app slot is pure dead weight. **Select Tools
-> Partition Scheme -> "No OTA (2MB APP/2MB SPIFFS)"** before compiling - it
trades the unused OTA slot for a single 2MB app partition (comfortable
headroom) and a 2MB LittleFS region (observed announcement WAVs run
180-280KB; at Piper's default 22kHz/16-bit mono output of about 44KB/second,
2MB is roughly a 45-second ceiling per announcement, well beyond what this is
meant for). If you ever add real OTA updates later, you'd need to switch back
to a dual-app-slot scheme and re-trim flash usage elsewhere instead.

RAM use is a healthy ~19% (62,560 bytes) baseline; the group-topology
additions are deliberately bounded (a short IP-length string per speaker, and
one multi-KB buffer that's fetched, parsed, and freed at discovery time only
- never held during an actual announcement) to avoid regressing that.

### 6. First boot checklist

1. Flash with WiFi/MQTT/`TTS_HOST` details filled in (from `Config.h`).
2. Watch the Serial monitor (115200 baud) - confirm WiFi connects, MQTT
   connects, and SSDP discovery logs the room names you expect.
3. Publish a test announce message to `sonosannouncer/announce/cmd` and watch
   the Serial log follow it through snapshot -> TTS fetch -> play -> wait ->
   restore.
4. If the display doesn't show the banner correctly, revisit the panel class
   note in `DisplayUI.cpp` before anything else.

### 7. Running the Piper TTS server (Proxmox LXC)

This firmware expects a Piper TTS HTTP server reachable at `TTS_HOST:TTS_PORT`
on your LAN. A small unprivileged LXC container (Debian 12, 2 vCPU, 1GB RAM,
8GB disk, DHCP networking) is enough:

```bash
apt update && apt install -y python3-venv python3-pip curl
mkdir -p /opt/piper/voices
python3 -m venv /opt/piper/venv
/opt/piper/venv/bin/pip install --upgrade pip
/opt/piper/venv/bin/pip install "piper-tts[http]"

cd /opt/piper/voices
/opt/piper/venv/bin/python3 -m piper.download_voices en_GB-alan-medium
# add more voices the same way if you want to compare, e.g. en_US-lessac-medium
```

Then run it as a systemd service so it survives reboots:

```bash
cat > /etc/systemd/system/piper-tts.service <<'EOF'
[Unit]
Description=Piper TTS HTTP server
After=network.target

[Service]
ExecStart=/opt/piper/venv/bin/python3 -m piper.http_server -m en_GB-alan-medium --data-dir /opt/piper/voices --host 0.0.0.0 --port 5000
Restart=on-failure
User=root

[Install]
WantedBy=multi-user.target
EOF

systemctl daemon-reload
systemctl enable --now piper-tts
```

Test it directly before pointing the ESP32 at it:

```bash
curl -s -X POST -H 'Content-Type: application/json' \
  -d '{"text":"Testing one two three"}' \
  -o /tmp/test.wav http://localhost:5000/synthesize
```

To switch voices without touching the firmware, pass `"voice": "<name>"` in
the JSON body of a request (any voice present in `--data-dir` can be
requested per-call), or change the service's `-m` flag and `systemctl restart
piper-tts` to change the default.

### 8. Setting up the Home Assistant dashboard card

See Part 3 above for the full walkthrough (helpers -> script -> dashboard
card -> test).

---

## Known limitations

- **Grouped/bonded speakers are handled via group-coordinator resolution**
  (see Part 2) - this depends on the household's topology being current in
  the ESP32's cache, which is refreshed automatically every
  `SONOS_REDISCOVER_INTERVAL_MS` (10 minutes by default) while idle.
- **Coarse-grained state machine, not fully non-blocking** - see Part 2 for
  why, and what a stricter task-per-core design would look like if you want
  to remove this simplification later.
- **TTS traffic is plain HTTP, not TLS** - fine as long as `TTS_HOST` stays on
  your own LAN and is never exposed to the internet; this is also what lets
  the sketch skip `WiFiClientSecure` entirely, freeing a meaningful chunk of
  flash and RAM.
- **SOAP/XML parsing uses simple substring extraction**, not a real XML
  parser, to save RAM - reliable against Sonos's actual responses, but if you
  ever hit a parsing issue, the fastest way to debug it is firing the same
  SOAP request from a computer (`curl` against `http://<speaker-ip>:1400/...`)
  and comparing the response tags against what `extractTag`/`extractTagRaw`
  in `SonosControl.cpp` expect.
- **Sonos's local SOAP/UPnP interface is undocumented/unofficial** - stable
  for years and used by every major third-party Sonos tool, but Sonos could
  change it in a future firmware update without notice; there's no
  vendor-supported alternative for this kind of direct local control today.
- **LittleFS write wear** - an announcement writes one small WAV file to
  flash each time. Fine for typical use; avoid driving this continuously at
  very high frequency (multiple times a minute, for long periods).
- **Dashboard toggles persist state between uses** - see Part 3's usage note.

#include "DisplayUI.h"
#include "Config.h"

// -----------------------------------------------------------------------------
// READ THIS BEFORE FLASHING: WeAct has shipped more than one controller chip
// under the "2.9 inch B/W/R" name over the years (SSD1680-family and others).
// Picking the wrong GxEPD2 class below will compile and upload fine but show
// nothing, garbage, or only two colours on screen. To get the right one:
//   1. Look at the part/model number printed on your panel's flex cable or
//      the small PCB it's soldered to.
//   2. Open GxEPD2's own "GxEPD2_display_selection_new_style.h" (ships with
//      the library's examples) and find the matching entry in its "3-colour
//      GDEY/GDEW" section.
//   3. Cross-reference against WeAct Studio's own product wiki/page for your
//      exact SKU if the model number is ambiguous.
// GxEPD2_290_C90c below is the commonly-cited match for WeAct's 2.9" B/W/R
// module at the time this was written - treat it as a starting point to
// verify, not a guarantee for your specific board revision.
// -----------------------------------------------------------------------------
#include <GxEPD2_3C.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>

#define PANEL_CLASS GxEPD2_290_C90c   // <-- confirm this against your board, see note above

static GxEPD2_3C<PANEL_CLASS, PANEL_CLASS::HEIGHT> display(
    PANEL_CLASS(EPD_CS_PIN, EPD_DC_PIN, EPD_RST_PIN, EPD_BUSY_PIN));

static const int MAX_SENSOR_ROWS = 8;
static String sensorKeys[MAX_SENSOR_ROWS];
static String sensorValues[MAX_SENSOR_ROWS];
static int sensorCount = 0;

// A tri-colour full refresh blocks for ~15-30s. To keep that off the main loop
// (which drives the Sonos/TTS/audio-server work), all drawing happens on a
// dedicated task; the public displayShow*() calls just post a request and
// return. Latest request wins, so a quick announce -> dashboard sequence
// skips the intermediate screen rather than queueing two slow refreshes.
enum DisplayRequest { REQ_NONE, REQ_ANNOUNCING, REQ_DASHBOARD };

static SemaphoreHandle_t stateMutex = nullptr;
static TaskHandle_t displayTask = nullptr;
static DisplayRequest pendingRequest = REQ_NONE;
static String pendingText;

static void renderAnnouncing(const String& text) {
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.fillRect(0, 0, display.width(), 28, GxEPD_RED);
    display.setFont(&FreeSansBold12pt7b);
    display.setTextColor(GxEPD_WHITE);
    display.setCursor(6, 20);
    display.print("Announcing");

    display.setFont(&FreeSans9pt7b);
    display.setTextColor(GxEPD_BLACK);

    // Simple manual word-wrap into the remaining screen area.
    int16_t x1, y1;
    uint16_t w, h;
    int cursorY = 50;
    String line = "";
    for (unsigned int i = 0; i <= text.length(); i++) {
      char c = (i < text.length()) ? text[i] : '\0';
      String candidate = line + (c ? String(c) : "");
      display.getTextBounds(candidate, 0, 0, &x1, &y1, &w, &h);
      if (w > (uint16_t)(display.width() - 12) || c == '\0') {
        display.setCursor(6, cursorY);
        display.print(line);
        cursorY += 18;
        line = c ? String(c) : "";
        if (cursorY > display.height() - 10) break;
      } else {
        line = candidate;
      }
    }
  } while (display.nextPage());
}

void displaySetSensorValue(const String& key, const String& value) {
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  for (int i = 0; i < sensorCount; i++) {
    if (sensorKeys[i] == key) {
      sensorValues[i] = value;
      xSemaphoreGive(stateMutex);
      return;
    }
  }
  if (sensorCount < MAX_SENSOR_ROWS) {
    sensorKeys[sensorCount] = key;
    sensorValues[sensorCount] = value;
    sensorCount++;
  }
  xSemaphoreGive(stateMutex);
}

static void renderDashboard() {
  // Copy the rows under the lock so MQTT updates can't change them mid-refresh
  // (the draw loop below runs once per display page).
  String keys[MAX_SENSOR_ROWS], values[MAX_SENSOR_ROWS];
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  int count = sensorCount;
  for (int i = 0; i < count; i++) {
    keys[i] = sensorKeys[i];
    values[i] = sensorValues[i];
  }
  xSemaphoreGive(stateMutex);

  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setFont(&FreeSansBold12pt7b);
    display.setTextColor(GxEPD_BLACK);
    display.setCursor(6, 20);
    display.print("Status");

    display.setFont(&FreeSans9pt7b);
    int y = 50;
    for (int i = 0; i < count; i++) {
      display.setCursor(6, y);
      display.print(keys[i] + ": " + values[i]);
      y += 18;
    }
    if (count == 0) {
      display.setCursor(6, y);
      display.print("(no sensor data yet)");
    }
  } while (display.nextPage());
}

static void displayTaskFn(void*) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    xSemaphoreTake(stateMutex, portMAX_DELAY);
    DisplayRequest req = pendingRequest;
    String text = pendingText;
    pendingRequest = REQ_NONE;
    xSemaphoreGive(stateMutex);

    if (req == REQ_ANNOUNCING) renderAnnouncing(text);
    else if (req == REQ_DASHBOARD) renderDashboard();
  }
}

static void postRequest(DisplayRequest req, const String& text) {
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  pendingRequest = req;
  pendingText = text;
  xSemaphoreGive(stateMutex);
  xTaskNotifyGive(displayTask);
}

void displaySetup() {
  display.init(115200);
  display.setRotation(1);
  display.setTextColor(GxEPD_BLACK);

  stateMutex = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(displayTaskFn, "display", 8192, nullptr, 1, &displayTask, 0);
}

void displayShowAnnouncing(const String& text) { postRequest(REQ_ANNOUNCING, text); }

void displayShowDashboard() { postRequest(REQ_DASHBOARD, ""); }

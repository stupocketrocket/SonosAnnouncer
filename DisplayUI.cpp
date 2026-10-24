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

void displaySetup() {
  display.init(115200);
  display.setRotation(1);
  display.setTextColor(GxEPD_BLACK);
}

void displayShowAnnouncing(const String& text) {
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
  for (int i = 0; i < sensorCount; i++) {
    if (sensorKeys[i] == key) {
      sensorValues[i] = value;
      return;
    }
  }
  if (sensorCount < MAX_SENSOR_ROWS) {
    sensorKeys[sensorCount] = key;
    sensorValues[sensorCount] = value;
    sensorCount++;
  }
}

void displayShowDashboard() {
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
    for (int i = 0; i < sensorCount; i++) {
      display.setCursor(6, y);
      display.print(sensorKeys[i] + ": " + sensorValues[i]);
      y += 18;
    }
    if (sensorCount == 0) {
      display.setCursor(6, y);
      display.print("(no sensor data yet)");
    }
  } while (display.nextPage());
}

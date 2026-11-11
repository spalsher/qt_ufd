// V11: see README.md and API.md for the new layout and authenticated API.
// Legacy V10 notes below describe compatibility routes only.
// ======================================================================
//  TOLL DISPLAY (UFD) V10 OLD UFD  -  ESP32 + W5500 + HUB75 panels (OLD panel type,
//                         ESP32-HUB75-MatrixPanel-DMA library)
//                         P5 64x32 1/8 scan, 3 panels STACKED vertically
//                         Screen: 64 x 96
//
//  Core 0 : Ethernet, HTTP server, picture downloads
//  Panels : refreshed by the I2S DMA hardware (no CPU needed)
//
//  Libraries : Adafruit GFX Library (+ Adafruit BusIO),
//              ESP32 HUB75 LED MATRIX PANEL DMA Display (mrcodetastic)
//  Files     : TOLL_DISPLAY.ino, UfdPanel.h, LedFont.h, PlateFont.h, UfdPng.h,
//              BuiltinPics.h - all in the same folder
//  Board     : ESP32 Dev Module, Partition Scheme "Default 4MB with spiffs"
//
//  SCREENS
//    Toll commands : picture on panels 1+2, live text on panel 3
//    Idle          : idle pictures on all 3 panels, one after another
//
//  PICTURES
//    Every picture is built into the firmware (factory default) and can be
//    replaced from the picture server (folder set on /config). The UFD
//    downloads changed pictures in the background, checks them, and keeps
//    them, so the screen never depends on the server or the network.
//    Server files: takeslip.png thankyou.png laneopen.png laneclosed.png (64 x 64)
//                  payfare.png, metag.png (64 x 32, panel 1)
//                  idle1.png ... idle6.png (64 x 96)
//    saved with UFD Image Studio -> "Save for UFD server"
//
//  Toll commands:
//    /takeslip  /thankyou  /open  /closed
//    /?vehicle_number=..&take_slip          /?vehicle_number=..&fare_amount=..
//    ME-TAG paid  : /?vehicle_number=..&metag_paid=100&balance=4200
//    ME-TAG low   : /?vehicle_number=..&metag_low
//                   (ME-TAG logo on panel 1, vehicle number on panel 2, message on panel 3)
//  The vehicle number is drawn live as a Sindh-style number plate
//  (Ajrak band, black letters on white) on the pay toll, ME-TAG and take slip screens.
//  Pay toll: PAY TOLL picture on panel 1, number plate on panel 2, fare on panel 3.
//    /display?line1=..&line2=..&color=red   (plain text)
//  Other pages:
//    /idle  /refresh  /status  /config  /panel  /color?c=red
// ======================================================================

// ======================================================
// UFD DEFAULT SETTINGS (used on a new ESP32 or after a settings reset)
// ======================================================
#define PANEL_COUNT      3       // number of stacked panels
#define DEFAULT_PATTERN  0       // FM6373 only - not used by the old panels
#define DEFAULT_REGS     0       // FM6373 only - not used by the old panels
#define DEFAULT_REV      true    // true = bottom panel first in the chain
#define DEFAULT_BRIGHT   255     // 255 = 100%, 128 = 50%, 64 = 25%, 32 = 12%

// Picture server (can be changed on the /config page)
#define DEFAULT_PIC_SERVER  "http://192.168.78.200/mewuploads/ufd/"
#define DEFAULT_IDLE_SEC    3      // each idle picture stays this long
#define DEFAULT_RETURN_SEC  10     // after a toll screen, back to idle after this long
#define SYNC_MINUTES        5      // check the server for changed pictures
#define FARE_PREFIX         "RS "  // shown before the fare on panel 3

// FM6373 only - not used by the old panels (kept for the saved settings)
#define FM6373_DMA_CLK_INVERT 0  // clock edge: 0 = "B" (works), 1 = "A"
#define FM6373_DMA_MHZ        10 // clock speed: 5, 8 or 10 MHz (10 works)

// Network: Ethernet (W5500) only - no Wi-Fi, so all RAM goes to the panels

#include <ETH.h>
#include <SPI.h>
#include <NetworkClient.h>
#include <NetworkServer.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <Adafruit_GFX.h>
#include <HTTPClient.h>
#include "esp_heap_caps.h"
#include "UfdPanel.h"
#include "LedFont.h"
#include "PlateFont.h"
#include "UfdPng.h"
#include "BuiltinPics.h"
#include "UfdConfig.h"
#include "DisplayPolicy.h"
#include "esp_system.h"
#include "esp_task_wdt.h"

// ======================================================
// W5500 PINS
// ======================================================
#define W5500_CS    21
#define W5500_SCK   18
#define W5500_MISO  19
#define W5500_MOSI  23
#define W5500_INT   -1
#define W5500_RST   -1

// ======================================================
// DEFAULT ETHERNET SETTINGS
// ======================================================
#define DEFAULT_DHCP  false     // static IP (true = address from the network)
#define DEFAULT_IP    "192.168.78.12"
#define DEFAULT_GW    "192.168.78.1"
#define DEFAULT_SN    "255.255.254.0"
#define DEFAULT_DNS   "8.8.8.8"

// ======================================================
// HUB75 PANEL PINS
// ======================================================
#define R1_PIN 33
#define G1_PIN 26
#define B1_PIN 25
#define R2_PIN 4
#define G2_PIN 27
#define B2_PIN 16

#define A_PIN 17
#define B_PIN 14
#define C_PIN 5

#define LAT_PIN 12
#define OE_PIN 13
#define CLK_PIN 22

// ======================================================
// SCREEN SIZE (calculated, do not change)
// ======================================================
#define PANEL_W   64
#define PANEL_H   32
#define SCREEN_W  PANEL_W                    // 64
#define SCREEN_H  (PANEL_H * PANEL_COUNT)    // 96

// ======================================================
// TEXT SETTINGS (two lines around the middle of the screen)
// ======================================================
#define CHAR_W        6
#define LINE1_Y       (SCREEN_H / 2 - 8)
#define LINE2_Y       (SCREEN_H / 2 + 6)
#define SCROLL_MS     40
#define SCROLL_GAP    20
#define MAX_TEXT_LEN  100

// ======================================================
// STACKED SCREEN
// Draws on a 64 x (32*N) screen and sends every pixel to the right
// panel in the chain. Panels can be turned upside down (flip).
// ======================================================
class StackedScreen : public Adafruit_GFX
{
public:
  StackedScreen(UfdPanel *drv, uint8_t count)
    : Adafruit_GFX(PANEL_W, PANEL_H * count), _drv(drv), _count(count) {}

  void drawPixel(int16_t x, int16_t y, uint16_t color) override
  {
    if (x < 0 || y < 0 || x >= PANEL_W || y >= PANEL_H * _count) return;
    uint8_t k  = y / PANEL_H;               // which panel (0 = top)
    int16_t lx = x;
    int16_t ly = y % PANEL_H;
    if (_flip & (1 << k))                   // panel mounted upside down
    {
      lx = PANEL_W - 1 - lx;
      ly = PANEL_H - 1 - ly;
    }
    _drv->drawPixel(k * PANEL_W + lx, ly, color);
  }

  void fillScreen(uint16_t color) override { _drv->fillScreen(color); }
  void clearScreen()                       { _drv->fillScreen(0); }

  void    setFlipMask(uint8_t m) { _flip = m; }
  uint8_t getFlipMask() const    { return _flip; }

private:
  UfdPanel *_drv;
  uint8_t _count;
  uint8_t _flip = 0;
};

UfdPanel  *panels = nullptr;   // low-level driver (whole chain)
StackedScreen *scr    = nullptr;   // draw everything here
GFXcanvas16   *lineCanvas = nullptr;

uint16_t myRED = 0, myGREEN = 0, myBLUE = 0;

// ======================================================
// STATE
// ======================================================
NetworkServer serverHTTP(80);
Preferences prefs;

volatile bool ethernetConnected = false;
volatile bool ethernetStopped = false;
volatile bool linkDetected      = false;

bool   cfgDHCP;
String cfgIP, cfgGW, cfgSN, cfgDNS;

uint8_t panelPattern = DEFAULT_PATTERN;
uint8_t panelRegs    = DEFAULT_REGS;
bool    panelRev     = DEFAULT_REV;    // true: bottom panel is first in the chain
uint8_t panelFlip    = 0;              // bit k set: panel k is upside down
uint8_t panelBright  = DEFAULT_BRIGHT;
bool    panelClkInv  = FM6373_DMA_CLK_INVERT;  // DMA clock edge
uint8_t panelMhz     = FM6373_DMA_MHZ;         // DMA clock speed

struct TextLine
{
  String   text;
  uint16_t color;
  int      y;
  int      textW;
  int      offset;
  bool     scroll;
};

TextLine lines[2];
bool scrollActive = false;
unsigned long lastScroll = 0;

bool     fsOK        = false;
#define  IO_BYTES 1024                // small buffer for files and uploads
static uint8_t ioBuf[IO_BYTES];

// ---------------- server pictures ----------------
struct ServerPic
{
  const char *name;      // file on the server: <name>.png
  uint8_t     h;         // needed height (width is always 64)
  bool        ok;        // a good server copy is stored on the UFD
  bool        hidden;    // idle picture removed on the server: not shown
  String      state;     // last result, shown on /status
};
ServerPic pics[] = {
  { "takeslip",   64 }, { "payfare",  32 }, { "thankyou", 64 },
  { "laneopen",   64 }, { "laneclosed", 64 }, { "metag", 32 },
  { "idle1", 96 }, { "idle2", 96 }, { "idle3", 96 },
  { "idle4", 96 }, { "idle5", 96 }, { "idle6", 96 },
};
const int PIC_COUNT = sizeof(pics) / sizeof(pics[0]);

String   picServer = DEFAULT_PIC_SERVER;
uint16_t idleSec   = DEFAULT_IDLE_SEC;
uint16_t returnSec = DEFAULT_RETURN_SEC;

volatile bool syncNow  = true;     // download as soon as Ethernet is up
unsigned long lastSync = 0;
String        lastSyncText = "not yet";
SemaphoreHandle_t picLock = nullptr;  // protects the status texts between the two tasks

// what is on the screen
bool     idleActive   = false;     // idle pictures are cycling
int      idleIndex    = 0;
unsigned long idleLast = 0;
bool     tollScreen   = false;     // a toll picture is shown (returns to idle)
unsigned long tollSince = 0;
DisplayPolicy displayPolicy;
String activePlate, activeFare, activeBalance, activePassage;
String bootId, lastPayload;
bool displayReady = false;


// ======================================================
// CONFIG STORAGE
// ======================================================
void loadConfig()
{
  prefs.begin("net", true);
  cfgDHCP = prefs.getBool("dhcp", DEFAULT_DHCP);
  cfgIP   = prefs.getString("ip",  DEFAULT_IP);
  cfgGW   = prefs.getString("gw",  DEFAULT_GW);
  cfgSN   = prefs.getString("sn",  DEFAULT_SN);
  cfgDNS  = prefs.getString("dns", DEFAULT_DNS);
  prefs.end();

  prefs.begin("panel", true);
  panelPattern = prefs.getUChar("pattern", DEFAULT_PATTERN);
  panelRegs    = prefs.getUChar("regs",    DEFAULT_REGS);
  panelRev     = prefs.getBool("rev",      DEFAULT_REV);
  panelFlip    = prefs.getUChar("flip",    0);
  panelBright  = prefs.getUChar("bright",  DEFAULT_BRIGHT);
  panelClkInv  = prefs.getBool("clkinv",   FM6373_DMA_CLK_INVERT);
  panelMhz     = prefs.getUChar("mhz",     FM6373_DMA_MHZ);
  prefs.end();
}

void saveConfig()
{
  prefs.begin("net", false);
  prefs.putBool("dhcp", cfgDHCP);
  prefs.putString("ip",  cfgIP);
  prefs.putString("gw",  cfgGW);
  prefs.putString("sn",  cfgSN);
  prefs.putString("dns", cfgDNS);
  prefs.end();
}

void savePanelConfig()
{
  prefs.begin("panel", false);
  prefs.putUChar("pattern", panelPattern);
  prefs.putUChar("regs", panelRegs);
  prefs.putBool("rev", panelRev);
  prefs.putUChar("flip", panelFlip);
  prefs.putUChar("bright", panelBright);
  prefs.putBool("clkinv", panelClkInv);
  prefs.putUChar("mhz", panelMhz);
  prefs.end();
}

void loadPicConfig()
{
  prefs.begin("pics", true);
  picServer = prefs.getString("url", DEFAULT_PIC_SERVER);
  idleSec   = prefs.getUShort("idle", DEFAULT_IDLE_SEC);
  returnSec = prefs.getUShort("ret",  DEFAULT_RETURN_SEC);
  prefs.end();
  if (!picServer.endsWith("/")) picServer += "/";
}

void savePicConfig()
{
  prefs.begin("pics", false);
  prefs.putString("url", picServer);
  prefs.putUShort("idle", idleSec);
  prefs.putUShort("ret", returnSec);
  prefs.end();
}

void resetConfig()
{
  prefs.begin("net", false);
  prefs.clear();
  prefs.end();
}

bool validIP(const String &s)
{
  IPAddress t;
  return t.fromString(s);
}

// Files from older versions (uploaded frames) - removed once to free storage
void deleteOldFiles()
{
  for (int i = 0; i < 300; i++)
  {
    String p = "/f" + String(i) + ".bin";
    if (LittleFS.exists(p)) LittleFS.remove(p);
  }
  for (int z = 0; z < 4; z++)
  {
    String p = "/z" + String(z) + ".bin";
    if (LittleFS.exists(p)) LittleFS.remove(p);
    for (int i = 0; i < 30; i++)
    {
      p = "/z" + String(z) + "_" + String(i) + ".bin";
      if (LittleFS.exists(p)) LittleFS.remove(p);
    }
  }
  prefs.begin("zones", false); prefs.clear(); prefs.end();
}

// ======================================================
// CHIP CHECK
// ======================================================
uint8_t readW5500Version()
{
  SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
  digitalWrite(W5500_CS, LOW);
  SPI.transfer(0x00);
  SPI.transfer(0x39);
  SPI.transfer(0x00);
  uint8_t v = SPI.transfer(0x00);
  digitalWrite(W5500_CS, HIGH);
  SPI.endTransaction();
  return v;
}

// ======================================================
// NETWORK EVENTS
// ======================================================
void onNetworkEvent(arduino_event_id_t event, arduino_event_info_t info)
{
  switch (event)
  {
    case ARDUINO_EVENT_ETH_START:
      ethernetStopped = false;
      ETH.setHostname("ESP32-W5500");
      Serial.println("ETH STARTED");
      break;
    case ARDUINO_EVENT_ETH_CONNECTED:
      linkDetected = true;
      Serial.println("ETH CONNECTED (link detected)");
      break;
    case ARDUINO_EVENT_ETH_GOT_IP:
      ethernetConnected = true;
      Serial.print("ETH GOT IP: ");
      Serial.println(ETH.localIP());
      break;
    case ARDUINO_EVENT_ETH_LOST_IP:
      ethernetConnected = false;
      Serial.println("ETH LOST IP");
      break;
    case ARDUINO_EVENT_ETH_DISCONNECTED:
    case ARDUINO_EVENT_ETH_STOP:
      if(event == ARDUINO_EVENT_ETH_STOP) ethernetStopped = true;
      linkDetected = false;
      ethernetConnected = false;
      Serial.println("ETH DISCONNECTED/STOPPED");
      break;
    default:
      break;
  }
}

// ======================================================
// SERVER PICTURES: download, convert, store
// Every picture is stored on the UFD as /pic_<name>.bin (RGB565), so it can
// be shown instantly and still works when the server is not reachable.
// ======================================================
String picPath(const char *name) { return String("/pic_") + name + ".bin"; }

// Network -> PNG reader, with a timeout
class NetSource : public UfdPngSource
{
public:
  NetworkClient *c; int left;
  uint32_t started = millis();
  size_t received = 0;
  int read(uint8_t *b, int n) override
  {
    if ((uint32_t)(millis()-started) >= 20000 || received >= 65536) return 0;
    if ((size_t)n > 65536-received) n = 65536-received;
    if (left >= 0 && n > left) n = left;
    if (n <= 0) return 0;
    unsigned long t = millis();
    while (millis() - t < 5000 && (uint32_t)(millis()-started) < 20000)
    {
      int r = c->read(b, n);
      if (r > 0) { received += r; if (left >= 0) left -= r; return r; }
      if (!c->connected() && !c->available()) return 0;
      vTaskDelay(2);
    }
    return 0;
  }
};

// PNG reader -> temporary file, one row of 64 pixels at a time
class FileSink : public UfdPngSink
{
public:
  File *f;
  size_t written = 0;                 // counted here: File::size() is not reliable while writing
  bool row(int, const uint16_t *px) override
  {
    size_t n = f->write((const uint8_t *)px, 128);
    written += n;
    return n == 128;
  }
};

// Download one picture if it changed on the server. Returns a short result text.
String syncPicture(ServerPic &p)
{
  xSemaphoreTake(picLock, portMAX_DELAY);
  String url = picServer + p.name + ".png";
  xSemaphoreGive(picLock);
  String etagKey = String("e_") + p.name;

  Preferences pp;                       // own object: this runs in the background task
  pp.begin("pics", true);
  String oldTag = pp.getString(etagKey.c_str(), "");
  pp.end();
  bool haveCopy = LittleFS.exists(picPath(p.name));

  HTTPClient http;
  http.setTimeout(4000);
  http.setConnectTimeout(3000);
  if (!http.begin(url)) return "bad server address";
  const char *keys[] = { "ETag", "Last-Modified" };
  http.collectHeaders(keys, 2);
  if (haveCopy && oldTag.length()) http.addHeader("If-None-Match", oldTag);

  int code = http.GET();
  if (code == 304) { http.end(); return "up to date"; }
  if (code == 404)
  {
    http.end();
    if (haveCopy) LittleFS.remove(picPath(p.name));   // removed on the server
    p.ok = false;
    bool idle = !strncmp(p.name, "idle", 4);
    if (idle && !p.hidden)
    {
      Preferences pp2; pp2.begin("pics", false); pp2.putBool((String("h_") + p.name).c_str(), true); pp2.end();
      p.hidden = true;
    }
    return idle ? "not on server (not shown)" : "not on server (built-in picture used)";
  }
  if (code != 200) { http.end(); return code < 0 ? "server not reachable" : "server error " + String(code); }

  int len = http.getSize();                     // -1 if the server does not say
  if(len > 65536) { http.end(); return "picture too large"; }
  String newTag = http.header("ETag");
  if (!newTag.length()) newTag = http.header("Last-Modified");

  // read, check and convert while downloading, into a temporary file
  String tmp = String("/pic_") + p.name + ".tmp";
  File f = LittleFS.open(tmp, "w");
  if (!f) { http.end(); return "storage error (cannot create file)"; }
  NetSource src; src.c = http.getStreamPtr(); src.left = len;
  FileSink sink; sink.f = &f;
  UfdPngReader rd;
  int r = rd.read(src, p.h, sink);
  size_t need = (size_t)PANEL_W * p.h * 2;
  bool good = (r == UFDPNG_OK) && sink.written == need;
  f.close();
  http.end();

  if (!good)
  {
    LittleFS.remove(tmp);                       // the working picture stays as it is
    if (r == UFDPNG_BAD_SIZE)
      return "wrong size " + String(rd.width) + "x" + String(rd.height) + " (needs 64x" + String(p.h) + ")";
    if (r == UFDPNG_OK) return "storage error (wrote " + String(sink.written) + " of " + String(need) + " bytes)";
    if (r == UFDPNG_WRITE) return "storage error (storage full?)";
    return ufdPngText(r);
  }
  // LittleFS/VFS rename replaces the destination: do not delete the working copy first.
  if (!LittleFS.rename(tmp, picPath(p.name))) { LittleFS.remove(tmp); return "storage error (cannot rename)"; }

  Preferences pp2;
  pp2.begin("pics", false);
  pp2.putString(etagKey.c_str(), newTag);
  pp2.putBool((String("h_") + p.name).c_str(), false);
  pp2.end();
  p.ok = true;
  p.hidden = false;
  return "updated";
}

void syncAll()
{
  int updated = 0, failed = 0;
  for (int i = 0; i < PIC_COUNT; i++)
  {
    String r = syncPicture(pics[i]);
    xSemaphoreTake(picLock, portMAX_DELAY);
    pics[i].state = r;
    xSemaphoreGive(picLock);
    if (r == "updated") updated++;
    else if (r != "up to date" && !r.startsWith("not on server")) failed++;
    Serial.printf("Picture %-10s: %s\n", pics[i].name, r.c_str());
  }
  xSemaphoreTake(picLock, portMAX_DELAY);
  lastSync = millis();
  lastSyncText = String(updated) + " updated, " + String(failed) + " problems";
  xSemaphoreGive(picLock);
}

// Background task: checks the server at start, every few minutes and on /refresh
void syncTask(void *)
{
  for (;;)
  {
    bool due = syncNow || (lastSync && millis() - lastSync > SYNC_MINUTES * 60000UL);
    if (ethernetConnected && fsOK && due)
    {
      syncNow = false;
      syncAll();
    }
    vTaskDelay(pdMS_TO_TICKS(500));
  }
}

int findPic(const char *name)
{
  for (int i = 0; i < PIC_COUNT; i++) if (!strcmp(pics[i].name, name)) return i;
  return -1;
}

const BuiltinPic *builtinPic(const char *name)
{
  for (int i = 0; i < BUILTIN_COUNT; i++) if (!strcmp(BUILTIN_PICS[i].name, name)) return &BUILTIN_PICS[i];
  return nullptr;
}

// Built-in picture: runs of (count, colour) from flash
bool drawBuiltin(const char *name, int top, int rows)
{
  const BuiltinPic *b = builtinPic(name);
  if (!b) return false;
  int n = 0, limit = PANEL_W * rows;
  for (int k = 0; k < b->count && n < limit; k++)
  {
    uint16_t cnt = pgm_read_word(b->runs + 2 * k), col = pgm_read_word(b->runs + 2 * k + 1);
    for (uint16_t j = 0; j < cnt && n < limit; j++, n++)
      scr->drawPixel(n % PANEL_W, top + n / PANEL_W, col);
  }
  return true;
}

// Server copy if there is one, otherwise the built-in picture
bool drawPic(const char *name, int top, int rows)
{
  int i = findPic(name);
  if (i >= 0 && pics[i].ok)
  {
    File f = LittleFS.open(picPath(name), "r");
    if (f && (int)f.size() >= PANEL_W * rows * 2)
    {
      const int rowBytes = PANEL_W * 2, per = IO_BYTES / rowBytes;
      bool good = true;
      for (int y = 0; y < rows && good; y += per)
      {
        int n = (rows - y < per) ? rows - y : per;
        good = (int)f.read(ioBuf, n * rowBytes) == n * rowBytes;
        if (good) scr->drawRGBBitmap(0, top + y, (uint16_t *)ioBuf, PANEL_W, n);
      }
      f.close();
      if (good) return true;
    }
    else if (f) f.close();
  }
  return drawBuiltin(name, top, rows);
}

// ---------------- sharp LED text (panel 3) ----------------
int ledCharIndex(char c)
{
  const char *p = strchr(LED_FONT_CHARS, toupper(c));
  return (p && c) ? (int)(p - LED_FONT_CHARS) : 0;     // unknown -> space
}

int ledTextWidth(const LedFont &f, const String &s)
{
  int w = 0;
  for (unsigned i = 0; i < s.length(); i++) w += f.widths[ledCharIndex(s[i])] + f.gap;
  return s.length() ? w - f.gap : 0;
}

// Biggest font in which the text fits into maxW LEDs
const LedFont &fitFont(const String &s, int maxW)
{
  if (ledTextWidth(FONT_L, s) <= maxW) return FONT_L;
  if (ledTextWidth(FONT_M, s) <= maxW) return FONT_M;
  return FONT_S;
}

void ledText(const LedFont &f, const String &s, int cx, int y, uint16_t color)
{
  int x = cx - ledTextWidth(f, s) / 2;
  for (unsigned i = 0; i < s.length(); i++)
  {
    int k = ledCharIndex(s[i]), w = f.widths[k];
    const uint16_t *rows = f.rows + f.offsets[k];
    for (int r = 0; r < f.h; r++)
      for (int c = 0; c < w; c++)
        if (rows[r] & (1 << (w - 1 - c))) scr->drawPixel(x + c, y + r, color);
    x += w + f.gap;
  }
}

// Panel 3 (rows 64..95): vehicle number in a plate frame
// ---------------- Sindh-style number plate (one panel, drawn live) ----------------
int plateCharIndex(char c)
{
  const char *p = strchr(PLATE_CHARS, toupper(c));
  return (p && c) ? (int)(p - PLATE_CHARS) : 0;          // unknown -> space
}

int plateTextWidth(const String &v, int gap)
{
  int w = 0;
  for (unsigned i = 0; i < v.length(); i++) w += PLATE_W[plateCharIndex(v[i])] + gap;
  return v.length() ? w - gap : 0;
}

// Text in the plate font (18 LEDs tall), centred at cx. Letter gap 2 if the text then
// fits gapLimit, else 1. false (nothing drawn) if it does not fit maxW.
bool plateText(String v, int cx, int top, int gapLimit, int maxW, uint16_t color)
{
  v.toUpperCase();
  int gap = plateTextWidth(v, 2) <= gapLimit ? 2 : 1;
  int tw  = plateTextWidth(v, gap);
  if (tw > maxW) return false;
  int x = cx - tw / 2;
  for (unsigned i = 0; i < v.length(); i++)
  {
    int k = plateCharIndex(v[i]), w = PLATE_W[k];
    const uint8_t *rows = PLATE_ROWS + k * PLATE_H;
    for (int r = 0; r < PLATE_H; r++)
      for (int c = 0; c < w; c++)
        if (rows[r] & (1 << (w - 1 - c))) scr->drawPixel(x + c, top + r, color);
    x += w + gap;
  }
  return true;
}

// Ajrak band on top, black letters on a white plate: the panel that starts at row 'top'
void panelPlate(const String &vehicle, int top)
{
  String v = vehicle; v.toUpperCase(); v.trim();
  if (!v.length()) return;
  static const char *AJRAK[7] = { "RRRWWRRR", "RRWKKWRR", "RWKWWKWR", "WKWRRWKW",
                                  "RWKWWKWR", "RRWKKWRR", "RRRWWRRR" };
  const uint16_t WHITE = UfdPanel::color565(235, 235, 235);
  const uint16_t RED   = UfdPanel::color565(190, 0, 20);
  const uint16_t BLACK = 0;

  scr->fillRect(0, top, SCREEN_W, 32, WHITE);                 // white plate, edge to edge
  for (int y = 0; y < 7; y++)                                  // Ajrak band
    for (int x = 0; x < SCREEN_W; x++)
    {
      char c = AJRAK[y][x % 8];
      scr->drawPixel(x, top + y, c == 'R' ? RED : c == 'W' ? WHITE : BLACK);
    }
  scr->drawFastHLine(0, top + 7, SCREEN_W, BLACK);             // line under the band
  scr->drawPixel(0, top, BLACK);           scr->drawPixel(SCREEN_W - 1, top, BLACK);       // soft corners
  scr->drawPixel(0, top + 31, BLACK);      scr->drawPixel(SCREEN_W - 1, top + 31, BLACK);

  int y = top + 8 + (24 - PLATE_H) / 2;
  if (v.indexOf('.') >= 0 || !plateText(v, SCREEN_W / 2, y, 58, 62, BLACK))             // very long number: smaller font
  {
    const LedFont &f = fitFont(v, 62);
    ledText(f, v, SCREEN_W / 2, top + 8 + (24 - f.h) / 2, BLACK);
  }
}

void panel3Vehicle(const String &vehicle)
{
  if (PANEL_COUNT < 3) return;
  panelPlate(vehicle, 64);
}

// Small text line, centred; "RS " is dropped if the line would not fit
void smallLine(String s, int y, uint16_t color)
{
  s.toUpperCase();
  if (ledTextWidth(FONT_S, s) > 62) s.replace("RS ", "");
  ledText(FONT_S, s, SCREEN_W / 2, y, color);
}

// Panel 3: vehicle number (small) and the fare (big)
void panel3Fare(const String &vehicle, const String &fare)
{
  if (PANEL_COUNT < 3) return;
  String v = vehicle; v.toUpperCase();
  String a = String(FARE_PREFIX) + fare; a.toUpperCase();
  uint16_t white = UfdPanel::color565(240, 240, 240);
  uint16_t gold  = UfdPanel::color565(255, 200, 0);
  ledText(FONT_S, v, SCREEN_W / 2, 66, white);
  const LedFont &f = (ledTextWidth(FONT_L, a) <= 62) ? FONT_L : FONT_M;
  ledText(f, a, SCREEN_W / 2, 75 + (19 - f.h) / 2, gold);
}

// ======================================================
// DISPLAY HELPERS  (call panels->show() after drawing)
// ======================================================
void stopAnim() { idleActive = false; tollScreen = false; }

// Toll screen: picture on panels 1+2 (server copy or built-in), live text on panel 3.
// keep = true: stays until the next command (lane closed)
void showTollPicture(const char *name, int panel3, const String &vehicle, const String &fare, bool keep)
{
  activePlate = activeFare = activeBalance = "";
  stopAnim();
  scrollActive = false;
  scr->clearScreen();
  drawPic(name, 0, 64);
  if (panel3 == 1) panel3Vehicle(vehicle);
  if (panel3 == 2) panel3Fare(vehicle, fare);
  displayPolicy.mode = keep ? DisplayPolicy::CLOSED : DisplayPolicy::IDLE;
  tollScreen = !keep;
  tollSince  = millis();
}

// Transaction layout: 64x32 plate, 64x32 fare, 64x32 status/balance.
void renderTransaction()
{
  stopAnim(); scrollActive = false; scr->clearScreen();
  const uint16_t cyan = UfdPanel::color565(70,210,255);
  const uint16_t gold = UfdPanel::color565(255,190,40);
  const uint16_t green = UfdPanel::color565(50,230,110);
  if (activePlate.length()) panelPlate(activePlate, 0);
  else smallLine("NO PLATE", 13, cyan);
  auto mode = displayPolicy.mode;
  if (mode == DisplayPolicy::UNAVAILABLE) {
    smallLine("UPDATE", 40, gold); smallLine("EXPIRED", 52, gold);
    smallLine("SEE STAFF", 77, gold);
    return;
  }
  if (mode == DisplayPolicy::ASSIST) {
    smallLine("PAYMENT", 39, gold); smallLine("REQUIRED", 52, gold);
    smallLine("SEE STAFF", 77, gold); return;
  }
  if (mode == DisplayPolicy::CHECKING && !activeFare.length()) {
    smallLine("RFID", 39, cyan); smallLine("CHECKING", 52, cyan);
  } else {
    uint16_t color = (mode == DisplayPolicy::PAID || mode == DisplayPolicy::RFID_PAID) ? green : gold;
    smallLine(mode == DisplayPolicy::PAID ? "PAID RS" : "FARE RS", 34, color);
    if (activeFare.indexOf('.') >= 0 || !plateText(activeFare, 32, 44, 60, 62, color))
      ledText(fitFont(activeFare, 62), activeFare, 32, 44, color);
  }
  if (mode == DisplayPolicy::RFID_PAID) {
    smallLine("BAL RS", 66, cyan);
    if (activeBalance.indexOf('.') >= 0 || !plateText(activeBalance, 32, 76, 60, 62, green))
      ledText(fitFont(activeBalance, 62), activeBalance, 32, 76, green);
  } else if (mode == DisplayPolicy::PAID) {
    smallLine("THANK", 70, green); smallLine("YOU", 83, green);
  } else if (mode == DisplayPolicy::CHECKING) {
    smallLine("PLEASE", 70, cyan); smallLine("WAIT", 83, cyan);
  } else {
    smallLine("PLEASE", 70, cyan); smallLine("PAY", 83, cyan);
  }
}

void legacyTransaction(DisplayPolicy::Mode mode, const String &plate, const String &fare, const String &balance)
{
  bool same = displayPolicy.mode == mode && activePlate == plate && activeFare == fare && activeBalance == balance;
  if (same) return; // repeated polling must not restart the display timer
  activePlate = plate; activeFare = fare; activeBalance = balance;
  displayPolicy.mode = mode; displayPolicy.since = millis();
  displayPolicy.leaseMs = UFD_ACTIVE_LEASE_MS;
  renderTransaction();
}
void showMetag(bool paid, const String &vehicle, const String &amount, const String &balance)
{
  legacyTransaction(paid ? DisplayPolicy::RFID_PAID : DisplayPolicy::ASSIST, vehicle, amount, balance);
}
void showPayFare(const String &vehicle, const String &fare)
{
  legacyTransaction(DisplayPolicy::WAITING, vehicle, fare, "");
}

bool isIdlePic(int i) { return !strncmp(pics[i].name, "idle", 4); }

bool idleAvailable(int i)       // idle picture that is not removed on the server
{
  if (!isIdlePic(i) || pics[i].hidden) return false;
  return pics[i].ok || builtinPic(pics[i].name);
}

// Shown idle pictures. If none is available (for example wrong file names on
// the server), the built-in idle pictures are used: the screen is never black.
bool idleShown(int i)
{
  for (int k = 0; k < PIC_COUNT; k++)
    if (idleAvailable(k)) return idleAvailable(i);
  return isIdlePic(i) && builtinPic(pics[i].name);
}

// What the screen uses for picture i (for /status and the Serial Monitor)
String showingText(int i)
{
  if (isIdlePic(i) && !idleShown(i)) return "not shown";
  if (isIdlePic(i) && pics[i].hidden) return "built-in (no idle pictures on server)";
  if (pics[i].ok) return "server copy";
  return builtinPic(pics[i].name) ? "built-in" : "none";
}

int idleCount()
{
  int n = 0;
  for (int i = 0; i < PIC_COUNT; i++) if (idleShown(i)) n++;
  return n;
}

// Show idle picture number k (0 = first stored one)
bool drawIdle(int k)
{
  for (int i = 0; i < PIC_COUNT; i++)
  {
    if (!idleShown(i)) continue;
    if (k-- == 0) { scr->clearScreen(); return drawPic(pics[i].name, 0, SCREEN_H); }
  }
  return false;
}

// Idle: the idle pictures on all panels, one after another
void startIdle()
{
  displayPolicy.mode = DisplayPolicy::IDLE;
  activePlate = activeFare = activeBalance = "";
  stopAnim();
  scrollActive = false;
  scr->clearScreen();
  idleIndex  = 0;
  idleLast   = millis();
  idleActive = true;
  drawIdle(0);
  panels->show();
}

void idleStep()
{
  // back to idle some seconds after a toll screen
  if (displayPolicy.expire(millis())) {
    if (displayPolicy.mode == DisplayPolicy::IDLE) startIdle();
    else { renderTransaction(); panels->show(); }
  }
  if (tollScreen && millis() - tollSince > (unsigned long)returnSec * 1000UL) startIdle();
  if (!idleActive) return;
  int n = idleCount();
  if (n < 2 || millis() - idleLast < (unsigned long)idleSec * 1000UL) return;
  idleLast  = millis();
  idleIndex = (idleIndex + 1) % n;
  if (drawIdle(idleIndex)) panels->show();
}

// One test screen per panel: border, diagonal, R-G-B squares, "#number"
void drawTestPattern()
{
  stopAnim();
  scrollActive = false;

  uint16_t white  = UfdPanel::color565(255, 255, 255);
  uint16_t yellow = UfdPanel::color565(255, 255, 0);
  uint16_t cyan   = UfdPanel::color565(0, 255, 255);

  scr->clearScreen();
  for (int k = 0; k < PANEL_COUNT; k++)
  {
    int y0 = k * PANEL_H;
    scr->drawRect(0, y0, PANEL_W, PANEL_H, white);
    scr->drawLine(1, y0 + 1, 30, y0 + 30, yellow);
    scr->fillRect(3,  y0 + 3, 5, 5, myRED);
    scr->fillRect(9,  y0 + 3, 5, 5, myGREEN);
    scr->fillRect(15, y0 + 3, 5, 5, myBLUE);
    scr->setTextColor(cyan);
    scr->setCursor(34, y0 + 4);
    scr->print("P");
    scr->print(panelPattern);
    scr->print("R");
    scr->print(panelRegs);
    scr->setCursor(40, y0 + 20);
    scr->print("#");
    scr->print(k + 1);
  }
  panels->show();
}

// ======================================================
// DYNAMIC TEXT HELPERS
// ======================================================
uint16_t parseColor(String s, uint16_t defColor)
{
  s.toLowerCase();
  s.trim();
  if (s.length() == 0) return defColor;

  if (s == "red")     return UfdPanel::color565(255, 0, 0);
  if (s == "green")   return UfdPanel::color565(0, 255, 0);
  if (s == "blue")    return UfdPanel::color565(0, 0, 255);
  if (s == "white")   return UfdPanel::color565(255, 255, 255);
  if (s == "black")   return UfdPanel::color565(0, 0, 0);
  if (s == "yellow")  return UfdPanel::color565(255, 255, 0);
  if (s == "cyan")    return UfdPanel::color565(0, 255, 255);
  if (s == "magenta") return UfdPanel::color565(255, 0, 255);
  if (s == "orange")  return UfdPanel::color565(255, 128, 0);

  if (s.length() == 6)
  {
    long v = strtol(s.c_str(), NULL, 16);
    return UfdPanel::color565((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
  }
  return defColor;
}

void renderLine(int i)
{
  TextLine &L = lines[i];

  lineCanvas->fillScreen(0);
  lineCanvas->setTextColor(L.color);

  if (L.text.length() > 0)
  {
    if (!L.scroll)
    {
      int x = (SCREEN_W - L.textW) / 2;
      lineCanvas->setCursor(x, 0);
      lineCanvas->print(L.text);
    }
    else
    {
      int cycle = L.textW + SCROLL_GAP;
      int x = -(L.offset % cycle);
      lineCanvas->setCursor(x, 0);
      lineCanvas->print(L.text);
      lineCanvas->setCursor(x + cycle, 0);
      lineCanvas->print(L.text);
    }
  }

  scr->drawRGBBitmap(0, L.y, lineCanvas->getBuffer(), SCREEN_W, 8);
}

void setLine(int i, String text, uint16_t color, int y)
{
  if (text.length() > MAX_TEXT_LEN) text = text.substring(0, MAX_TEXT_LEN);
  lines[i].text   = text;
  lines[i].color  = color;
  lines[i].y      = y;
  lines[i].textW  = text.length() * CHAR_W;
  lines[i].offset = 0;
  lines[i].scroll = lines[i].textW > SCREEN_W;
}

void showDynamic(String t1, String t2, uint16_t c1, uint16_t c2)
{
  stopAnim();
  setLine(0, t1, c1, LINE1_Y);
  setLine(1, t2, c2, LINE2_Y);
  scrollActive = lines[0].scroll || lines[1].scroll;
  lastScroll = millis();

  scr->clearScreen();
  renderLine(0);
  renderLine(1);
}

// ======================================================
// HTTP HELPERS
// ======================================================
String urlDecode(String s)
{
  s.replace("+", " ");
  String out;
  out.reserve(s.length());
  for (unsigned int i = 0; i < s.length(); i++)
  {
    if (s[i] == '%' && i + 2 < s.length())
    {
      char hex[3] = { s[i + 1], s[i + 2], 0 };
      out += (char)strtol(hex, NULL, 16);
      i += 2;
    }
    else
    {
      out += s[i];
    }
  }
  return out;
}

String getParam(const String &req, const String &key)
{
  int start = req.indexOf("?" + key + "=");
  if (start < 0) start = req.indexOf("&" + key + "=");
  if (start < 0) return "";
  start += key.length() + 2;
  int end = req.indexOf('&', start);
  int sp  = req.indexOf(' ', start);
  if (end < 0 || (sp >= 0 && sp < end)) end = sp;
  if (end < 0) end = req.length();
  return urlDecode(req.substring(start, end));
}

void sendHtml(NetworkClient &c, const String &body, int code = 200)
{
  c.print("HTTP/1.1 ");
  c.print(code);
  c.println(code == 200 ? " OK" : " Bad Request");
  c.println("Content-Type: text/html");
  c.println("Connection: close");
  c.println();
  c.println("<html><head><meta name='viewport' content='width=device-width,initial-scale=1'></head><body>");
  c.println(body);
  c.println("</body></html>");
}

String configPage(const String &message = "")
{
  String h = "<h2>Network Config</h2>";
  if (message.length()) h += "<p><b>" + message + "</b></p>";

  h += "<p>Ethernet IP now: ";
  h += ethernetConnected ? ETH.localIP().toString() : String("none");
  h += "</p>";

  h += "<form action='/config' method='get'>";
  h += "<label><input type='checkbox' name='dhcp' value='1'";
  if (cfgDHCP) h += " checked";
  h += "> Use DHCP</label><br><br>";
  h += "IP: <input name='ip' value='" + cfgIP + "'><br><br>";
  h += "Gateway: <input name='gw' value='" + cfgGW + "'><br><br>";
  h += "Subnet: <input name='sn' value='" + cfgSN + "'><br><br>";
  h += "DNS: <input name='dns' value='" + cfgDNS + "'><br><br>";
  h += "<input type='hidden' name='save' value='1'>";
  h += "<button type='submit'>Save &amp; Reboot</button>";
  h += "</form>";
  h += "<p><a href='/config?reset=1'>Reset to defaults</a> | <a href='/panel'>Panel setup</a></p>";

  h += "<h2>Picture server</h2>";
  h += "<form action='/config' method='get'>";
  h += "Folder address: <input name='url' size='45' value='" + picServer + "'><br><br>";
  h += "Each idle picture (seconds): <input name='idle' type='number' min='1' max='600' value='" + String(idleSec) + "'><br><br>";
  h += "Back to idle after a toll screen (seconds): <input name='ret' type='number' min='2' max='600' value='" + String(returnSec) + "'><br><br>";
  h += "<input type='hidden' name='picsave' value='1'>";
  h += "<button type='submit'>Save</button></form>";
  h += "<p><a href='/refresh'>Download changed pictures now</a> | <a href='/status'>Picture status</a></p>";

  h += panels->tuningHtml();        // HUB75 panel tuning
  return h;
}

String driverName()
{
  return panels->isDMA() ? "ESP32-HUB75-MatrixPanel-DMA" : "NOT RUNNING (check Serial Monitor)";
}

String panelPage()
{
  String h = "<h2>UFD Panel Setup</h2>";
  h += "<p>Panels: <b>" + String(PANEL_COUNT) + "</b> stacked, screen <b>" +
       String(SCREEN_W) + " x " + String(SCREEN_H) + "</b><br>"
       "Driver <b>" + driverName() + "</b>, brightness <b>" + String(panelBright) + "</b></p>";

  h += "<p><a href='/panel?test=1'><b>Show test pattern</b></a> &nbsp;"
       "(correct: #1 at the top, #2 below it";
  if (PANEL_COUNT > 2) h += ", #3 at the bottom";
  h += "; every panel shows a full white border, the yellow line starts top-left)</p>";

  h += "<p><b>Solid color test</b> (look at one pixel with your eyes):<br>"
       "<a href='/color?c=red'>[ red ]</a> <a href='/color?c=green'>[ green ]</a> "
       "<a href='/color?c=blue'>[ blue ]</a> <a href='/color?c=white'>[ white ]</a> "
       "<a href='/color?c=black'>[ black ]</a></p>";

  h += "<p><b>1. Panel order</b> (if the numbers are in the wrong order):<br>"
       "<a href='/panel?rev=0'>[ top panel first ]</a> "
       "<a href='/panel?rev=1'>[ bottom panel first ]</a> &nbsp;now: <b>" +
       String(panelRev ? "bottom first" : "top first") + "</b></p>";

  h += "<p><b>2. Upside-down panels</b> (if one panel's test is upside down, tap it):<br>";
  for (int k = 0; k < PANEL_COUNT; k++)
  {
    bool f = panelFlip & (1 << k);
    h += "<a href='/panel?flip=" + String(k) + "'>[ panel #" + String(k + 1) +
         (f ? ": flipped" : ": normal") + " ]</a> ";
  }
  h += "</p>";

  h += "<p><b>3. Brightness:</b><br>"
       "<a href='/panel?bright=255'>[ 100% ]</a> <a href='/panel?bright=128'>[ 50% ]</a> "
       "<a href='/panel?bright=64'>[ 25% ]</a> <a href='/panel?bright=32'>[ 12% ]</a></p>";

  h += "<p><a href='/idle'>Show idle pictures</a> | "
       "<a href='/config'>Network</a> | <a href='/status'>Status</a></p>";
  return h;
}

// ======================================================
// NETWORK SETUP (core 0)
// ======================================================
// Ethernet start: the W5500 is reset (if RST is wired) and checked up to
// 15 times. The web server is only started when Ethernet is running -
// starting it without a network crashed and restarted the ESP32.
bool netStarted = false;

bool startEthernet()
{
  // Hardware reset of the W5500 (only if W5500_RST is set to a pin)
  if (W5500_RST >= 0)
  {
    pinMode(W5500_RST, OUTPUT);
    digitalWrite(W5500_RST, LOW);
    delay(10);
    digitalWrite(W5500_RST, HIGH);
    delay(200);
  }

  pinMode(W5500_CS, OUTPUT);
  digitalWrite(W5500_CS, HIGH);
  SPI.begin(W5500_SCK, W5500_MISO, W5500_MOSI, W5500_CS);

  uint8_t ver = 0;
  for (int i = 1; i <= 15; i++)
  {
    ver = readW5500Version();
    if (ver == 0x04) break;
    Serial.printf("W5500 version register: 0x%X (try %d of 15)\n", ver, i);
    delay(200);
  }
  Serial.print("W5500 version register: 0x");
  Serial.println(ver, HEX);

  if (ver != 0x04)
  {
    Serial.println("W5500 NOT RESPONDING - display keeps running, trying again in 10 s "
                   "(check the W5500 wiring, 3.3V power and GND)");
    return false;
  }

  Serial.println("W5500 chip OK");
  if (!ETH.begin(ETH_PHY_W5500, 0, W5500_CS, W5500_INT, W5500_RST, SPI))
  {
    Serial.println("ETH.begin() FAILED - trying again in 10 s");
    return false;
  }

  if (!cfgDHCP)
  {
    IPAddress i, g, s, d;
    if (i.fromString(cfgIP) && g.fromString(cfgGW) &&
        s.fromString(cfgSN) && d.fromString(cfgDNS))
    {
      ETH.config(i, g, s, d);
      Serial.println("Static IP configured: " + cfgIP);
    }
    else
    {
      Serial.println("Saved IP settings invalid, using DHCP");
    }
  }
  else
  {
    Serial.println("Using DHCP");
  }

  serverHTTP.begin();
  Serial.println("HTTP Server Started");
  return true;
}

void setupNetwork()
{
  Network.onEvent(onNetworkEvent);
  netStarted = startEthernet();
}

// HTTP lines have a total deadline and size limit: a slow sender cannot grow String forever.
bool boundedLine(NetworkClient &client, String &out, size_t limit, uint32_t started)
{
  out = "";
  while ((uint32_t)(millis() - started) < 1000) {
    while (client.available()) {
      int ch = client.read();
      if (ch == '\n') { out.trim(); return true; }
      if (ch < 0 || out.length() >= limit) return false;
      out += (char)ch;
    }
    if (!client.connected()) return false;
    vTaskDelay(1);
  }
  return false;
}
void jsonReply(NetworkClient &c, int status, const String &body)
{
  c.printf("HTTP/1.1 %d %s\r\n", status, status == 200 ? "OK" : "Error");
  c.print("Content-Type: application/json\r\nCache-Control: no-store\r\nConnection: close\r\nContent-Length: ");
  c.print(body.length()); c.print("\r\n\r\n"); c.print(body);
}
const char *modeName()
{
  switch (displayPolicy.mode) {
    case DisplayPolicy::WAITING: return "waiting";
    case DisplayPolicy::CHECKING: return "checking";
    case DisplayPolicy::PAID: return "paid";
    case DisplayPolicy::RFID_PAID: return "rfid-paid";
    case DisplayPolicy::ASSIST: return "assist";
    case DisplayPolicy::CLOSED: return "closed";
    case DisplayPolicy::UNAVAILABLE: return "expired";
    default: return "idle";
  }
}
String statusJson(const char *result)
{
  return String("{\"result\":\"") + result + "\",\"bootId\":\"" + bootId +
    "\",\"sequence\":" + String(displayPolicy.sequence) + ",\"passageId\":\"" + activePassage +
    "\",\"mode\":\"" + modeName() + "\",\"ready\":" + (displayReady ? "true" : "false") +
    ",\"controlled\":" + (displayPolicy.controlled ? "true" : "false") +
    ",\"authenticated\":" + (strlen(UFD_API_TOKEN) ? "true" : "false") +
    ",\"uptimeMs\":" + String(millis()) + ",\"freeHeap\":" + String(ESP.getFreeHeap()) + "}";
}
bool unsignedValue(const String &s, uint32_t &out, uint32_t maxValue)
{
  if (!s.length() || s.length() > 10) return false;
  out = 0;
  for (unsigned i=0; i<s.length(); ++i) {
    if (s[i] < '0' || s[i] > '9') return false;
    unsigned digit = s[i]-'0';
    if (out > maxValue/10 || (out == maxValue/10 && digit > maxValue%10)) return false;
    out = out*10 + digit;
  }
  return true;
}
bool safeLabel(const String &s, unsigned maxLen, bool plate)
{
  if (s.length() > maxLen) return false;
  for (unsigned i=0; i<s.length(); ++i) {
    char c=s[i];
    if (!((c>='A'&&c<='Z') || (c>='a'&&c<='z') || (c>='0'&&c<='9') || c=='-' || (!plate && c=='_') || (plate && (c==' '||c=='.')))) return false;
  }
  return true;
}
bool amountValid(const String &s)
{
  if (!s.length() || s.length()>8) return false;
  unsigned dots=0, decimals=0;
  for (unsigned i=0;i<s.length();i++) {
    if (s[i]=='.') { if (++dots>1 || i==0 || i==s.length()-1) return false; }
    else { if(s[i]<'0'||s[i]>'9') return false; if(dots && ++decimals>2) return false; }
  }
  return true;
}
// Reject duplicate keys and encoded/control parameter names. Values are validated below.
bool validQuery(const String &target)
{
  int q=target.indexOf('?'); if(q<0) return true;
  String seen="|";
  for(int at=q+1;at<(int)target.length();) {
    int end=target.indexOf('&',at); if(end<0) end=target.length();
    String pair=target.substring(at,end); int eq=pair.indexOf('=');
    String key=eq<0?pair:pair.substring(0,eq);
    if(!key.length() || !safeLabel(key,32,false) || seen.indexOf("|"+key+"|")>=0) return false;
    seen+=key+"|"; at=end+1;
  }
  return true;
}
void applyFrame(NetworkClient &client, const String &request)
{
  const String allowed="|bootId|sequence|expiresAtMs|passageId|state|plate|fare|balance|leaseMs|";
  int endTarget=request.indexOf(' ',request.indexOf(' ')+1);
  int at=request.indexOf('?')+1;
  while(at>0 && at<endTarget) {
    int end=request.indexOf('&',at); if(end<0 || end>endTarget) end=endTarget;
    int eq=request.indexOf('=',at);
    if(eq<at || eq>=end || allowed.indexOf("|"+request.substring(at,eq)+"|")<0) {
      jsonReply(client,400,"{\"error\":\"unknown_parameter\"}"); return;
    }
    at=end+1;
  }
  String state=getParam(request,"state"), passage=getParam(request,"passageId");
  String plate=getParam(request,"plate"), fare=getParam(request,"fare"), balance=getParam(request,"balance");
  uint32_t seq=0, lease=UFD_ACTIVE_LEASE_MS;
  String leaseText=getParam(request,"leaseMs");
  if(!unsignedValue(getParam(request,"sequence"),seq,UINT32_MAX) ||
     (leaseText.length() && (!unsignedValue(leaseText,lease,300000) || lease<5000)) ||
     !safeLabel(passage,48,false) || !safeLabel(plate,12,true) || ledTextWidth(FONT_S,plate)>62) {
    jsonReply(client,400,"{\"error\":\"invalid_fields\"}"); return;
  }
  DisplayPolicy::Mode next;
  if(state=="waiting") next=DisplayPolicy::WAITING;
  else if(state=="checking") next=DisplayPolicy::CHECKING;
  else if(state=="paid") next=DisplayPolicy::PAID;
  else if(state=="rfid-paid") next=DisplayPolicy::RFID_PAID;
  else if(state=="assist") next=DisplayPolicy::ASSIST;
  else if(state=="closed") next=DisplayPolicy::CLOSED;
  else if(state=="idle" || state=="open") next=DisplayPolicy::IDLE;
  else { jsonReply(client,400,"{\"error\":\"invalid_state\"}"); return; }
  bool financial=next==DisplayPolicy::WAITING || next==DisplayPolicy::PAID || next==DisplayPolicy::RFID_PAID;
  bool transaction=financial || next==DisplayPolicy::CHECKING || next==DisplayPolicy::ASSIST;
  if(ledTextWidth(FONT_S,fare)>62 || ledTextWidth(FONT_S,balance)>62 || (transaction && !passage.length()) || (financial && !amountValid(fare)) ||
     (fare.length() && !amountValid(fare)) || (balance.length() && !amountValid(balance)) ||
     (next==DisplayPolicy::RFID_PAID && !amountValid(balance))) {
    jsonReply(client,400,"{\"error\":\"invalid_transaction\"}"); return;
  }
  String payload=state+"|"+passage+"|"+plate+"|"+fare+"|"+balance+"|"+String(lease)+"|"+getParam(request,"expiresAtMs");
  auto decision=displayPolicy.check(getParam(request,"bootId")==bootId,seq,payload==lastPayload,next,state=="open");
  if(decision==DisplayPolicy::DUPLICATE) { jsonReply(client,200,statusJson("duplicate")); return; }
  if(decision!=DisplayPolicy::APPLY) {
    jsonReply(client,409,String("{\"error\":\"") + (decision==DisplayPolicy::WRONG_BOOT?"boot_changed":decision==DisplayPolicy::LOCKED?"lane_closed":decision==DisplayPolicy::CONFLICT?"sequence_conflict":"stale_sequence") + "\"}"); return;
  }
  uint32_t expires=0;
  if(!unsignedValue(getParam(request,"expiresAtMs"),expires,UINT32_MAX) ||
     (int32_t)(expires-(uint32_t)millis()) <= 0 || (int32_t)(expires-(uint32_t)millis()) > 10000) {
    jsonReply(client,409,"{\"error\":\"expired_or_invalid_deadline\"}"); return;
  }
  if(!displayReady) { jsonReply(client,503,"{\"error\":\"panel_unavailable\"}"); return; }
  activePassage=passage; activePlate=plate; activeFare=fare; activeBalance=balance;
  displayPolicy.commit(seq,next,millis(),lease); lastPayload=payload;
  if(next==DisplayPolicy::IDLE) startIdle();
  else if(next==DisplayPolicy::CLOSED) showTollPicture("laneclosed",0,"","",true);
  else renderTransaction();
  panels->show();
  jsonReply(client,200,statusJson("applied"));
}

// ======================================================
// NETWORK LOOP (core 0)
// ======================================================
void networkLoop()
{
  // ---------- Idle pictures, and back to idle after a toll screen ----------
  idleStep();

  // ---------- Scroll long text ----------
  if (scrollActive && millis() - lastScroll >= SCROLL_MS)
  {
    lastScroll = millis();
    for (int i = 0; i < 2; i++)
    {
      if (lines[i].scroll)
      {
        lines[i].offset++;
        renderLine(i);
      }
    }
    panels->show();
  }

  // ---------- Status print ----------
  static unsigned long lastStatus = 0;
  if (millis() - lastStatus >= 5000)
  {
    lastStatus = millis();
    Serial.print("ETH link: ");
    Serial.print(linkDetected ? "UP" : "DOWN");
    Serial.print(" | ETH IP: ");
    Serial.print(ethernetConnected ? ETH.localIP().toString() : String("none"));
    Serial.printf(" | free RAM %u KB\n", (unsigned)(ESP.getFreeHeap() / 1024));
  }

  // ---------- W5500 did not answer at start: try again every 10 s ----------
  if(ethernetStopped) netStarted = false;
  if (!netStarted)
  {
    static unsigned long lastTry = 0;
    if (millis() - lastTry >= 10000)
    {
      lastTry = millis();
      netStarted = startEthernet();
    }
    return;                                   // no web server without Ethernet
  }

  NetworkClient httpClient = serverHTTP.accept();
  if (!httpClient) return;

  uint32_t readStarted = millis();
  String request, header, authorization;
  if (!boundedLine(httpClient, request, 1024, readStarted)) {
    jsonReply(httpClient,400,"{\"error\":\"request_too_long_or_timeout\"}"); httpClient.stop(); return;
  }
  unsigned headerBytes=0; bool headersDone=false;
  while (headerBytes < 2048 && boundedLine(httpClient,header,512,readStarted)) {
    headerBytes+=header.length()+2;
    if (!header.length()) { headersDone=true; break; }
    int colon=header.indexOf(':');
    String name=header.substring(0,colon); name.toLowerCase();
    if(name=="authorization") { authorization=header.substring(colon+1); authorization.trim(); }
  }
  if(!headersDone) { jsonReply(httpClient,400,"{\"error\":\"invalid_headers\"}"); httpClient.stop(); return; }
  int firstSpace=request.indexOf(' '), lastSpace=request.lastIndexOf(' ');
  if(firstSpace<=0 || lastSpace<=firstSpace || !request.substring(lastSpace+1).startsWith("HTTP/1.")) {
    jsonReply(httpClient,400,"{\"error\":\"invalid_request\"}"); httpClient.stop(); return;
  }
  String method=request.substring(0,firstSpace), target=request.substring(firstSpace+1,lastSpace);
  int q=target.indexOf('?'); String path=q<0?target:target.substring(0,q);
  if(!validQuery(target)) { jsonReply(httpClient,400,"{\"error\":\"invalid_query\"}"); httpClient.stop(); return; }
  if(!strlen(UFD_API_TOKEN) && !UFD_ALLOW_LEGACY) {
    jsonReply(httpClient,503,"{\"error\":\"configure_api_token\"}"); httpClient.stop(); return;
  }
  bool authorized = strlen(UFD_API_TOKEN)==0 || authorization==String("Bearer ")+UFD_API_TOKEN;
  if(!authorized) { jsonReply(httpClient,401,"{\"error\":\"unauthorized\"}"); httpClient.stop(); return; }
  if(path=="/v1/status" && method=="GET") {
    jsonReply(httpClient,200,statusJson("status")); httpClient.stop(); return;
  }
  if(path=="/v1/frame" && method=="POST") {
    applyFrame(httpClient,request); httpClient.stop(); return;
  }
  bool admin=path=="/config" || path=="/panel" || path=="/color" || path=="/display" || path=="/refresh";
  if(admin && (!UFD_ENABLE_ADMIN || !strlen(UFD_API_TOKEN))) {
    jsonReply(httpClient,403,"{\"error\":\"admin_disabled\"}"); httpClient.stop(); return;
  }
  bool readOnly=path=="/status" || target=="/";
  if(target=="/" && method=="GET") { jsonReply(httpClient,200,statusJson("status")); httpClient.stop(); return; }
  bool known=readOnly||admin||path=="/"||path=="/idle"||path=="/thankyou"||path=="/takeslip"||path=="/open"||path=="/closed";
  if(!known || method!="GET") { jsonReply(httpClient,404,"{\"error\":\"unknown_command\"}"); httpClient.stop(); return; }
  if(!readOnly && !admin && (!UFD_ALLOW_LEGACY || displayPolicy.controlled)) {
    jsonReply(httpClient,409,"{\"error\":\"use_v1_frame\"}"); httpClient.stop(); return;
  }
  if(!readOnly && !displayReady) { jsonReply(httpClient,503,"{\"error\":\"panel_unavailable\"}"); httpClient.stop(); return; }
  if(!readOnly && !admin && displayPolicy.mode==DisplayPolicy::CLOSED && path!="/closed" && path!="/open") {
    jsonReply(httpClient,409,"{\"error\":\"lane_closed\"}"); httpClient.stop(); return;
  }
  String vehicle=getParam(request,"vehicle_number"), fare=getParam(request,"fare_amount");
  vehicle.toUpperCase(); vehicle.trim();
  // Old callers use a special WELCOME plate; never let it replace a live transaction.
  bool welcome=path=="/idle" || (path=="/" && vehicle=="WELCOME");
  if(welcome) {
    if(displayPolicy.legacyIdleAllowed()) startIdle();
    jsonReply(httpClient,200,statusJson(displayPolicy.legacyIdleAllowed()?"applied":"suppressed")); httpClient.stop(); return;
  }
  if(path=="/") {
    if(request.indexOf("&CLOSED")>=0 && vehicle=="BOOTH") {
      showTollPicture("laneclosed",0,"","",true); panels->show();
      jsonReply(httpClient,200,statusJson("applied")); httpClient.stop(); return;
    }
    bool valid=safeLabel(vehicle,12,true) && vehicle.length() && ledTextWidth(FONT_S,vehicle)<=62;
    bool slip=request.indexOf("&take_slip")>=0;
    bool low=request.indexOf("metag_low")>=0;
    bool metag=request.indexOf("metag_paid=")>=0;
    unsigned commands=(slip?1:0)+(low?1:0)+(metag?1:0)+(fare.length()?1:0);
    valid=valid && commands==1;
    if(metag) valid=valid && amountValid(getParam(request,"metag_paid")) && amountValid(getParam(request,"balance"));
    if(fare.length()) valid=valid && amountValid(fare) && ledTextWidth(FONT_S,fare)<=62;
    if(!valid) { jsonReply(httpClient,400,"{\"error\":\"invalid_legacy_command\"}"); httpClient.stop(); return; }
  }
  Serial.printf("UFD %s %s mode=%s\n",method.c_str(),path.c_str(),modeName());
  String req = request; req.toLowerCase();

  bool responded = false;

  // ---------------- PANEL SETUP ----------------
  if (req.indexOf("get /panel") >= 0)
  {
    bool changed = false;
    String v;

    v = getParam(request, "pattern");
    if (v.length()) { panelPattern = v.toInt() % UfdPanel::NUM_PATTERNS; panels->setPattern(panelPattern); changed = true; }

    v = getParam(request, "regs");
    if (v.length()) { panelRegs = v.toInt() % UfdPanel::NUM_REGSETS; panels->setRegSet(panelRegs); changed = true; }

    v = getParam(request, "rev");
    if (v.length()) { panelRev = (v.toInt() != 0); panels->setChainReverse(panelRev); changed = true; }

    v = getParam(request, "flip");
    if (v.length())
    {
      int k = v.toInt();
      if (k >= 0 && k < PANEL_COUNT)
      {
        panelFlip ^= (1 << k);
        scr->setFlipMask(panelFlip);
        changed = true;
      }
    }

    v = getParam(request, "bright");
    if (v.length()) { panelBright = constrain(v.toInt(), 1, 255); panels->setBrightness8(panelBright); changed = true; }

    v = getParam(request, "clkinv");
    if (v.length()) { panelClkInv = (v.toInt() != 0); panels->setDmaClockInvert(panelClkInv); changed = true; }

    v = getParam(request, "mhz");
    if (v.length()) { panels->setDmaMHz(v.toInt()); panelMhz = panels->getDmaMHz(); changed = true; }

    if (changed) savePanelConfig();
    if (changed || req.indexOf("test=1") >= 0) drawTestPattern();

    sendHtml(httpClient, panelPage());
    responded = true;
  }
  // ---------------- CONFIG PAGE ----------------
  else if (req.indexOf("get /config") >= 0)
  {
    if (req.indexOf("hub=1") >= 0)          // HUB75 panel tuning
    {
      panels->saveTuning(request);
      sendHtml(httpClient, "<h3>Panel tuning saved. Restarting...</h3>"
                           "<p>" + panels->tuningText() + "</p>"
                           "<p>Wait 10 seconds, then <a href='/config'>back to /config</a>.</p>");
      httpClient.stop();
      delay(500);
      ESP.restart();
    }
    else if (req.indexOf("picsave=1") >= 0)
    {
      String u = getParam(request, "url");
      u.trim();
      if (u.startsWith("http://"))
      {
        if (!u.endsWith("/")) u += "/";
        xSemaphoreTake(picLock, portMAX_DELAY);
        picServer = u;
        xSemaphoreGive(picLock);
      }
      idleSec   = constrain(getParam(request, "idle").toInt(), 1, 600);
      returnSec = constrain(getParam(request, "ret").toInt(), 2, 600);
      savePicConfig();
      syncNow = true;
      sendHtml(httpClient, configPage(u.startsWith("http://") ? "Picture server saved. Downloading pictures..."
                                                              : "Address must start with http:// - not changed."));
    }
    else if (req.indexOf("reset=1") >= 0)
    {
      resetConfig();
      sendHtml(httpClient, "<h3>Settings reset. Rebooting...</h3>");
      httpClient.stop();
      delay(500);
      ESP.restart();
    }
    else if (req.indexOf("save=1") >= 0)
    {
      bool dhcp   = req.indexOf("dhcp=1") >= 0;
      String nIP  = getParam(request, "ip");
      String nGW  = getParam(request, "gw");
      String nSN  = getParam(request, "sn");
      String nDNS = getParam(request, "dns");

      if (!dhcp && !(validIP(nIP) && validIP(nGW) && validIP(nSN) && validIP(nDNS)))
      {
        sendHtml(httpClient, configPage("Invalid IP address. Nothing saved."), 400);
      }
      else
      {
        cfgDHCP = dhcp;
        if (validIP(nIP))  cfgIP  = nIP;
        if (validIP(nGW))  cfgGW  = nGW;
        if (validIP(nSN))  cfgSN  = nSN;
        if (validIP(nDNS)) cfgDNS = nDNS;
        saveConfig();

        sendHtml(httpClient,
          "<h3>Saved. Rebooting...</h3><p>New Ethernet IP: " +
          String(dhcp ? "DHCP (assigned by router)" : cfgIP) + "</p>");
        httpClient.stop();
        delay(500);
        ESP.restart();
      }
      responded = true;
    }
    else
    {
      sendHtml(httpClient, configPage());
      responded = true;
    }
  }
  // ---------------- STATUS ----------------
  else if (req.indexOf("get /status") >= 0)
  {
    String s = "ETH link: " + String(linkDetected ? "UP" : "DOWN");
    s += "<br>ETH IP: " + (ethernetConnected ? ETH.localIP().toString() : String("none"));
    s += "<br>Panels: " + String(PANEL_COUNT) + " stacked (" + String(SCREEN_W) + "x" + String(SCREEN_H) + ")";
    s += "<br>Order " + String(panelRev ? "bottom first" : "top first") +
         ", brightness " + String(panelBright);
    s += "<br>Driver: " + driverName();
    s += "<br>Panel tuning: " + panels->tuningText();
    s += "<br>Free RAM: " + String(ESP.getFreeHeap() / 1024) + " KB";
    xSemaphoreTake(picLock, portMAX_DELAY);
    s += "<br><br><b>Picture server:</b> " + picServer;
    s += "<br>Largest free memory block: " + String(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) / 1024) + " KB";
    s += "<br>Last check: " + (lastSync ? String((millis() - lastSync) / 1000) + " s ago, " + lastSyncText : String("not yet"));
    s += "<br><table border='1' cellpadding='3' style='border-collapse:collapse'><tr><th>Picture</th><th>Showing</th><th>Last result</th></tr>";
    for (int i = 0; i < PIC_COUNT; i++)
      s += "<tr><td>" + String(pics[i].name) + ".png</td><td>" +
           showingText(i) +
           "</td><td>" + (pics[i].state.length() ? pics[i].state : String("-")) + "</td></tr>";
    s += "</table><a href='/refresh'>Download changed pictures now</a>";
    xSemaphoreGive(picLock);
    s += "<br><br><a href='/idle'>Show idle pictures</a> | <a href='/config'>Picture server settings</a>";
    sendHtml(httpClient, s);
    responded = true;
  }
  // ---------------- DYNAMIC TEXT ----------------
  else if (req.indexOf("get /display") >= 0)
  {
    String l1 = getParam(request, "line1");
    String l2 = getParam(request, "line2");

    uint16_t baseColor = parseColor(getParam(request, "color"), myRED);
    uint16_t c1 = parseColor(getParam(request, "color1"), baseColor);
    uint16_t c2 = parseColor(getParam(request, "color2"), baseColor);

    showDynamic(l1, l2, c1, c2);
  }
  // ---------------- SOLID COLOR TEST: /color?c=red|green|blue|white|black ----------------
  else if (req.indexOf("get /color") >= 0)
  {
    stopAnim();
    scrollActive = false;
    String c = getParam(request, "c");
    scr->fillScreen(parseColor(c, 0));
    Serial.println("Solid color: " + c);
  }
  // ---------------- FIXED SCREENS ----------------
  else if (req.indexOf("get /idle") >= 0)
  {
    startIdle();
  }
  else if (req.indexOf("get /refresh") >= 0)
  {
    syncNow = true;
    sendHtml(httpClient, "<h3>Checking the picture server...</h3><p>See <a href='/status'>status</a> in a few seconds.</p>");
    responded = true;
  }
  else if (req.indexOf("get /thankyou") >= 0)
  {
    if (activeFare.length()) legacyTransaction(DisplayPolicy::PAID,activePlate,activeFare,"");
    else { jsonReply(httpClient,409,"{\"error\":\"no_active_fare\"}"); httpClient.stop(); return; }
  }
  else if (req.indexOf("get /takeslip") >= 0)
  {
    showTollPicture("takeslip", 0, "", "", false);
  }
  else if (req.indexOf("get /open") >= 0)
  {
    showTollPicture("laneopen", 0, "", "", false);
  }
  else if (req.indexOf("get /closed") >= 0)
  {
    showTollPicture("laneclosed", 0, "", "", true);
  }
  else if (request.indexOf("metag_paid=") >= 0)
  {
    showMetag(true, getParam(request, "vehicle_number"), getParam(request, "metag_paid"), getParam(request, "balance"));
  }
  else if (request.indexOf("metag_low") >= 0)
  {
    showMetag(false, getParam(request, "vehicle_number"), "", "");
  }
  else if (request.indexOf("vehicle_number=") >= 0 &&
           request.indexOf("&take_slip") >= 0)
  {
    showTollPicture("takeslip", 1, vehicle, "", false);
  }
  else if (path=="/" && fare.length())
  {
    showPayFare(vehicle, fare);
  }
  else
  {
    jsonReply(httpClient,404,"{\"error\":\"unknown_command\"}");
    responded=true;
  }

  if (!responded)
  {
    panels->show();
    jsonReply(httpClient,200,statusJson("applied"));
  }

  delay(1);
  httpClient.stop();
}

void networkTask(void *)
{
  esp_task_wdt_config_t watchdog = {};
  watchdog.timeout_ms = 30000; watchdog.idle_core_mask = 0; watchdog.trigger_panic = true;
  esp_err_t wd = esp_task_wdt_init(&watchdog);
  if(wd == ESP_ERR_INVALID_STATE) wd = esp_task_wdt_reconfigure(&watchdog);
  bool watched = wd == ESP_OK && esp_task_wdt_add(NULL) == ESP_OK;
  Serial.printf("Command watchdog: %s\n", watched ? "enabled" : "FAILED");
  setupNetwork();
  for (;;)
  {
    networkLoop();
    if(watched) esp_task_wdt_reset();
    vTaskDelay(1);
  }
}

// ======================================================
// SETUP
// ======================================================
void setup()
{
  Serial.begin(115200);
  bootId = String(esp_random(), HEX) + String(esp_random(), HEX);
  delay(2000);
  Serial.println();
  Serial.println("Start UFD V11: plate / fare / status");
  Serial.printf("Auth=%s legacy=%d admin=%d reset_reason=%d\n", strlen(UFD_API_TOKEN)?"enabled":"NOT CONFIGURED",UFD_ALLOW_LEGACY,UFD_ENABLE_ADMIN,(int)esp_reset_reason());

  picLock = xSemaphoreCreateMutex();

  loadConfig();
  loadPicConfig();

  fsOK = LittleFS.begin(false);    // never erase cached assets automatically after mount failure
  Serial.println(fsOK ? "LittleFS OK" : "LittleFS FAILED");
  if (fsOK) deleteOldFiles();      // free the space of the old version
  for (int i = 0; i < PIC_COUNT; i++)                        // pictures stored from earlier
  {
    File f = fsOK ? LittleFS.open(picPath(pics[i].name), "r") : File();
    pics[i].ok = f && (int)f.size() >= PANEL_W * pics[i].h * 2;
    prefs.begin("pics", true);
    pics[i].hidden = prefs.getBool((String("h_") + pics[i].name).c_str(), false);
    prefs.end();
    pics[i].state = pics[i].ok ? "server copy stored" : "built-in";
    if (f) f.close();
  }

  UfdPanel_Pins pins = {
    R1_PIN, G1_PIN, B1_PIN, R2_PIN, G2_PIN, B2_PIN,
    A_PIN, B_PIN, C_PIN,
    LAT_PIN, OE_PIN, CLK_PIN
  };

  panels = new UfdPanel(pins, PANEL_COUNT);
  panels->setDmaClockInvert(panelClkInv);    // saved signal settings
  panels->setDmaMHz(panelMhz);
  displayReady = panels->begin();
  if (!displayReady)
  {
    Serial.println("Panel init FAILED (see HUB75 message above)");
  }
  panels->setPattern(panelPattern);
  panels->setRegSet(panelRegs);
  panels->setChainReverse(panelRev);
  panels->setBrightness8(panelBright);

  scr = new StackedScreen(panels, PANEL_COUNT);
  scr->setFlipMask(panelFlip);
  scr->setTextWrap(false);

  myRED   = UfdPanel::color565(255, 0, 0);
  myGREEN = UfdPanel::color565(0, 255, 0);
  myBLUE  = UfdPanel::color565(0, 0, 255);

  lineCanvas = new GFXcanvas16(SCREEN_W, 8);
  lineCanvas->setTextWrap(false);
  lineCanvas->setTextSize(1);

  startIdle();                      // idle pictures until the first toll command
  Serial.printf("Panels: %d stacked (%dx%d), %s, brightness %d\n",
                PANEL_COUNT, SCREEN_W, SCREEN_H,
                panelRev ? "bottom first" : "top first", panelBright);
  for (int i = 0; i < PIC_COUNT; i++)
    Serial.printf("  %-10s %s\n", pics[i].name, showingText(i).c_str());

  xTaskCreatePinnedToCore(networkTask, "net", 8192, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(syncTask, "picsync", 6144, NULL, 1, NULL, 0);

  // Panels are refreshed by DMA (startTask does nothing for this driver)
  panels->startTask(1);
}

void loop()
{
  vTaskDelete(NULL);
}

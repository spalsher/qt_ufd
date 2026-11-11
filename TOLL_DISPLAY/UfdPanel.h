#pragma once
/* ======================================================================
   UfdPanel  -  driver for the OLD HUB75 panels (normal shift-register
                driver chips) using the ESP32-HUB75-MatrixPanel-DMA library.

   It has the same functions as FM6373_Panel, so the UFD sketch works
   unchanged: draw into the picture, then call show().

   V10 DEFAULTS (tested on the old panels, 3 stacked):
     scan 1/8, 8 MHz, clock edge A, latch 2, refresh >= 60 Hz,
     colour depth 8 bit, driver shift register.

   TUNING
   All settings that can cause flicker, wrong colours, colours in black
   areas or missing LEDs can be changed on  http://<ip>/config
   ("HUB75 panel tuning"). They are stored in their own memory area
   ("hub75") and the ESP32 restarts by itself to apply them.

     Scan mode        1/16 (normal) | 1/8 | 1/8 halves swapped
     Clock speed      5 / 8 / 10 / 15 / 20 MHz
     Clock edge       B (library default) | A
     Latch blanking   1..8   (higher = less ghosting / colour bleeding)
     Min refresh      30..240 Hz (higher = less flicker, fewer colours)
     Colour depth     2..8 bits per colour
     Driver chip      shift register | FM6124 | FM6126A | ICN2038S | MBI5124
   ====================================================================== */

#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <Preferences.h>

struct UfdPanel_Pins
{
  int8_t r1, g1, b1, r2, g2, b2;   // data
  int8_t a, b, c;                  // row address
  int8_t lat, oe, clk;             // control
};

// All tuning values, saved in Preferences namespace "hub75"
struct UfdTuning
{
  uint8_t  scan    = 1;     // 0 = 1/16, 1 = 1/8, 2 = 1/8 halves swapped   (tested: 1/8)
  uint8_t  mhz     = 8;     // 5, 8, 10, 15, 20                             (tested: 8)
  bool     edgeA   = true;  // true = edge A (fixes the extra colours)      (tested: A)
  uint8_t  latch   = 2;     // latch blanking 1..8                          (tested: 2)
  uint16_t refresh = 60;    // minimum refresh rate in Hz                   (tested: 60)
  uint8_t  depth   = 8;     // colour depth bits 2..8                       (tested: 8)
  uint8_t  driver  = 0;     // 0 shift reg, 1 FM6124, 2 FM6126A, 3 ICN2038S, 4 MBI5124 (tested: 0)
};

class UfdPanel : public Adafruit_GFX
{
public:
  static const uint8_t PANEL_W      = 64;
  static const uint8_t PANEL_H      = 32;
  static const uint8_t NUM_PATTERNS = 1;   // scan mode is on /config now
  static const uint8_t NUM_REGSETS  = 1;   // FM6373 only - not used here

  UfdPanel(const UfdPanel_Pins &pins, uint8_t chain = 1)
    : Adafruit_GFX(PANEL_W * chain, PANEL_H), _p(pins), _chain(chain) {}

  // ------------------------------------------------------------------
  bool begin()
  {
    _W = PANEL_W * _chain;
    size_t n = (size_t)_W * PANEL_H;
    _fb    = (uint16_t *)calloc(n, sizeof(uint16_t));
    _shown = (uint16_t *)calloc(n, sizeof(uint16_t));
    _mtx   = xSemaphoreCreateMutex();
    if (!_fb || !_shown || !_mtx) return false;

    loadTuning();
    const UfdTuning &t = _t;

    HUB75_I2S_CFG::i2s_pins pins = {
      _p.r1, _p.g1, _p.b1, _p.r2, _p.g2, _p.b2,
      _p.a, _p.b, _p.c, -1, -1,                 // D and E not used
      _p.lat, _p.oe, _p.clk
    };

    // 1/8 scan panels: the DMA buffer is twice as wide and half as high
    HUB75_I2S_CFG cfg(t.scan ? PANEL_W * 2 : PANEL_W,
                      t.scan ? PANEL_H / 2 : PANEL_H, _chain, pins);
    cfg.driver           = (HUB75_I2S_CFG::shift_driver)t.driver;
    cfg.i2sspeed         = (HUB75_I2S_CFG::clk_speed)((uint32_t)t.mhz * 1000000UL);
    cfg.clkphase         = !t.edgeA;
    cfg.latch_blanking   = t.latch;
    cfg.min_refresh_rate = t.refresh;
    cfg.setPixelColorDepthBits(t.depth);

    _dma = new MatrixPanel_I2S_DMA(cfg);
    if (!_dma || !_dma->begin())
    {
      Serial.println("HUB75: DMA begin FAILED (not enough memory? try lower colour depth)");
      return false;
    }
    _dma->setBrightness8(_bright);
    _dma->clearScreen();
    _ready = true;
    _force = true;
    Serial.println("HUB75: running - " + tuningText());
    return true;
  }

  // The DMA refreshes the panels by itself - nothing to start.
  void startTask(BaseType_t core = 1) { (void)core; }

  // ------------------------------------------------------------------
  // Drawing (Adafruit_GFX). The picture is kept as RGB565.
  void drawPixel(int16_t x, int16_t y, uint16_t color) override
  {
    if (!_fb || x < 0 || y < 0 || x >= _W || y >= PANEL_H) return;
    _fb[y * _W + x] = color;
  }

  void fillScreen(uint16_t color) override
  {
    if (!_fb) return;
    for (size_t i = 0; i < (size_t)_W * PANEL_H; i++) _fb[i] = color;
  }

  void clearScreen() { fillScreen(0); }

  // Send the current drawing to the panels (only the changed pixels)
  void show()
  {
    if (!_ready) return;
    xSemaphoreTake(_mtx, portMAX_DELAY);
    for (uint16_t y = 0; y < PANEL_H; y++)
    {
      for (uint16_t x = 0; x < _W; x++)
      {
        size_t i = (size_t)y * _W + x;
        uint16_t c = _fb[i];
        if (_force || c != _shown[i])
        {
          uint8_t k = x / PANEL_W;                  // panel number in the picture
          if (_rev) k = _chain - 1 - k;             // chain wired the other way
          int16_t dx = k * PANEL_W + x % PANEL_W, dy = y;
          if (_t.scan)                              // 1/8 scan remapping
          {
            bool firstHalf = ((y & 8) == 0);
            if (_t.scan == 2) firstHalf = !firstHalf;
            dx += (firstHalf ? (k + 1) : k) * PANEL_W;
            dy  = (y >> 4) * 8 + (y & 7);
          }
          _dma->drawPixel(dx, dy, c);
          _shown[i] = c;
        }
      }
    }
    _force = false;
    _uploads++;
    xSemaphoreGive(_mtx);
  }

  static uint16_t color565(uint8_t r, uint8_t g, uint8_t b)
  {
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
  }

  void setBrightness8(uint8_t b)
  {
    _bright = b;
    if (_ready) _dma->setBrightness8(b);
  }

  // Panel order in the chain (true = the other way round)
  void setChainReverse(bool r)   { _rev = r; _force = true; }

  // FM6373-style settings: handled on /config now, no effect here
  void setPattern(uint8_t p)        { (void)p; }
  void setRegSet(uint8_t s)         { (void)s; }
  void setRawTest(int8_t row)       { (void)row; }
  void setDmaClockInvert(bool inv)  { (void)inv; }
  void setDmaMHz(uint8_t mhz)       { (void)mhz; }

  uint8_t  getPattern()        const { return _t.scan; }
  uint8_t  getRegSet()         const { return 0; }
  bool     getChainReverse()   const { return _rev; }
  uint32_t getUploadCount()    const { return _uploads; }
  bool     isDMA()             const { return _ready; }
  bool     getDmaClockInvert() const { return _t.edgeA; }
  uint8_t  getDmaMHz()         const { return _t.mhz; }
  uint32_t getDmaFailures()    const { return 0; }
  bool     dmaFellBack()       const { return false; }

  // ------------------------------------------------------------------
  // TUNING  (used by the /config page of the sketch)
  // ------------------------------------------------------------------

  // One line with all current settings (Serial Monitor and /status)
  String tuningText() const
  {
    static const char *scanName[] = { "1/16", "1/8", "1/8 swapped" };
    static const char *drvName[]  = { "shift register", "FM6124", "FM6126A", "ICN2038S", "MBI5124" };
    return String("scan ") + scanName[_t.scan] + ", " + _t.mhz + " MHz, edge " +
           (_t.edgeA ? "A" : "B") + ", latch " + _t.latch + ", refresh >= " +
           _t.refresh + " Hz, depth " + _t.depth + " bit, driver " + drvName[_t.driver];
  }

  // HTML form for the /config page
  String tuningHtml() const
  {
    String h = "<h2>HUB75 panel tuning</h2>";
    h += "<p>Now: <b>" + tuningText() + "</b></p>";
    h += "<form action='/config' method='get'>";
    h += "<input type='hidden' name='hub' value='1'>";

    h += "Scan mode: " + sel("hscan", _t.scan,
           "0:1/16 (normal)|1:1/8|2:1/8 halves swapped") + "<br><br>";
    h += "Clock speed: " + sel("hmhz", _t.mhz,
           "5:5 MHz|8:8 MHz (default)|10:10 MHz|15:15 MHz|20:20 MHz") + "<br><br>";
    h += "Clock edge: " + sel("hedge", _t.edgeA ? 1 : 0,
           "0:B|1:A (default)") + "<br><br>";
    h += "Latch blanking: " + sel("hlat", _t.latch,
           "1:1|2:2|3:3|4:4|5:5|6:6|7:7|8:8") +
         " <small>(higher = less ghosting / colours in black areas, slightly dimmer)</small><br><br>";
    h += "Minimum refresh: " + sel("hrefresh", _t.refresh,
           "30:30 Hz|60:60 Hz|90:90 Hz|120:120 Hz|180:180 Hz|240:240 Hz") +
         " <small>(higher = less flicker, but fewer colours / dark LEDs may switch off)</small><br><br>";
    h += "Colour depth: " + sel("hdepth", _t.depth,
           "2:2 bit|3:3 bit|4:4 bit|5:5 bit|6:6 bit|7:7 bit|8:8 bit") +
         " <small>(lower = faster refresh, fewer colour steps)</small><br><br>";
    h += "Driver chip: " + sel("hdrv", _t.driver,
           "0:Shift register (normal)|1:FM6124|2:FM6126A|3:ICN2038S|4:MBI5124") + "<br><br>";

    h += "<button type='submit'>Save &amp; Restart</button></form>";
    h += "<p><a href='/config?hub=1&hdefault=1'>Back to default tuning (1/8, 8 MHz, edge A, latch 2, 60 Hz, 8 bit)</a></p>";
    h += "<p>Check after the restart: "
         "<a href='/panel?test=1'>test pattern</a> | "
         "<a href='/color?c=black'>black</a> | <a href='/color?c=white'>white</a> | "
         "<a href='/color?c=red'>red</a> | <a href='/color?c=green'>green</a> | "
         "<a href='/color?c=blue'>blue</a> | "
         "<a href='/takeslip'>take slip</a> | <a href='/idle'>idle</a></p>";
    return h;
  }

  // Save the values from a /config?hub=1&... request (then restart)
  void saveTuning(const String &request)
  {
    UfdTuning t = _t;
    if (request.indexOf("hdefault=1") >= 0)
    {
      t = UfdTuning();
    }
    else
    {
      String v;
      if ((v = param(request, "hscan")).length())    t.scan    = v.toInt();
      if ((v = param(request, "hmhz")).length())     t.mhz     = v.toInt();
      if ((v = param(request, "hedge")).length())    t.edgeA   = v.toInt() != 0;
      if ((v = param(request, "hlat")).length())     t.latch   = v.toInt();
      if ((v = param(request, "hrefresh")).length()) t.refresh = v.toInt();
      if ((v = param(request, "hdepth")).length())   t.depth   = v.toInt();
      if ((v = param(request, "hdrv")).length())     t.driver  = v.toInt();
    }
    clampTuning(t);

    Preferences pr;
    pr.begin("hub75", false);
    pr.putUChar("scan", t.scan);
    pr.putUChar("mhz", t.mhz);
    pr.putBool("edgeA", t.edgeA);
    pr.putUChar("latch", t.latch);
    pr.putUShort("refresh", t.refresh);
    pr.putUChar("depth", t.depth);
    pr.putUChar("driver", t.driver);
    pr.end();
    _t = t;
    Serial.println("HUB75: tuning saved - " + tuningText());
  }

private:
  void loadTuning()
  {
    UfdTuning d;
    Preferences pr;
    pr.begin("hub75", true);
    _t.scan    = pr.getUChar("scan", d.scan);
    _t.mhz     = pr.getUChar("mhz", d.mhz);
    _t.edgeA   = pr.getBool("edgeA", d.edgeA);
    _t.latch   = pr.getUChar("latch", d.latch);
    _t.refresh = pr.getUShort("refresh", d.refresh);
    _t.depth   = pr.getUChar("depth", d.depth);
    _t.driver  = pr.getUChar("driver", d.driver);
    pr.end();
    clampTuning(_t);
  }

  static void clampTuning(UfdTuning &t)
  {
    if (t.scan > 2) t.scan = 1;
    if (t.mhz != 5 && t.mhz != 8 && t.mhz != 10 && t.mhz != 15 && t.mhz != 20) t.mhz = 8;
    t.latch   = constrain(t.latch, 1, 8);
    t.refresh = constrain(t.refresh, 30, 240);
    t.depth   = constrain(t.depth, 2, 8);
    if (t.driver > 4) t.driver = 0;
  }

  // <select> from "value:label|value:label|..."
  static String sel(const char *name, int current, const char *options)
  {
    String h = String("<select name='") + name + "'>";
    String all = options;
    int start = 0;
    while (start < (int)all.length())
    {
      int end = all.indexOf('|', start);
      if (end < 0) end = all.length();
      String item = all.substring(start, end);
      int colon = item.indexOf(':');
      String val = item.substring(0, colon), label = item.substring(colon + 1);
      h += "<option value='" + val + "'" + (val.toInt() == current ? " selected" : "") +
           ">" + label + "</option>";
      start = end + 1;
    }
    return h + "</select>";
  }

  // Value of ?key=... or &key=... in the request line
  static String param(const String &req, const char *key)
  {
    String k = String(key) + "=";
    int s = req.indexOf("?" + k);
    if (s < 0) s = req.indexOf("&" + k);
    if (s < 0) return "";
    s += k.length() + 1;
    int e = s;
    while (e < (int)req.length() && req[e] != '&' && req[e] != ' ') e++;
    return req.substring(s, e);
  }

  UfdPanel_Pins _p;
  uint8_t  _chain;
  uint16_t _W = PANEL_W;
  UfdTuning _t;

  MatrixPanel_I2S_DMA *_dma = nullptr;
  uint16_t *_fb    = nullptr;     // picture being drawn
  uint16_t *_shown = nullptr;     // what the panels show now
  SemaphoreHandle_t _mtx = nullptr;

  volatile bool _ready = false, _force = true, _rev = false;
  uint8_t  _bright = 255;
  uint32_t _uploads = 0;
};

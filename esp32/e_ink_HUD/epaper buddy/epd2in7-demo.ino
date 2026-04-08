#include <SPI.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Arduino_JSON.h>
#include <time.h>
#include <WebServer.h>

#include "epd2in7.h"
#include "epdpaint.h"

#define COLORED     0
#define UNCOLORED   1

// ==== CONFIG (edit these) ====
static const char* WIFI_SSID = "edison science corner";
static const char* WIFI_PASS = "eeeeeeee";
static const char* OWM_API_KEY = "3a8986a158746450fd30abea7f45694d";
static const char* OWM_CITY = "Idukki";
static const char* OWM_COUNTRY = "IN";
static const char* TZ_STRING = "IST-5:30"; // example: "IST-5:30"
static const char* NTP_SERVER = "pool.ntp.org";

// ==== UI / INPUT ====
static const int BUTTON_PIN = 39; // GPIO39 (input-only)
#define BUTTON_ACTIVE_LOW 1       // set to 0 if your button is active-high
static const unsigned long PAGE_REFRESH_MS = 60UL * 1000UL;
static const unsigned long WEATHER_UPDATE_MS = 60UL * 1000UL;
static const unsigned long BUTTON_DEBOUNCE_MS = 80;

Epd epd;

// Full screen buffer: 176 * 264 / 8 = 5808 bytes (fits on ESP32).
static unsigned char screenBuf[EPD_WIDTH * EPD_HEIGHT / 8];
Paint paint(screenBuf, EPD_WIDTH, EPD_HEIGHT);

struct ForecastDay {
  char dayName[4];   // "Mon"
  int tempC = 0;
  char main[16];     // "Clear"
};

static float temperatureC = 0.0f;
static float feelsLikeC = 0.0f;
static int humidityPct = 0;
static char weatherMain[16] = "N/A";
static char weatherDesc[32] = "N/A";
static ForecastDay forecast[3];
static const int TODO_MAX_ITEMS = 5;
static String todoItems[TODO_MAX_ITEMS];
static int todoCount = 0;
static void renderAndShow();

// GFXfont compatibility types (for Orbitron font headers).
typedef struct {
  uint16_t bitmapOffset;
  uint8_t width;
  uint8_t height;
  uint8_t xAdvance;
  int8_t xOffset;
  int8_t yOffset;
} GFXglyph;

typedef struct {
  uint8_t* bitmap;
  GFXglyph* glyph;
  uint16_t first;
  uint16_t last;
  uint8_t yAdvance;
} GFXfont;

#include "Orbitron_Medium_20.h"
#include "orbitron44.h"
WebServer webServer(80);

static int canvasWidth() {
  return (paint.GetRotate() == ROTATE_90 || paint.GetRotate() == ROTATE_270) ? paint.GetHeight() : paint.GetWidth();
}
static int canvasHeight() {
  return (paint.GetRotate() == ROTATE_90 || paint.GetRotate() == ROTATE_270) ? paint.GetWidth() : paint.GetHeight();
}

static int textPixelWidth(const char* s, sFONT* font) {
  if (!s) return 0;
  int len = 0;
  while (s[len] != 0) len++;
  return len * font->Width;
}

static int textPixelWidthGfx(const char* s, const GFXfont* font) {
  if (!s || !font) return 0;
  int x = 0;
  while (*s) {
    uint8_t c = (uint8_t)(*s++);
    if (c >= font->first && c <= font->last) {
      const GFXglyph* g = &font->glyph[c - font->first];
      x += g->xAdvance;
    }
  }
  return x;
}

static int gfxAscent(const GFXfont* font) {
  if (!font) return 0;
  uint16_t idxA = (uint16_t)('A');
  if (idxA >= font->first && idxA <= font->last) {
    const GFXglyph* g = &font->glyph[idxA - font->first];
    return (g->yOffset < 0) ? -g->yOffset : font->yAdvance;
  }
  return font->yAdvance;
}

static void drawCharAtGfx(int x, int yBaseline, char c, const GFXfont* font, int colored) {
  if (!font) return;
  uint8_t uc = (uint8_t)c;
  if (uc < font->first || uc > font->last) return;

  const GFXglyph* glyph = &font->glyph[uc - font->first];
  const uint8_t* bitmap = font->bitmap;

  uint16_t bo = glyph->bitmapOffset;
  uint8_t bits = 0;
  uint8_t bit = 0;

  for (uint8_t yy = 0; yy < glyph->height; yy++) {
    for (uint8_t xx = 0; xx < glyph->width; xx++) {
      if ((bit++ & 7) == 0) {
        bits = pgm_read_byte(bitmap + bo++);
      }
      if (bits & 0x80) {
        paint.DrawPixel(x + glyph->xOffset + xx, yBaseline + glyph->yOffset + yy, colored);
      }
      bits <<= 1;
    }
  }
}

static void drawStringAtGfx(int x, int yTop, const char* text, const GFXfont* font, int colored) {
  if (!text || !font) return;
  int cursorX = x;
  int baselineY = yTop + gfxAscent(font);
  const char* p = text;
  while (*p) {
    uint8_t c = (uint8_t)(*p);
    if (c >= font->first && c <= font->last) {
      const GFXglyph* g = &font->glyph[c - font->first];
      drawCharAtGfx(cursorX, baselineY, *p, font, colored);
      cursorX += g->xAdvance;
    }
    p++;
  }
}

static void sendScreenBuffer() {
  // Send full buffer in strips to EPD SRAM, then refresh.
  const int stripH = 24;
  unsigned char strip[EPD_WIDTH * stripH / 8];
  Paint stripPaint(strip, EPD_WIDTH, stripH);

  for (int y = 0; y < EPD_HEIGHT; y += stripH) {
    int h = stripH;
    if (y + h > EPD_HEIGHT) h = EPD_HEIGHT - y;
    stripPaint.SetHeight(h);

    // Copy raw bytes row-wise
    const int bytesPerRow = EPD_WIDTH / 8;
    for (int yy = 0; yy < h; yy++) {
      memcpy(stripPaint.GetImage() + yy * bytesPerRow,
             screenBuf + (y + yy) * bytesPerRow,
             bytesPerRow);
    }

    epd.TransmitPartialData(stripPaint.GetImage(), 0, y, EPD_WIDTH, h);
  }
  epd.DisplayFrame();
}

static bool connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < 15000) {
    delay(200);
  }
  return WiFi.status() == WL_CONNECTED;
}

static void setupTime() {
  configTime(0, 0, NTP_SERVER);
  setenv("TZ", TZ_STRING, 1);
  tzset();
}

static void toTitleCaseFirst(char* s) {
  if (s && s[0] >= 'a' && s[0] <= 'z') s[0] = (char)(s[0] - 32);
}

static bool fetchWeatherAndForecast() {
  if (WiFi.status() != WL_CONNECTED) return false;

  HTTPClient http;

  // --- Current weather ---
  String url = String("http://api.openweathermap.org/data/2.5/weather?q=") +
               OWM_CITY + "," + OWM_COUNTRY +
               "&appid=" + OWM_API_KEY + "&units=metric";
  http.begin(url);
  bool ok = false;
  if (http.GET() == 200) {
    JSONVar obj = JSON.parse(http.getString());
    if (JSON.typeof(obj) != "undefined") {
      temperatureC = (double)obj["main"]["temp"];
      feelsLikeC = (double)obj["main"]["feels_like"];
      humidityPct = (int)obj["main"]["humidity"];

      const char* mainStr = (const char*)obj["weather"][0]["main"];
      const char* descStr = (const char*)obj["weather"][0]["description"];
      if (mainStr) {
        strncpy(weatherMain, mainStr, sizeof(weatherMain) - 1);
        weatherMain[sizeof(weatherMain) - 1] = 0;
      }
      if (descStr) {
        strncpy(weatherDesc, descStr, sizeof(weatherDesc) - 1);
        weatherDesc[sizeof(weatherDesc) - 1] = 0;
        toTitleCaseFirst(weatherDesc);
      }
      ok = true;
    }
  }
  http.end();

  // --- 3-day forecast (use same approach as deskbuddy: pick midday-ish entries) ---
  url = String("http://api.openweathermap.org/data/2.5/forecast?q=") +
        OWM_CITY + "," + OWM_COUNTRY +
        "&appid=" + OWM_API_KEY + "&units=metric";
  http.begin(url);
  if (http.GET() == 200) {
    JSONVar fo = JSON.parse(http.getString());
    if (JSON.typeof(fo) != "undefined") {
      struct tm t;
      if (getLocalTime(&t)) {
        const char* days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
        int today = t.tm_wday;
        int indices[3] = {7, 15, 23};
        for (int i = 0; i < 3; i++) {
          int idx = indices[i];
          forecast[i].tempC = (int)(double)fo["list"][idx]["main"]["temp"];

          const char* m = (const char*)fo["list"][idx]["weather"][0]["main"];
          if (m) {
            strncpy(forecast[i].main, m, sizeof(forecast[i].main) - 1);
            forecast[i].main[sizeof(forecast[i].main) - 1] = 0;
          } else {
            strncpy(forecast[i].main, "N/A", sizeof(forecast[i].main) - 1);
            forecast[i].main[sizeof(forecast[i].main) - 1] = 0;
          }

          int nextDayIndex = (today + i + 1) % 7;
          strncpy(forecast[i].dayName, days[nextDayIndex], sizeof(forecast[i].dayName) - 1);
          forecast[i].dayName[sizeof(forecast[i].dayName) - 1] = 0;
        }
      }
    }
  }
  http.end();

  return ok;
}

enum PageId : uint8_t { PAGE_FACE = 0, PAGE_TIME = 1, PAGE_CURRENT = 2, PAGE_FORECAST = 3, PAGE_TODO = 4, PAGE_INFO = 5 };
static PageId currentPage = PAGE_FACE;
static unsigned long lastPageSwitchMs = 0;
static unsigned long lastWeatherUpdateMs = 0;
static int lastButtonStable = 0;
static int lastButtonRead = 0;
static unsigned long lastButtonChangeMs = 0;

static int readButtonPressed() {
  int v = digitalRead(BUTTON_PIN);
#if BUTTON_ACTIVE_LOW
  return v == LOW ? 1 : 0;
#else
  return v == HIGH ? 1 : 0;
#endif
}

static bool buttonPressedEdge() {
  int r = readButtonPressed();
  unsigned long now = millis();
  if (r != lastButtonRead) {
    lastButtonRead = r;
    lastButtonChangeMs = now;
  }
  if ((now - lastButtonChangeMs) > BUTTON_DEBOUNCE_MS && lastButtonStable != lastButtonRead) {
    int prev = lastButtonStable;
    lastButtonStable = lastButtonRead;
    return (prev == 0 && lastButtonStable == 1);
  }
  return false;
}

static void nextPage() {
  currentPage = (PageId)(((int)currentPage + 1) % 6);
  lastPageSwitchMs = millis();
}

static void drawCharAtScaled(int x, int y, char ascii_char, sFONT* font, int colored, int scale) {
  if (scale <= 1) {
    paint.DrawCharAt(x, y, ascii_char, font, colored);
    return;
  }
  const int bytesPerRow = (font->Width / 8) + ((font->Width % 8) ? 1 : 0);
  unsigned int char_offset = (ascii_char - ' ') * font->Height * bytesPerRow;
  const unsigned char* ptr = &font->table[char_offset];

  for (int j = 0; j < (int)font->Height; j++) {
    const unsigned char* rowPtr = ptr + j * bytesPerRow;
    for (int i = 0; i < (int)font->Width; i++) {
      const unsigned char b = pgm_read_byte(rowPtr + (i / 8));
      if (b & (0x80 >> (i % 8))) {
        const int px = x + i * scale;
        const int py = y + j * scale;
        for (int dy = 0; dy < scale; dy++) {
          for (int dx = 0; dx < scale; dx++) {
            paint.DrawPixel(px + dx, py + dy, colored);
          }
        }
      }
    }
  }
}

static void drawStringAtScaled(int x, int y, const char* text, sFONT* font, int colored, int scale) {
  if (!text) return;
  int refcolumn = x;
  const char* p = text;
  while (*p != 0) {
    drawCharAtScaled(refcolumn, y, *p, font, colored, scale);
    refcolumn += font->Width * scale;
    p++;
  }
}

static void drawFilledRoundedSquare(int x, int y, int size, int radius, int colored) {
  if (size <= 0) return;
  if (radius < 0) radius = 0;
  if (radius > (size / 2)) radius = size / 2;

  if (radius == 0) {
    paint.DrawFilledRectangle(x, y, x + size - 1, y + size - 1, colored);
    return;
  }

  // Middle bands.
  paint.DrawFilledRectangle(x + radius, y, x + size - radius - 1, y + size - 1, colored);
  paint.DrawFilledRectangle(x, y + radius, x + size - 1, y + size - radius - 1, colored);

  // Corner rounds.
  paint.DrawFilledCircle(x + radius, y + radius, radius, colored);
  paint.DrawFilledCircle(x + size - radius - 1, y + radius, radius, colored);
  paint.DrawFilledCircle(x + radius, y + size - radius - 1, radius, colored);
  paint.DrawFilledCircle(x + size - radius - 1, y + size - radius - 1, radius, colored);
}

// ========== Weather Icon Drawing (merged from logo demo) ==========

static void addcloud(int x, int y, int scale, int linesize) {
  paint.DrawFilledCircle(x - scale * 3, y, scale, COLORED);
  paint.DrawFilledCircle(x + scale * 3, y, scale, COLORED);
  paint.DrawFilledCircle(x - scale, y - scale, scale * 1.4, COLORED);
  paint.DrawFilledCircle(x + scale * 1.5, y - scale * 1.3, scale * 1.75, COLORED);
  paint.DrawFilledRectangle(x - scale * 3 - 1, y - scale, x - scale * 3 - 1 + scale * 6 - 1, y - scale + scale * 2, COLORED);
  paint.DrawFilledCircle(x - scale * 3, y, scale - linesize, UNCOLORED);
  paint.DrawFilledCircle(x + scale * 3, y, scale - linesize, UNCOLORED);
  paint.DrawFilledCircle(x - scale, y - scale, scale * 1.4 - linesize, UNCOLORED);
  paint.DrawFilledCircle(x + scale * 1.5, y - scale * 1.3, scale * 1.75 - linesize, UNCOLORED);
  paint.DrawFilledRectangle(x - scale * 3 + 2, y - scale + linesize - 1,
                            x - scale * 3 + 2 + (int)(scale * 5.9) - 1,
                            y - scale + linesize - 1 + scale * 2 - linesize * 2 + 1, UNCOLORED);
}

static void addsun(int x, int y, int scale) {
  int ls = 2;
  paint.DrawFilledRectangle(x - scale * 2, y, x - scale * 2 + scale * 4 - 1, y + ls - 1, COLORED);
  paint.DrawFilledRectangle(x, y - scale * 2, x + ls - 1, y - scale * 2 + scale * 4 - 1, COLORED);
  paint.DrawLine(x - scale * 1.3, y - scale * 1.3, x + scale * 1.3, y + scale * 1.3, COLORED);
  paint.DrawLine(x - scale * 1.3, y + scale * 1.3, x + scale * 1.3, y - scale * 1.3, COLORED);
  paint.DrawLine(1 + x - scale * 1.3, y - scale * 1.3, 1 + x + scale * 1.3, y + scale * 1.3, COLORED);
  paint.DrawLine(1 + x - scale * 1.3, y + scale * 1.3, 1 + x + scale * 1.3, y - scale * 1.3, COLORED);
  paint.DrawFilledCircle(x, y, scale * 1.3, UNCOLORED);
  paint.DrawFilledCircle(x, y, scale, COLORED);
  paint.DrawFilledCircle(x, y, scale - ls, UNCOLORED);
}

static void addrain(int x, int y, int scale) {
  for (int i = 0; i < 6; i++) {
    paint.DrawLine(x - scale * 4 + scale * i * 1.3, y + scale * 2.5,
                   x - scale * 3.5 + scale * i * 1.3, y + scale * 1.2, COLORED);
    paint.DrawLine(x - scale * 4 + scale * i * 1.3 + 1, y + scale * 2.5,
                   x - scale * 3.5 + scale * i * 1.3 + 1, y + scale * 1.2, COLORED);
  }
}

static void addsnow(int x, int y, int scale) {
  int gap = scale * 1.5;
  for (int i = 0; i < 5; i++)
    paint.DrawFilledCircle(x - scale * 3 + i * gap, y + scale * 1.8, scale * 0.35, COLORED);
  for (int i = 0; i < 4; i++)
    paint.DrawFilledCircle(x - scale * 2.2 + i * gap, y + scale * 2.8, scale * 0.35, COLORED);
}

static void addtstorm(int x, int y, int scale) {
  y += scale / 2;
  for (int i = 0; i < 5; i++) {
    paint.DrawLine(x - scale * 4 + scale * i * 1.5, y + scale * 1.5,
                   x - scale * 3.5 + scale * i * 1.5, y + scale, COLORED);
    paint.DrawLine(x - scale * 4 + scale * i * 1.5 + 1, y + scale * 1.5,
                   x - scale * 3.5 + scale * i * 1.5 + 1, y + scale, COLORED);
    paint.DrawLine(x - scale * 4 + scale * i * 1.5, y + scale * 1.5,
                   x - scale * 3 + scale * i * 1.5, y + scale * 1.5, COLORED);
    paint.DrawLine(x - scale * 3.5 + scale * i * 1.4, y + scale * 2.5,
                   x - scale * 3 + scale * i * 1.5, y + scale * 1.5, COLORED);
  }
}

static void addfog(int x, int y, int scale, int linesize) {
  for (int i = 0; i < 3; i++)
    paint.DrawFilledRectangle(x - scale * 3, y + (int)(scale * (1.5 + i * 0.5)),
                              x + scale * 3 - 1, y + (int)(scale * (1.5 + i * 0.5)) + linesize - 1, COLORED);
}

static void drawWeatherIcon(const char* w, int x, int y, int scale, int linesize) {
  if (strcmp(w, "Clear") == 0)             addsun(x, y, scale * 1.6);
  else if (strcmp(w, "Clouds") == 0) {
    addcloud(x + scale, y - scale * 1.5, scale / 2, linesize);
    addcloud(x, y, scale, linesize);
  }
  else if (strcmp(w, "Rain") == 0)       { addcloud(x, y, scale, linesize); addrain(x, y, scale); }
  else if (strcmp(w, "Drizzle") == 0)    { addsun(x - scale * 1.8, y - scale * 1.8, scale); addcloud(x, y, scale, linesize); addrain(x, y, scale); }
  else if (strcmp(w, "Thunderstorm") == 0){ addcloud(x, y, scale, linesize); addtstorm(x, y, scale); }
  else if (strcmp(w, "Snow") == 0)       { addcloud(x, y, scale, linesize); addsnow(x, y, scale); }
  else if (strcmp(w, "Mist") == 0 || strcmp(w, "Fog") == 0)
    { addcloud(x, y - scale * 0.5, scale, linesize); addfog(x, y - scale * 0.5, scale, linesize); }
  else if (strcmp(w, "Haze") == 0 || strcmp(w, "Smoke") == 0 || strcmp(w, "Dust") == 0)
    { addsun(x, y - scale * 0.5, scale * 1.4); addfog(x, y - scale * 0.5, scale * 1.4, linesize); }
  else { addsun(x - scale * 1.8, y - scale * 1.8, scale); addcloud(x, y, scale, linesize); }
}

static void renderPageFace() {
  paint.Clear(UNCOLORED);
  const int cw = canvasWidth();
  const int ch = canvasHeight();

  // Face page with only two centered eyes.
  int eyeSize = ((ch / 4) * 3 * 3) / 2; // 1.5x larger than current size
  if (eyeSize < 45) eyeSize = 45;
  if (eyeSize > 75) eyeSize = 75;
  int eyeRadius = eyeSize / 6;
  if (eyeRadius < 4) eyeRadius = 4;

  const int eyeGap = eyeSize / 3;
  const int faceWidth = eyeSize * 2 + eyeGap;
  const int leftEyeX = (cw - faceWidth) / 2;
  const int rightEyeX = leftEyeX + eyeSize + eyeGap;
  int eyeY = (ch - eyeSize) / 2;
  if (eyeY < 0) eyeY = 0;

  drawFilledRoundedSquare(leftEyeX, eyeY, eyeSize, eyeRadius, COLORED);
  drawFilledRoundedSquare(rightEyeX, eyeY, eyeSize, eyeRadius, COLORED);
}

static void renderPageTime() {
  paint.Clear(UNCOLORED);
  struct tm t;
  if (!getLocalTime(&t)) {
    const char* msg = "Syncing time...";
    drawStringAtGfx((canvasWidth() - textPixelWidthGfx(msg, &Orbitron_Medium_20)) / 2,
                    (canvasHeight() - Orbitron_Medium_20.yAdvance) / 2,
                    msg, &Orbitron_Medium_20, COLORED);
    return;
  }

  int h12 = t.tm_hour % 12;
  if (h12 == 0) h12 = 12;
  char timeStr[10];
  snprintf(timeStr, sizeof(timeStr), "%02d:%02d", h12, t.tm_min);
  char dateStr[32];
  strftime(dateStr, sizeof(dateStr), "%a, %b %d", &t);

  int timeX = (canvasWidth() - textPixelWidthGfx(timeStr, &Orbitron_Bold_44)) / 2;
  int timeY = (canvasHeight() / 2) - (Orbitron_Bold_44.yAdvance / 2);
  if (timeY < 0) timeY = 0;
  drawStringAtGfx(timeX, timeY, timeStr, &Orbitron_Bold_44, COLORED);

  int dateX = (canvasWidth() - textPixelWidthGfx(dateStr, &Orbitron_Medium_20)) / 2;
  int dateY = timeY + Orbitron_Bold_44.yAdvance + 8;
  drawStringAtGfx(dateX, dateY, dateStr, &Orbitron_Medium_20, COLORED);
}

static void renderPageCurrent() {
  paint.Clear(UNCOLORED);
  char line[64];
  const int cw = canvasWidth();

  // City name at top
  snprintf(line, sizeof(line), "%s, %s", OWM_CITY, OWM_COUNTRY);
  drawStringAtGfx((cw - textPixelWidthGfx(line, &Orbitron_Medium_20)) / 2, 14, line, &Orbitron_Medium_20, COLORED);
  paint.DrawLine(10, 40, cw - 10, 40, COLORED);

  // Weather icon on the left half
  drawWeatherIcon(weatherMain, 40, 100, 7, 2);

  // Temperature (large) on the right
  snprintf(line, sizeof(line), "%dC", (int)temperatureC);
  drawStringAtGfx(80, 40, line, &Orbitron_Bold_44, COLORED);

  // Details
  paint.DrawStringAt(82, 96, weatherDesc, &Font16, COLORED);
  snprintf(line, sizeof(line), "Feels: %dC", (int)feelsLikeC);
  paint.DrawStringAt(82, 120, line, &Font16, COLORED);
  snprintf(line, sizeof(line), "Hum: %d%%", humidityPct);
  paint.DrawStringAt(82, 144, line, &Font16, COLORED);
}

static void renderPageForecast() {
  paint.Clear(UNCOLORED);
  const int cw = canvasWidth();
  const char* title = "Forecast";
  drawStringAtGfx((cw - textPixelWidthGfx(title, &Orbitron_Medium_20)) / 2, 12, title, &Orbitron_Medium_20, COLORED);
  paint.DrawLine(10, 38, cw - 10, 38, COLORED);

  char line[64];
  const int rowY[] = {40, 86, 132};
  for (int i = 0; i < 3; i++) {
    // Small weather icon on the left
    drawWeatherIcon(forecast[i].main, 35, rowY[i] + 16, 3, 1);
    // Day + temp
    snprintf(line, sizeof(line), "%s  %dC", forecast[i].dayName, forecast[i].tempC);
    paint.DrawStringAt(70, rowY[i], line, &Font16, COLORED);
    // Weather condition
    paint.DrawStringAt(70, rowY[i] + 22, forecast[i].main, &Font16, COLORED);
  }
}

static String htmlEscape(const String& in) {
  String out;
  out.reserve(in.length() + 16);
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    if (c == '&') out += "&amp;";
    else if (c == '<') out += "&lt;";
    else if (c == '>') out += "&gt;";
    else if (c == '"') out += "&quot;";
    else out += c;
  }
  return out;
}

static void renderPageTodo() {
  paint.Clear(UNCOLORED);
  const char* title = "To-Do";
  drawStringAtGfx((canvasWidth() - textPixelWidthGfx(title, &Orbitron_Medium_20)) / 2, 12, title, &Orbitron_Medium_20, COLORED);

  if (todoCount <= 0) {
    const char* emptyMsg = "No tasks";
    paint.DrawStringAt((canvasWidth() - textPixelWidth(emptyMsg, &Font16)) / 2, 64, emptyMsg, &Font16, COLORED);
    return;
  }

  const int y0 = 34;
  const int lineH = 26;
  const int maxChars = (canvasWidth() - 12) / Font16.Width;
  for (int i = 0; i < todoCount; i++) {
    char line[96];
    snprintf(line, sizeof(line), "%d. %s", i + 1, todoItems[i].c_str());
    if ((int)strlen(line) > maxChars) {
      line[maxChars > 3 ? maxChars - 3 : 0] = 0;
      strncat(line, "...", sizeof(line) - strlen(line) - 1);
    }
    paint.DrawStringAt(6, y0 + i * lineH, line, &Font16, COLORED);
  }
}

static void handleWebRoot() {
  String html = "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>ESCLABS To-Do</title>";
  html += "<style>";
  html += "body{font-family:Arial,Helvetica,sans-serif;background:#071A2B;color:#D8F3FF;max-width:760px;margin:0 auto;padding:24px 14px}";
  html += ".hero{height:28vh;display:flex;align-items:center;justify-content:center;font-size:40px;font-weight:700;letter-spacing:2px;color:#87CEEB}";
  html += ".card{background:#0A2C44;border:1px solid #1A5678;border-radius:12px;padding:16px}";
  html += "h3{color:#A9E7FF;margin:14px 0 10px}";
  html += "input{width:72%;max-width:500px;padding:10px;border-radius:8px;border:1px solid #2B7DA8;background:#04131F;color:#D8F3FF}";
  html += "button{padding:10px 12px;margin-left:8px;border-radius:8px;border:1px solid #2B7DA8;background:#0E4568;color:#D8F3FF;cursor:pointer}";
  html += "ol{padding-left:20px}";
  html += "li{margin:10px 0;display:flex;align-items:center;justify-content:space-between;gap:10px}";
  html += ".item{flex:1;word-break:break-word}";
  html += ".del{margin-left:12px;background:#1F3242;border-color:#4F9AC0}";
  html += "</style></head><body>";
  html += "<div class='hero'>ESCLABS</div>";
  html += "<div class='card'>";
  html += "<h3>To-Do</h3>";
  html += "<form method='POST' action='/add'><input name='item' maxlength='40' placeholder='New task'><button type='submit'>Add</button></form>";
  html += "<h3>Items (" + String(todoCount) + "/" + String(TODO_MAX_ITEMS) + ")</h3><ol>";
  for (int i = 0; i < todoCount; i++) {
    html += "<li><span class='item'>" + htmlEscape(todoItems[i]) + "</span>";
    html += "<form method='POST' action='/delete' style='margin:0'>";
    html += "<input type='hidden' name='id' value='" + String(i) + "'>";
    html += "<button class='del' type='submit'>Delete</button></form></li>";
  }
  html += "</ol></div></body></html>";
  webServer.send(200, "text/html", html);
}

static void handleWebAdd() {
  if (webServer.hasArg("item") && todoCount < TODO_MAX_ITEMS) {
    String item = webServer.arg("item");
    item.trim();
    if (item.length() > 0) {
      todoItems[todoCount++] = item;
      if (currentPage == PAGE_TODO) renderAndShow();
    }
  }
  webServer.sendHeader("Location", "/", true);
  webServer.send(303, "text/plain", "");
}

static void handleWebDelete() {
  if (webServer.hasArg("id")) {
    int id = webServer.arg("id").toInt();
    if (id >= 0 && id < todoCount) {
      for (int i = id; i < todoCount - 1; i++) {
        todoItems[i] = todoItems[i + 1];
      }
      todoCount--;
      if (currentPage == PAGE_TODO) renderAndShow();
    }
  }
  webServer.sendHeader("Location", "/", true);
  webServer.send(303, "text/plain", "");
}

static void setupWebInterface() {
  webServer.on("/", HTTP_GET, handleWebRoot);
  webServer.on("/add", HTTP_POST, handleWebAdd);
  webServer.on("/delete", HTTP_POST, handleWebDelete);
  webServer.begin();
  Serial.print("Web UI: http://");
  Serial.println(WiFi.localIP());
}

static void renderPageInfo() {
  paint.Clear(UNCOLORED);
  const int cw = canvasWidth();
  const int ch = canvasHeight();
  const char* title = "System Info";
  drawStringAtGfx((cw - textPixelWidthGfx(title, &Orbitron_Medium_20)) / 2, 12, title, &Orbitron_Medium_20, COLORED);
  paint.DrawLine(10, 38, cw - 10, 38, COLORED);

  if (WiFi.status() == WL_CONNECTED) {
    String ipStr = String("IP: ") + WiFi.localIP().toString();
    paint.DrawStringAt(10, 50, ipStr.c_str(), &Font16, COLORED);
    String ssidStr = String("WiFi: ") + WIFI_SSID;
    paint.DrawStringAt(10, 74, ssidStr.c_str(), &Font16, COLORED);
  } else {
    paint.DrawStringAt(10, 50, "No WiFi", &Font16, COLORED);
  }
}

static void renderAndShow() {
  epd.ClearFrame();
  switch (currentPage) {
    case PAGE_FACE: renderPageFace(); break;
    case PAGE_TIME: renderPageTime(); break;
    case PAGE_CURRENT: renderPageCurrent(); break;
    case PAGE_FORECAST: renderPageForecast(); break;
    case PAGE_TODO: renderPageTodo(); break;
    case PAGE_INFO: renderPageInfo(); break;
  }
  sendScreenBuffer();
}

void setup() {
  Serial.begin(115200);

  if (epd.Init() != 0) {
    Serial.println("e-Paper init failed");
    return;
  }

  pinMode(BUTTON_PIN, INPUT);
  lastButtonRead = readButtonPressed();
  lastButtonStable = lastButtonRead;
  lastButtonChangeMs = millis();

  paint.SetRotate(ROTATE_90);

  epd.ClearFrame();
  paint.Clear(UNCOLORED);
  sendScreenBuffer();

  if (!connectWiFi()) {
    paint.Clear(UNCOLORED);
    const char* msg = "WiFi failed";
    paint.DrawStringAt((canvasWidth() - textPixelWidth(msg, &Font20)) / 2,
                       (canvasHeight() - Font20.Height) / 2,
                       msg, &Font20, COLORED);
    epd.ClearFrame();
    sendScreenBuffer();
    return;
  }

  setupTime();
  setupWebInterface();
  fetchWeatherAndForecast();
  lastWeatherUpdateMs = millis();
  lastPageSwitchMs = millis();
  renderAndShow();
}

void loop() {
  unsigned long now = millis();
  webServer.handleClient();

  if (buttonPressedEdge()) {
    nextPage();
    renderAndShow();
  }

  if (now - lastWeatherUpdateMs >= WEATHER_UPDATE_MS) {
    fetchWeatherAndForecast();
    lastWeatherUpdateMs = now;
    renderAndShow();
  } else if (now - lastPageSwitchMs >= PAGE_REFRESH_MS) {
    // Refresh the current page once per minute without auto-switching pages.
    lastPageSwitchMs = now;
    renderAndShow();
  }

  delay(20);
}


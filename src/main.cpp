/*
  盛岡市 気象情報表示アプリ
  ESP32 Development Board (CH340C Type-C版) + GC9A01 (240x240 円形ディスプレイ)

  ・Wi-Fi 経由で Open-Meteo API (https://open-meteo.com/) から
    無償・APIキー不要で気象情報を取得します。
  ・盛岡市 (岩手県) の現在の気象情報と、今後4日間の予報を表示します。

  ビルド環境: VS Code + PlatformIO

  必要なライブラリはすべて platformio.ini の lib_deps に記載されており、
  ビルド時に自動でダウンロード・インストールされます。
    - TFT_eSPI              (Bodmer)
    - ArduinoJson    (7.x)  (Benoit Blanchon)
    - U8g2                  (oliver / olikraus)  ※日本語表示用
    - U8g2_for_Adafruit_GFX (oliver / olikraus)  ※U8g2をAdafruit_GFX系に橋渡し
    - Adafruit GFX Library  (Adafruit)           ※上記のアダプター基底クラスに使用

  TFT_eSPI は Adafruit_GFX を継承していないため、U8g2_for_Adafruit_GFX に
  直接渡すことができません。そのため drawPixel() だけを TFT_eSPI に転送する
  簡易アダプタークラス (TFTAdafruitGFXAdapter) を介して接続しています。

  TFT_eSPI の設定 (ドライバ種別・ピン配置・フォント) は、従来の
  User_Setup.h を編集する方式ではなく、platformio.ini の
  build_flags で直接指定しています (USER_SETUP_LOADED 方式)。
  ピン配置を変更する場合は platformio.ini を編集してください。
*/

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <Adafruit_GFX.h>
#include <U8g2_for_Adafruit_GFX.h>
#include <time.h>

// ============ ユーザー設定 ============
const char* WIFI_SSID     = "YOUR_WIFI_SSID";        // 使用するWiFiの情報に書き換え
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

// 盛岡市 (岩手県) の緯度経度　（希望の場所に書き換える）
const float LAT = 39.7036f;
const float LON = 141.1527f;
const char* LOCATION_NAME_JP = "もりおか";       // 画面上部に表示する地名（漢字も指定できるがフォントが無い場合もある）

// 気象情報の更新間隔 (ミリ秒)
const unsigned long UPDATE_INTERVAL_MS = 10UL * 60UL * 1000UL; // 10分
// =======================================

#define FORECAST_DAYS 4

TFT_eSPI tft = TFT_eSPI();

// U8g2_for_Adafruit_GFX::begin() は Adafruit_GFX& を要求しますが、
// TFT_eSPI は Adafruit_GFX を継承していないため、直接渡すことができません。
// そこで drawPixel() を TFT_eSPI に転送するだけの薄いアダプタークラスを用意します。
class TFTAdafruitGFXAdapter : public Adafruit_GFX {
  public:
    explicit TFTAdafruitGFXAdapter(TFT_eSPI &display)
      : Adafruit_GFX(TFT_WIDTH, TFT_HEIGHT), tftRef(display) {}

    void drawPixel(int16_t x, int16_t y, uint16_t color) override {
      tftRef.drawPixel(x, y, color);
    }

  private:
    TFT_eSPI &tftRef;
};

TFTAdafruitGFXAdapter tftAdapter(tft);
U8G2_FOR_ADAFRUIT_GFX u8g2;

// ---- 画面の色 ----
#define COL_BG      TFT_BLACK
#define COL_TEXT    TFT_WHITE
#define COL_SUB     0x8410   // グレー
#define COL_ACCENT  0x07FF   // 水色
#define COL_WARN    0xF800   // 赤

// ---- 天気カテゴリ ----
enum WeatherCat { WCAT_CLEAR, WCAT_PCLOUDY, WCAT_CLOUDY, WCAT_FOG,
                   WCAT_RAIN, WCAT_SNOW, WCAT_THUNDER, WCAT_UNKNOWN };

struct CurrentWeather {
  float temperature = 0;
  float apparentTemp = 0;
  int   humidity = 0;
  float windSpeed = 0;
  int   weatherCode = 0;
  bool  isDay = true;
  bool  valid = false;
};

struct DailyForecast {
  int   weatherCode = 0;
  float tempMax = 0;
  float tempMin = 0;
  int   wday = 0; // 0=日曜
};

CurrentWeather gCurrent;
DailyForecast  gForecast[FORECAST_DAYS];
char gLastUpdate[6] = "--:--";
bool gWifiOk = false;
unsigned long gLastFetchMs = 0;
unsigned long gLastClockMs = 0;

const char* WDAY_JP[7] = { "日", "月", "火", "水", "木", "金", "土" };

// ---------------------------------------------------------------
// WMOの天気コード -> 日本語の説明 / アイコン種別
// ---------------------------------------------------------------
void weatherCodeToInfo(int code, String &descOut, WeatherCat &catOut) {
  switch (code) {
    case 0: descOut = "快晴"; catOut = WCAT_CLEAR; break;
    case 1: descOut = "ほぼ晴れ"; catOut = WCAT_CLEAR; break;
    case 2: descOut = "晴れ時々曇り"; catOut = WCAT_PCLOUDY; break;
    case 3: descOut = "くもり"; catOut = WCAT_CLOUDY; break;
    case 45: case 48: descOut = "霧"; catOut = WCAT_FOG; break;
    case 51: case 53: case 55: descOut = "霧雨"; catOut = WCAT_RAIN; break;
    case 56: case 57: descOut = "着氷性の霧雨"; catOut = WCAT_RAIN; break;
    case 61: case 63: case 65: descOut = "雨"; catOut = WCAT_RAIN; break;
    case 66: case 67: descOut = "着氷性の雨"; catOut = WCAT_RAIN; break;
    case 71: case 73: case 75: descOut = "雪"; catOut = WCAT_SNOW; break;
    case 77: descOut = "霧雪"; catOut = WCAT_SNOW; break;
    case 80: case 81: case 82: descOut = "にわか雨"; catOut = WCAT_RAIN; break;
    case 85: case 86: descOut = "にわか雪"; catOut = WCAT_SNOW; break;
    case 95: descOut = "雷雨"; catOut = WCAT_THUNDER; break;
    case 96: case 99: descOut = "雷雨(雹)"; catOut = WCAT_THUNDER; break;
    default: descOut = "不明"; catOut = WCAT_UNKNOWN; break;
  }
}

// ---------------------------------------------------------------
// Wi-Fi 接続
// ---------------------------------------------------------------
bool connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Wi-Fi 接続中");
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(300);
    Serial.print(".");
  }
  Serial.println();
  gWifiOk = (WiFi.status() == WL_CONNECTED);
  if (gWifiOk) {
    Serial.print("接続完了 IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("Wi-Fi 接続失敗");
  }
  return gWifiOk;
}

// ---------------------------------------------------------------
// NTP で時刻同期 (JST = UTC+9)
// ---------------------------------------------------------------
void syncTime() {
  configTime(9 * 3600, 0, "ntp.nict.jp", "ntp.jst.mfeed.ad.jp", "time.google.com");
  struct tm timeinfo;
  int retry = 0;
  while (!getLocalTime(&timeinfo) && retry < 10) {
    delay(500);
    retry++;
  }
}

// ---------------------------------------------------------------
// 日付文字列 "YYYY-MM-DD" -> 曜日 (0=日曜)
// ---------------------------------------------------------------
int dateStrToWday(const char* dateStr) {
  int y, m, d;
  if (sscanf(dateStr, "%d-%d-%d", &y, &m, &d) != 3) return 0;
  struct tm t = {0};
  t.tm_year = y - 1900;
  t.tm_mon  = m - 1;
  t.tm_mday = d;
  t.tm_hour = 12;
  time_t tt = mktime(&t);
  struct tm *res = localtime(&tt);
  return res ? res->tm_wday : 0;
}

// ---------------------------------------------------------------
// Open-Meteo API から気象情報を取得
// ---------------------------------------------------------------
bool fetchWeather() {
  if (WiFi.status() != WL_CONNECTED) return false;

  String url = "https://api.open-meteo.com/v1/forecast";
  url += "?latitude=" + String(LAT, 4);
  url += "&longitude=" + String(LON, 4);
  url += "&current=temperature_2m,relative_humidity_2m,apparent_temperature,weather_code,wind_speed_10m,is_day";
  url += "&daily=weather_code,temperature_2m_max,temperature_2m_min";
  url += "&timezone=Asia%2FTokyo";
  url += "&forecast_days=5";

  // Open-MeteoはHTTPS(TLS)のみ提供。ESP32のHTTPClientでHTTPSを使う場合は
  // WiFiClientSecureを明示的に渡す必要があります。
  // ルートCA証明書を個別に用意しない簡易運用として setInsecure() で
  // 証明書検証を省略しています(通信内容自体は暗号化されます)。
  WiFiClientSecure secureClient;
  secureClient.setInsecure();

  HTTPClient http;
  http.setTimeout(10000);
  http.setConnectTimeout(10000);
  if (!http.begin(secureClient, url)) {
    Serial.println("HTTP接続の初期化に失敗しました");
    return false;
  }

  int httpCode = http.GET();
  Serial.printf("HTTPステータスコード: %d\n", httpCode);

  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("HTTP GET失敗: %d\n", httpCode);
    http.end();
    return false;
  }

  // ストリームから直接deserializeJsonするより、一旦文字列として
  // 受信したほうが安定して解析できるため、getString()を使用します。
  String payload = http.getString();
  http.end();

  Serial.printf("受信バイト数: %d\n", payload.length());

  if (payload.length() == 0) {
    Serial.println("受信データが空です (通信または証明書の問題の可能性があります)");
    return false;
  }

  JsonDocument doc; // ArduinoJson 7: 自動サイズ管理
  DeserializationError err = deserializeJson(doc, payload);

  if (err) {
    Serial.print("JSON解析エラー: ");
    Serial.println(err.c_str());
    Serial.println("受信内容(先頭200文字):");
    Serial.println(payload.substring(0, 200));
    return false;
  }

  JsonObject cur = doc["current"];
  gCurrent.temperature  = cur["temperature_2m"]   | 0.0;
  gCurrent.apparentTemp = cur["apparent_temperature"] | 0.0;
  gCurrent.humidity     = cur["relative_humidity_2m"] | 0;
  gCurrent.windSpeed    = cur["wind_speed_10m"]   | 0.0;
  gCurrent.weatherCode  = cur["weather_code"]     | 0;
  gCurrent.isDay        = (cur["is_day"] | 1) == 1;
  gCurrent.valid        = true;

  JsonArray codes = doc["daily"]["weather_code"];
  JsonArray tmax  = doc["daily"]["temperature_2m_max"];
  JsonArray tmin  = doc["daily"]["temperature_2m_min"];
  JsonArray dates = doc["daily"]["time"];

  // index 0 は「今日」なので、予報欄には 1〜FORECAST_DAYS を使う
  for (int i = 0; i < FORECAST_DAYS; i++) {
    int srcIdx = i + 1;
    gForecast[i].weatherCode = codes[srcIdx] | 0;
    gForecast[i].tempMax = tmax[srcIdx] | 0.0;
    gForecast[i].tempMin = tmin[srcIdx] | 0.0;
    const char* ds = dates[srcIdx];
    gForecast[i].wday = ds ? dateStrToWday(ds) : 0;
  }

  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    strftime(gLastUpdate, sizeof(gLastUpdate), "%H:%M", &timeinfo);
  }

  return true;
}

// ---------------------------------------------------------------
// 天気アイコン描画 (ベクター図形、ビットマップ不要)
// ---------------------------------------------------------------
void drawWeatherIcon(int cx, int cy, int size, WeatherCat cat) {
  switch (cat) {
    case WCAT_CLEAR: {
      tft.fillCircle(cx, cy, size / 2, TFT_YELLOW);
      for (int a = 0; a < 360; a += 45) {
        float rad = a * PI / 180.0;
        int x1 = cx + cos(rad) * (size / 2 + 3);
        int y1 = cy + sin(rad) * (size / 2 + 3);
        int x2 = cx + cos(rad) * (size / 2 + size / 3);
        int y2 = cy + sin(rad) * (size / 2 + size / 3);
        tft.drawLine(x1, y1, x2, y2, TFT_YELLOW);
      }
      break;
    }
    case WCAT_PCLOUDY: {
      tft.fillCircle(cx - size / 4, cy - size / 6, size / 3, TFT_YELLOW);
      tft.fillCircle(cx + size / 6, cy + size / 6, size / 3, COL_SUB);
      tft.fillCircle(cx + size / 6 + size / 4, cy + size / 6, size / 4, COL_SUB);
      break;
    }
    case WCAT_CLOUDY: {
      tft.fillCircle(cx - size / 5, cy, size / 3, COL_SUB);
      tft.fillCircle(cx + size / 5, cy - size / 8, size / 3, TFT_LIGHTGREY);
      tft.fillCircle(cx + size / 2, cy, size / 4, COL_SUB);
      break;
    }
    case WCAT_FOG: {
      for (int i = -1; i <= 1; i++) {
        tft.drawFastHLine(cx - size / 2, cy + i * (size / 5), size, TFT_LIGHTGREY);
      }
      break;
    }
    case WCAT_RAIN: {
      tft.fillCircle(cx, cy - size / 6, size / 3, TFT_LIGHTGREY);
      for (int i = -1; i <= 1; i++) {
        int x = cx + i * (size / 4);
        tft.drawLine(x, cy + size / 6, x - 3, cy + size / 2, COL_ACCENT);
      }
      break;
    }
    case WCAT_SNOW: {
      tft.fillCircle(cx, cy - size / 6, size / 3, TFT_LIGHTGREY);
      for (int i = -1; i <= 1; i++) {
        int x = cx + i * (size / 4);
        int y = cy + size / 3;
        tft.drawLine(x - 4, y, x + 4, y, TFT_WHITE);
        tft.drawLine(x, y - 4, x, y + 4, TFT_WHITE);
      }
      break;
    }
    case WCAT_THUNDER: {
      tft.fillCircle(cx, cy - size / 6, size / 3, COL_SUB);
      int x = cx, y = cy + size / 6;
      tft.drawLine(x, y, x - 6, y + 10, TFT_YELLOW);
      tft.drawLine(x - 6, y + 10, x + 2, y + 10, TFT_YELLOW);
      tft.drawLine(x + 2, y + 10, x - 4, y + 22, TFT_YELLOW);
      break;
    }
    default:
      tft.drawCircle(cx, cy, size / 2, COL_SUB);
      break;
  }
}

// ---------------------------------------------------------------
// 日本語テキストを中央揃えで描画 (U8g2_for_Adafruit_GFX)
// ---------------------------------------------------------------
void drawCenteredJP(int cx, int y, const char* text, const uint8_t* font, uint16_t color) {
  u8g2.setFont(font);
  u8g2.setForegroundColor(color);
  u8g2.setFontMode(1); // 背景は透過
  int w = u8g2.getUTF8Width(text);
  u8g2.setCursor(cx - w / 2, y);
  u8g2.print(text);
}

// ---------------------------------------------------------------
// 画面全体を描画
// ---------------------------------------------------------------
void drawScreen() {
  tft.fillScreen(COL_BG);

  // --- 上部: 地名 + Wi-Fi状態 ---
  drawCenteredJP(120, 26, LOCATION_NAME_JP, u8g2_font_b12_t_japanese2, COL_TEXT);
  tft.fillCircle(gWifiOk ? 205 : 205, 16, 4, gWifiOk ? TFT_GREEN : COL_WARN);

  if (!gCurrent.valid) {
    drawCenteredJP(120, 120, "気象情報取得中...", u8g2_font_b12_t_japanese2, COL_SUB);
    return;
  }

  // --- 中央: 天気アイコン + 気温 ---
  String desc; WeatherCat cat;
  weatherCodeToInfo(gCurrent.weatherCode, desc, cat);

  drawWeatherIcon(72, 108, 56, cat);

  tft.setTextColor(COL_TEXT, COL_BG);
  tft.setTextDatum(TL_DATUM);
  tft.drawFloat(gCurrent.temperature, 1, 118, 84, 7); // 7セグ風フォント

  // 度記号
  int degX = 118 + tft.textWidth(String(gCurrent.temperature, 1), 7) + 6;
  tft.drawCircle(degX, 90, 4, COL_TEXT);
  drawCenteredJP(degX + 16, 96, "C", u8g2_font_b16_t_japanese2, COL_TEXT);

  // --- 天気の説明 ---
  drawCenteredJP(120, 140, desc.c_str(), u8g2_font_b16_t_japanese2, COL_ACCENT);

  // --- 体感温度 / 湿度 / 風速 ---
  char sub[48];
  snprintf(sub, sizeof(sub), "体感 %.1f℃  湿度 %d%%", gCurrent.apparentTemp, gCurrent.humidity);
  drawCenteredJP(120, 160, sub, u8g2_font_b10_t_japanese2, COL_SUB);
  char sub2[32];
  snprintf(sub2, sizeof(sub2), "風速 %.1fm/s", gCurrent.windSpeed);
  drawCenteredJP(120, 176, sub2, u8g2_font_b10_t_japanese2, COL_SUB);

  // --- 下部: 4日間の予報 ---
  int colX[FORECAST_DAYS] = { 42, 94, 146, 198 };
  for (int i = 0; i < FORECAST_DAYS; i++) {
    String fdesc; WeatherCat fcat;
    weatherCodeToInfo(gForecast[i].weatherCode, fdesc, fcat);

    drawCenteredJP(colX[i], 196, WDAY_JP[gForecast[i].wday], u8g2_font_b10_t_japanese2, COL_TEXT);
    drawWeatherIcon(colX[i], 210, 22, fcat);

    char t[16];
    snprintf(t, sizeof(t), "%.0f/%.0f", gForecast[i].tempMax, gForecast[i].tempMin);
    drawCenteredJP(colX[i], 232, t, u8g2_font_b10_t_japanese2, COL_SUB);
  }

  // --- 最終更新時刻 ---
  tft.setTextDatum(TR_DATUM);
  tft.setTextColor(COL_SUB, COL_BG);
  tft.drawString(gLastUpdate, 225, 18, 1);
}

// ---------------------------------------------------------------
// setup / loop
// ---------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(300);

  tft.init();
  tft.setRotation(0);
  tft.fillScreen(COL_BG);
  u8g2.begin(tftAdapter);

  drawCenteredJP(120, 116, "起動中...", u8g2_font_b16_t_japanese2, COL_TEXT);

  connectWiFi();
  if (gWifiOk) {
    syncTime();
    fetchWeather();
  }
  drawScreen();

  gLastFetchMs = millis();
  gLastClockMs = millis();
}

void loop() {
  unsigned long now = millis();

  // Wi-Fi切断時は再接続を試みる
  if (WiFi.status() != WL_CONNECTED) {
    gWifiOk = false;
    connectWiFi();
  }

  // 定期的に気象情報を再取得
  if (now - gLastFetchMs >= UPDATE_INTERVAL_MS) {
    gLastFetchMs = now;
    if (fetchWeather()) {
      drawScreen();
    }
  }

  delay(200);
}
 

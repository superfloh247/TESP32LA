/*
    License: CC BY-NC-SA
*/

// installed via Arduino IDE Library manager
#include <TaskScheduler.h>
#include <TaskSchedulerDeclarations.h>

// install TFT_eSPI, in TFT_eSPI/User_Setup_Select.h, comment out the default settings #include <User_Setup.h>,
// select #include <User_Setups/Setup25_TTGO_T_Display.h> , Save Settings.
#include <TFT_eSPI.h>
#include <SPI.h>
#include <WiFi.h>
#include <Wire.h>
#include <HTTPClient.h>
#include <WebServer.h>

#include <IotWebConf.h>
// disable LED blinking
#define IOTWEBCONF_STATUS_ENABLED 0
#define IOTWEBCONF_CONFIG_VERSION "v002"
#define IOTWEBCONF_STRING_PARAM_LEN 513
#define IOTWEBCONF_DEBUG_TO_SERIAL
#define IOTWEBCONF_CONFIG_USE_MDNS

// version 6.x!
#include <ArduinoJson.h>

// get this from TFT_eSPI/examples/Smooth Fonts/FLASH_Array/Smooth_font_gradient
#include "NotoSansBold36.h"
#define AA_FONT_LARGE NotoSansBold36

#include "TESP32LA_icons.h"

#ifndef TFT_DISPOFF
#define TFT_DISPOFF 0x28
#endif

#ifndef TFT_SLPIN
#define TFT_SLPIN   0x10
#endif

#define TFT_MOSI            19
#define TFT_SCLK            18
#define TFT_CS              5
#define TFT_DC              16
#define TFT_RST             23

#define TFT_BL          4  // Display backlight control pin
#define ADC_EN          14
#define ADC_PIN         34

#define DISPLAYWIDTH  135
#define DISPLAYHEIGHT 240

TFT_eSPI tft = TFT_eSPI(DISPLAYWIDTH, DISPLAYHEIGHT);
TFT_eSprite framebufferMain = TFT_eSprite(&tft);
TFT_eSprite framebufferIcons = TFT_eSprite(&tft);

Scheduler taskScheduler;
void httpClientCallback();
void updateDisplayCallback();
void iotWebConfLoopCallback();
void demoCallback();
Task httpClientTask(30000, TASK_FOREVER, &httpClientCallback); // 30 sec
Task demoTask(5000, TASK_FOREVER, &demoCallback); // 5 sec
Task updateDisplayTask(40, TASK_FOREVER, &updateDisplayCallback); // 25fps
Task iotWebConfLoopTask(100, TASK_FOREVER, &iotWebConfLoopCallback);

DNSServer dnsServer;
WebServer server(80);
HTTPUpdateServer httpUpdater;

const char thingName[] = "TESP32LA";
const char wifiInitialPassword[] = "TESP32LA";

IotWebConf iotWebConf(thingName, &dnsServer, &server, wifiInitialPassword, IOTWEBCONF_CONFIG_VERSION);
char stringParamValue[IOTWEBCONF_STRING_PARAM_LEN] = "http://teslalogger/admin/current_json.php";
void handleRoot();
void configSaved();
void wifiConnected();
boolean formValidator();
IotWebConfParameter urlParam = IotWebConfParameter("TeslaLogger URL", "urlParam", stringParamValue, IOTWEBCONF_STRING_PARAM_LEN);

String url = "http://teslalogger/admin/current_json.php";

String SoC = "n/a";
String outsideTemp = "n/a";
bool stateOnline = false;
bool stateCharging = false;
bool stateSleeping = false;
uint8_t nextOnlineIcon = 192;
bool nextOnlineIconUp = true;
uint8_t nextZZZIcon = 192;
bool nextZZZIconUp = true;
uint8_t nextChargingIcon = 255;
bool nextChargingIconUp = false;
const int maincenterx = (DISPLAYHEIGHT - 40) / 2;
const int maincentery = DISPLAYWIDTH / 2 + 32;

const size_t capacity = (JSON_OBJECT_SIZE(48) + 101) * 2; // some extra space to store currently 31 fields, and 101 bytes for ESP32 architecture
DynamicJsonDocument doc(capacity);

void setup()
{
  Serial.begin(115200);
  Serial.println("Start");
  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE);
  if (TFT_BL > 0) { // TFT_BL has been set in the TFT_eSPI library in the User Setup file TTGO_T_Display.h
    pinMode(TFT_BL, OUTPUT); // Set backlight pin to output mode
    digitalWrite(TFT_BL, TFT_BACKLIGHT_ON); // Turn backlight on. TFT_BACKLIGHT_ON has been set in the TFT_eSPI library in the User Setup file TTGO_T_Display.h
  }
  tft.setSwapBytes(true); // TODO why?
  tft.println("Starting ...");
  framebufferMain.createSprite(DISPLAYHEIGHT - 40, DISPLAYWIDTH);
  framebufferMain.loadFont(AA_FONT_LARGE);
  framebufferIcons.createSprite(40, DISPLAYWIDTH);
  iotWebConf.addParameter(&urlParam);
  iotWebConf.setConfigSavedCallback(&configSaved);
  iotWebConf.setFormValidator(&formValidator);
  iotWebConf.getApTimeoutParameter()->visible = true;
  iotWebConf.setupUpdateServer(&httpUpdater, "/update");
  iotWebConf.setWifiConnectionCallback(&wifiConnected);
  iotWebConf.init();
  tft.println("IotWebConf is set up");
  server.on("/status", []() {
    if (iotWebConf.handleCaptivePortal()) {
      return; // all set, nothing more to be done here
    }
    String html = String("<html><head><meta http-equiv=\"refresh\" content=\"30\"></head><body>");
    JsonObject root = doc.as<JsonObject>();
    for (JsonPair kv : root) {
      html += String(kv.key().c_str()) + ": " + kv.value().as<String>() + "<br />";
    }
    html += String("</body></html>");
    server.send(200, "text/html", html);
  });
  server.on("/demo", []() {
    if (iotWebConf.handleCaptivePortal()) {
      return; // all set, nothing more to be done here
    }
    String html = String("<html><head></head><body>");
    html += "<h1>DEMO mode!</h1><a href='demooff'>stop DEMO mode</a>";
    html += String("</body></html>");
    server.send(200, "text/html", html);
    demoTask.enable();
    httpClientTask.disable();
  });
  server.on("/demooff", []() {
    if (iotWebConf.handleCaptivePortal()) {
      return; // all set, nothing more to be done here
    }
    String html = String("<html><head></head><body>");
    html += "<h1>DEMO mode off</h1>";
    html += String("</body></html>");
    server.send(200, "text/html", html);
    demoTask.disable();
    httpClientTask.enable();
  });
  server.on("/", handleRoot);
  server.on("/config", [] { iotWebConf.handleConfig(); });
  server.onNotFound([]() {
    iotWebConf.handleNotFound();
  });
  taskScheduler.init();
  taskScheduler.addTask(httpClientTask);
  taskScheduler.addTask(updateDisplayTask);
  taskScheduler.addTask(iotWebConfLoopTask);
  taskScheduler.addTask(demoTask);
  iotWebConfLoopTask.enable();
  tft.println("Task Scheduler is started");
  tft.println("Setting up wifi ...");
}

void loop() {
  if (iotWebConf.getState() == IOTWEBCONF_STATE_ONLINE) {
    if (httpClientTask.isEnabled() == false && demoTask.isEnabled() == false) {
      httpClientTask.enable();
    }
    taskScheduler.execute();
  }
  else {
    iotWebConf.doLoop();
  }
}

void httpClientCallback() {
  HTTPClient http;
  http.useHTTP10(true);
  http.begin(url);
  Serial.println("[HTTP] GET...");
  int httpCode = http.GET();
  Serial.print("[HTTP] GET... code: ");
  Serial.println(httpCode);
  if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();
    if (payload.length() > 3) { // remove UTF8 BOM
      if (payload[0] == char(0xEF) && payload[1] == char(0xBB) && payload[2] == char(0xBF)) {
        Serial.println("remove BOM from JSON");
        payload = payload.substring(3);
      }
    }
    DeserializationError error = deserializeJson(doc, payload);
    if (error) {
      Serial.print(F("deserializeJson() failed: "));
      Serial.println(error.c_str());
    }
    else {
      // Extract values
      Serial.println("JSON:");
      Serial.println("online: " + doc["online"].as<String>());
      Serial.println("charging: " + doc["charging"].as<String>());
      Serial.println("sleeping: " + doc["sleeping"].as<String>());
      Serial.println("outside_temp: " + (String)doc["outside_temp"].as<float>());
      Serial.println("battery_level: " + doc["battery_level"].as<int>());
      stateOnline = doc["online"];
      stateCharging = doc["charging"];
      stateSleeping = doc["sleeping"];
      SoC = doc["battery_level"].as<int>();
      outsideTemp = ((int)(doc["outside_temp"].as<float>() * 100)) / 100.0f;
    }
  }
  else {
    Serial.print("[HTTP] GET... failed, error: ");
    Serial.println(http.errorToString(httpCode).c_str());
  }
  http.end();
  if (updateDisplayTask.isEnabled() == false) {
    updateDisplayTask.enable();
  }
}

uint16_t RGB888toRGB565(uint8_t r, uint8_t g, uint8_t b) {
  return (((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

void drawStateIcons() {
  int offset = 0;
  framebufferIcons.fillSprite(TFT_BLACK);
  if (stateOnline && !stateSleeping) { // online
    framebufferIcons.drawXBitmap(0, 0, icon_online_bits, icon_online_width, icon_online_height + offset, RGB888toRGB565(0, nextOnlineIcon, 0), TFT_BLACK);
    offset += icon_online_height;
    if (nextOnlineIcon > 250) {
      nextOnlineIconUp = false;
    }
    else if (nextOnlineIcon < 130) {
      nextOnlineIconUp = true;
    }
    nextOnlineIcon += nextOnlineIconUp ? 4 : -4;
  }
  else if (!stateOnline && stateSleeping) { // asleep
    //framebufferIcons.drawXBitmap(0, 0, icon_zzz_bits, icon_zzz_height + offset, icon_zzz_width, RGB888toRGB565(0, 0, nextZZZIcon), TFT_BLACK);
    framebufferIcons.drawXBitmap(0, 0, icon_zzz_bits, icon_zzz_width, icon_zzz_height + offset, RGB888toRGB565(0, 0, nextZZZIcon), TFT_BLACK);
    offset += icon_zzz_height;
    if (nextZZZIcon > 250) {
      nextZZZIconUp = false;
    }
    else if (nextZZZIcon < 130) {
      nextZZZIconUp = true;
    }
    nextZZZIcon += nextZZZIconUp ? 4 : -4;
  }
  else if (!stateOnline && !stateSleeping) { // offline
    framebufferIcons.drawXBitmap(0, 0, icon_online_bits, icon_online_width, icon_online_height + offset, RGB888toRGB565(nextOnlineIcon, 0, 0), TFT_BLACK);
    offset += icon_online_height;
    if (nextOnlineIcon > 250) {
      nextOnlineIconUp = false;
    }
    else if (nextOnlineIcon < 130) {
      nextOnlineIconUp = true;
    }
    nextOnlineIcon += nextOnlineIconUp ? 4 : -4;
  }
  if (stateCharging) {
    framebufferIcons.drawXBitmap(0, offset, icon_battery_bits, icon_battery_width, icon_battery_height, RGB888toRGB565(nextChargingIcon, 0, 0), TFT_BLACK);
    offset += icon_battery_height;
    if (nextChargingIcon > 250) {
      nextChargingIconUp = false;
    }
    else if (nextChargingIcon < 130) {
      nextChargingIconUp = true;
    }
    nextChargingIcon += nextChargingIconUp ? 4 : -4;
  }
  framebufferIcons.pushSprite(DISPLAYHEIGHT - 40, 0);
}

uint16_t SoCtoColor(int soc) {
  if (soc > 20 && soc <= 90) {
    return TFT_GREEN;
  }
  else if (soc > 10 && soc <= 95) {
    return TFT_YELLOW;
  }
  else {
    return TFT_RED;
  }
}

void drawGauge() {
  framebufferMain.fillSprite(TFT_BLACK);
  // draw outer line
  const float degree = 1000.0 / 57296.0;
  int radius = 83;
  for (radius = 83; radius > 79; radius--) {
    for (float alpha = 10.0 * degree; alpha >= -190.0 * degree; alpha -= degree) {
      framebufferMain.fillRect(maincenterx + radius * cos(alpha), maincentery + radius * sin(alpha), 2, 2, SoCtoColor((int)((190 + alpha / degree) / 2.0))
                              );
    }
  }
  // draw inner bar background
  for (radius = 74; radius > 58; radius--) {
    for (float alpha = 10.0 * degree; alpha >= -190.0 * degree; alpha -= degree) {
      framebufferMain.fillRect(maincenterx + radius * cos(alpha), maincentery + radius * sin(alpha), 2, 2, TFT_DARKGREY);
    }
  }
  // draw inner bar foreground
  for (radius = 74; radius > 58; radius--) {
    for (float alpha = (-190.0 + SoC.toInt() * 2) * degree; alpha >= -190.0 * degree; alpha -= degree) {
      framebufferMain.fillRect(maincenterx + radius * cos(alpha), maincentery + radius * sin(alpha), 2, 2, SoCtoColor(SoC.toInt())
                              );
    }
  }
  // draw string
  framebufferMain.setTextColor(SoCtoColor(SoC.toInt()), TFT_BLACK);
  framebufferMain.setTextDatum(MC_DATUM);
  framebufferMain.drawString(SoC + "%", maincenterx, maincentery);
  framebufferMain.pushSprite(0, 0);
}

void updateDisplayCallback() {
  drawGauge();
  drawStateIcons();
  if (demoTask.isEnabled() == true) {
    tft.setCursor(0, 0);
    tft.println("DEMO");
  }
}

void handleRoot() {
  if (iotWebConf.handleCaptivePortal()) {
    return; // all set, nothing more to be done here
  }
  String html = String("<html><head></head><body>");
  html += "<h1>TESP32LA</h1>";
  html += "Go to <a href='config'>configuration</a> page to change settings.<br />";
  html += "Start <a href='demo'>DEMO</a> mode.<br />";
  html += "Check <a href='status'>status</a>.<br />";
  html += String("</body></html>");
  server.send(200, "text/html", html);
}

void iotWebConfLoopCallback() {
  iotWebConf.doLoop();
}

void configSaved() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE);
  tft.setCursor(0, 0);
  tft.println("Wifi configuration saved");
  tft.println("TeslaLogger configuration saved");
}

boolean formValidator() {
  // some checks to validate url "http://teslalogger/admin/current_json.php";
  String url = server.arg(urlParam.getId());
  if (!url.startsWith("http")) {
    urlParam.errorMessage = "TeslaLogger URL must start with http";
    return false;
  }
  if (!url.endsWith("/admin/current_json.php")) {
    urlParam.errorMessage = "TeslaLogger URL must end with /admin/current_json.php";
    return false;
  }
  return true;
}

void wifiConnected() {
  tft.println("Wifi connected");
  httpClientTask.enable();
}

void demoCallback() {
  SoC = String(random(0, 101)); // random will be from 0 to 100
  switch (random(0, 3)) { // random will be from 0 to 2
    case 0: // charging
      stateOnline = true;
      stateCharging = true;
      stateSleeping = false;
      break;
    case 1: // online
      stateOnline = true;
      stateCharging = false;
      stateSleeping = false;
      break;
    case 2: // offline
      stateOnline = false;
      stateCharging = false;
      stateSleeping = false;
      break;
  }
  Serial.println("DEMO: SoC " + SoC + " stateOnline: " + stateOnline + " stateCharging: " + stateCharging + " stateSleeping: " + stateSleeping);
}

#include <OneWire.h>
#include <DallasTemperature.h>
#include <NTPClient.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <uri/UriBraces.h>
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <ArduinoMqttClient.h>
#include <FS.h>
#include "html_blob.h"
#include "wificreds.h"

#define TEMPERATURE_PRECISION 9

// Define your wifi creds in wificreds.h


// Consts
const char* SSID         = STASSID;
const char* WIFIPASS     = STAPSK;
const byte  PWR_RELAY    = 16;
const byte  CH_RELAY     = 14;
const byte  HW_RELAY     = 12;
const byte  THERMO_RELAY = 13;
const byte  PWR_LED      = 15;
const byte  CH_LED       = 2;
const byte  HW_LED       = 4;
const byte  PWR_SW       = 0;
const byte  CH_SW      = 1; // TX_GPIO
const byte  HW_SW      = 3; // RX_GPIO
const unsigned int MAX_RUN_TIME = 5400;

// Globals
bool ENABLED = false;
int  MAX_HW_TEMP = 5600;

// Server
ESP8266WebServer server(80);

// Time stuff
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "time"); //, utcOffsetInSeconds);
unsigned long current_epoch;
unsigned long hw_off_epoch = 0;
unsigned long ch_off_epoch = 0;
unsigned long millis_1s = 0;
unsigned long millis_10m = 0;


// MQTT Stuff
WiFiClient wifiClient;
MqttClient mqttClient(wifiClient);
const char broker[] = "smarthome";
const int  port     = 1883;
const char topic[]  = "arduino/simple";

// One Wire Stuff
OneWire ds(5);
DallasTemperature sensors(&ds);
byte temperature_device_count = 0;

// Device mappings
String device_tank_top = "28ebed3c05000096";
String device_tank_mid = "";
String device_tank_btm = "";
int16_t temperature_top = 0;
int16_t temperature_mid = 0;
int16_t temperature_btm = 0;

// SPIFFS
File f;

void logger(String message){
  unsigned int fsize = f.size();
  if (fsize > 100000) {
    f.close();
    SPIFFS.gc();
    SPIFFS.remove("/log.txt");
    f = SPIFFS.open("/log.txt", "a+");
    f.println("Log cleared");
  }
  f.seek(0,SeekEnd);
  f.print(timeClient.getFormattedTime());
  f.print(" : ");
  f.println(message);
}

void store_temperature(String device_name, int16_t temperature) {
  if (device_name == device_tank_top) temperature_top = temperature;
  if (device_name == device_tank_mid) temperature_mid = temperature;
  if (device_name == device_tank_btm) temperature_btm = temperature;
}

String getDeviceAddressString(DeviceAddress tempDeviceAddress){
  String device_address_str;
  for (uint8_t i = 0; i < 8; i++) {
        if (tempDeviceAddress[i] < 16) device_address_str += "0";
        device_address_str += String(tempDeviceAddress[i], HEX);
      }
  return device_address_str;
}

int16_t onewire_reading(){
  // Send all the onewire temperature readings to mqtt
  // Return the current maximum.  Useful for knowing when to turn off the HW.
  int16_t max_temp = 0;
  DeviceAddress tempDeviceAddress;
  sensors.requestTemperatures();
  for (int i=0; i < temperature_device_count; i++){
    if(sensors.getAddress(tempDeviceAddress, i)) {
      String device_address_str = getDeviceAddressString(tempDeviceAddress);
      int16_t raw_temp = sensors.getTemp(tempDeviceAddress);
      raw_temp = raw_temp * 100 / 128; // Convert to c
      if (raw_temp > max_temp) max_temp = raw_temp;
      if (raw_temp == DEVICE_DISCONNECTED_RAW) {
        String message = "Failed to read one wire sensor: " + device_address_str;
        logger(message);
        return DEVICE_DISCONNECTED_RAW;
      }
      String json_string = "{\"" + device_address_str + "\":"+String(raw_temp)+"}";
      logger(json_string);
      // Send MQTT message
      mqttClient.beginMessage(topic);
      mqttClient.print(json_string);
      mqttClient.endMessage();
      // Save temperature for web site
      store_temperature(device_address_str, raw_temp);
    }
  }
  return max_temp;
}

void handleLog() {
  if (f) {
    f.seek(0, SeekSet);
    size_t sent = server.streamFile(f, "text/plain");
  }
}

void handleRoot() {
  server.send_P(200, "text/html", html_blob);
}

void handleSetMaxTemp(String new_max_temp){
  String message = String(new_max_temp);
  message = "Setting new MAX_HW_TEMP: " + message;
  logger(message);
  MAX_HW_TEMP = new_max_temp.toInt();
}

void controllerStatus(String actor) {
  // GET current status and return a JSON object
  String message;
  String hwc;
  bool status;
  unsigned int offTime = 0;

  if (actor == "psu") {
    status = digitalRead(PWR_RELAY);
  }
  else if (actor == "hw") {
    status = digitalRead(HW_RELAY);
    if (status) offTime = hw_off_epoch;
  }
  else if (actor == "ch") {
    status = digitalRead(CH_RELAY);
    if (status) offTime = ch_off_epoch;
  }
  else if (actor == "hwc") {
    String top = String(temperature_top);
    String mid = String(temperature_mid);
    String btm = String(temperature_btm);
    hwc = ", \"top\":" + top + ", \"mid\":" + mid + ", \"btm\":" + btm;
  }
  else if (actor == "roomstat") {
    status = digitalRead(THERMO_RELAY);
  }
  else {
    status = false;
  }
  String status_string = (status) ? "true" : "false";
  message = "{\"state\":" + status_string;
  if (hwc != "") message += hwc;
  if (offTime > 0) {
    message += ", \"offtime\":";
    message += String(offTime);
  }
  message += "}";
  server.send(200, "application/json", message);
}

void controllerAction(String actor, String action, String dur = String(MAX_RUN_TIME)){
  unsigned int duration = dur.toInt();
  // We expect duration in minutes from the old system.  Need to convert to seconds
  if (duration != MAX_RUN_TIME) duration = duration * 60;
  if (duration > MAX_RUN_TIME) duration = MAX_RUN_TIME;
  unsigned long off_epoch = current_epoch + duration;

  String offtime;
  String message;
  bool s = (action == "on") ? 1 : 0;
  String state = (s) ? "true" : "false";

  if (actor=="psu"){
    digitalWrite(PWR_RELAY, s);
    digitalWrite(PWR_LED, !s);
    ENABLED = s;
    if (!ENABLED) {
      ch_off_epoch = 0;
      hw_off_epoch = 0;
      digitalWrite(HW_RELAY, LOW);
      digitalWrite(CH_RELAY, LOW);
      digitalWrite(HW_LED, HIGH);
      digitalWrite(CH_LED, HIGH);
    }
    logger("Changing PSU state: " + state);
  }

  else if ((actor == "hw") && (ENABLED)){
    digitalWrite(HW_RELAY, s);
    digitalWrite(HW_LED, !s);
    if (s) {
      hw_off_epoch = off_epoch;
      offtime = ",\"offtime\":" + String(hw_off_epoch);
    }
    else {
      hw_off_epoch = 0;
    }
    logger("Changing HW state: " + action);
  }

  else if ((actor == "ch") && (ENABLED)) {
    digitalWrite(CH_RELAY, s);
    digitalWrite(CH_LED, !s);
    if (s) {
      ch_off_epoch = off_epoch;
      offtime = ",\"offtime\":" + String(ch_off_epoch);
    }
    else ch_off_epoch = 0;
    logger("Changing CH state: " + action);
  }

  else if (actor == "roomstat") {
    digitalWrite(THERMO_RELAY, s);
    logger("Changed room state: "+ action);
  }

  else {
    if (!ENABLED) server.send(404, "application/json", "{\"state\":false,\"message\":\"Controller not enabled\",\"result\":true}");
    else server.send(404, "application/json", "{\"status\":false,\"message\":\"Failed to parse POSTed options\",\"result\":false}");
    return;
  }
  message = "{\"result\":true, \"state\":" + state + offtime + "}";
  server.send(200, "application/json", message);
}

void handleNotFound() {
  String message = "File Not Found\n";
  server.send(404, "text/plain", message);
}

void setup() {
  //Serial.begin(115200);
  // Open the log file early
  if (SPIFFS.begin()) {
    f = SPIFFS.open("/log.txt", "a+");
  } else {
    pinMode(CH_SW, FUNCTION_0);
    pinMode(HW_SW, FUNCTION_0);
    Serial.begin(115200);
    Serial.println("Failed to open SPIFFS for log file.  Can't continue.");
    delay(5000);
    ESP.restart();
  }
  logger("\n\n\n\n----------\nBooting");
  
  WiFi.mode(WIFI_STA);
  WiFi.hostname("new_heat");
  WiFi.begin(SSID, WIFIPASS);
  while (WiFi.waitForConnectResult() != WL_CONNECTED) {
    logger("Couldn't connect to Wifi! Rebooting...");
    delay(5000);
    ESP.restart();
  }

  // Set up ins and outs
  pinMode(PWR_RELAY, OUTPUT);
  pinMode(CH_RELAY, OUTPUT);
  pinMode(HW_RELAY, OUTPUT);
  pinMode(THERMO_RELAY, OUTPUT);
  digitalWrite(PWR_RELAY, LOW);
  digitalWrite(CH_RELAY, LOW);
  digitalWrite(HW_RELAY, LOW);
  digitalWrite(THERMO_RELAY, LOW);
  pinMode(PWR_LED, OUTPUT);
  pinMode(CH_LED, OUTPUT);
  pinMode(HW_LED, OUTPUT);
  pinMode(PWR_SW, INPUT_PULLUP);
  pinMode(CH_SW, FUNCTION_3);
  pinMode(HW_SW, FUNCTION_3);
  pinMode(CH_SW, INPUT_PULLUP);
  pinMode(HW_SW, INPUT_PULLUP);

  
  // Set up OTA updates
  ArduinoOTA.setHostname("new_heat_ota");
  ArduinoOTA.onStart([]() {
    String type;
    if (ArduinoOTA.getCommand() == U_FLASH) {
      type = "sketch";
    } else { // U_FS
      type = "filesystem";
    }
    f.close();
    SPIFFS.gc();
    SPIFFS.end();
  });
  ArduinoOTA.begin();

  // NTP
  timeClient.begin();
  timeClient.update();
  logger("Connected to NTP server");

  // MQTT
  logger("Connecting to the MQTT broker");
  if (!mqttClient.connect(broker, port)) logger("MQTT connection failed!");

  // OneWire
  sensors.begin();
  temperature_device_count = sensors.getDeviceCount();
  String message = "Found 1wire temperature devices: ";
  message += String(temperature_device_count);
  logger(message);
  DeviceAddress tempDeviceAddress;
  for(int i=0;i<temperature_device_count; i++)  {
    if(sensors.getAddress(tempDeviceAddress, i)) {
      sensors.setResolution(tempDeviceAddress, TEMPERATURE_PRECISION);
    }
  }
    
  // Set up HTTP server responders
  server.on("/", HTTP_GET, handleRoot);
  server.on("/log", HTTP_GET, handleLog);
  server.on("/get/max_hw_temp", HTTP_GET, []() {
    String message = "{\"max_hw_temp\":";
    message += String(MAX_HW_TEMP);
    message += "}";
    logger(message);
    server.send(200, "application/json", message);
  });
  server.on(UriBraces("/get/{}"), HTTP_GET, []() {
    String actor = server.pathArg(0);
    controllerStatus(actor);
  });
  server.on(UriBraces("/set/{}/{}"), HTTP_POST, []() {
    String actor = server.pathArg(0);
    String action = server.pathArg(1);
    controllerAction(actor, action);
  });
  server.on(UriBraces("/set/{}/{}/{}"), HTTP_POST, []() {
    String actor    = server.pathArg(0);
    String action   = server.pathArg(1);
    String duration = server.pathArg(2);
    controllerAction(actor, action, duration);
  });
  server.on(UriBraces("/set/max_hw_temp/{}"), HTTP_POST, []() {
    String new_max_temp = server.pathArg(0);
    handleSetMaxTemp(new_max_temp);
  });
  server.onNotFound(handleNotFound);
  logger("Starting web server");
  server.begin();
  
  if (MDNS.begin("new-heat")) {
    logger("MDNS responder started");
  }

  logger("Setup complete. Ready.");  
}

void loop() {
  ArduinoOTA.handle();
  server.handleClient();
  mqttClient.poll();
  
  // Heat and hot water logic:
  // # Function  |  HW_OFF |  HW_ON  |  CH_ON
  // # All off   |   0     |    0    |    0
  // # HW Only   |   0     |    1    |    0
  // # CH Only   |   1     |    0    |    1
  // # All on    |   0     |    1    |    1


  if ((current_epoch > ch_off_epoch) && (ch_off_epoch > 0)){
      logger("CH time up.  Turning off");
      digitalWrite(CH_RELAY, LOW);
      ch_off_epoch = 0;
    }

  if ((current_epoch > hw_off_epoch) && (hw_off_epoch > 0)) {
      logger("HW time up.  Turning off");
      digitalWrite(HW_RELAY, LOW);
      hw_off_epoch = 0;
  }

  // Status LEDs are set by looking at the relays.
  // Logic is inverted because LEDs are pull down on
  digitalWrite(CH_LED, !digitalRead(CH_RELAY));
  digitalWrite(HW_LED, !digitalRead(HW_RELAY));
  

  // Switch stuff
  ///  PWR Switch
  if (digitalRead(PWR_SW) == LOW) {
    logger("PWR Button pressed");
    unsigned long st = millis();
    while (!digitalRead(PWR_SW) && (millis()-st < 2000)) {
      delay(10);
    };
    logger("PWR Button released");
    if (millis() - st > 2000) {
      String s = (!ENABLED) ? "on" : "off";
      controllerAction("psu", s);
      }
      delay(1000);  // Give the user time to let go of the button when they here the click
  };

  /// HW Switch
  if (digitalRead(HW_SW) == LOW) {
    logger("HW Button pressed");
    unsigned int st = millis();
    while (!digitalRead(HW_SW) && (millis()-st < 1000)) {
      delay(10);
    };
    logger("HW button released or timer exceeded");
    if ((millis() - st >= 1000) && (ENABLED)) {
      String s = (!digitalRead(HW_RELAY)) ? "on" : "off";
      controllerAction("hw", s);
      delay(1000);
    }
  };

  /// CH Switch
  if (digitalRead(CH_SW) == LOW) {
    logger("CH Button pressed");
    unsigned int st = millis();
    while (!digitalRead(CH_SW) && (millis()-st < 1000)) {
      delay(10);
    };
    logger("CH button released or timer exceeded");
    if ((millis() - st > 1000) && (ENABLED)) {
      String s = (!digitalRead(CH_RELAY)) ? "on" : "off";
      controllerAction("ch", s);
      delay(1000);
    }
  };

  // 1 second pulse
  if (millis() - millis_1s > 1000) {
    millis_1s = millis();
    current_epoch = timeClient.getEpochTime();
    if (!ENABLED) {
      // We are off, so flash the PWR light to show standby mode.
      digitalWrite(PWR_LED, !digitalRead(PWR_LED));
    }
  }

  // 10 minute pulse
  if (millis() - millis_10m > 600000) {
    timeClient.update();
    millis_10m = millis();
    int16_t max_temp = onewire_reading();
    if (max_temp > MAX_HW_TEMP) {
      logger("HW is up to temperature.  Turning off.");
      digitalWrite(HW_RELAY, LOW);
      hw_off_epoch = 0;
    }
  }
} 

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
#include "LittleFS.h"
#include "html_blob.h"
#include "wificreds.h"

#define TEMPERATURE_PRECISION 10 // 9 = 0.5C, 10 = 0.125C, 11 = 0.0625C.  Reading is slower higher precision.

// Define your wifi creds in wificreds.h
#ifndef HOSTNAME
#define HOSTNAME "piwarmer"
#endif

// Note for myself:  The "spare" wire was used to stop power back feeding from HW OFF to
// the old controller.  I can't be arsed hooking it up, so I left it disconnected.  You'll
// have to use the 4th relay if you want to make some kind of auto bypass.

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
const byte  ONEWIRE_PIN  = 5;
const byte  CH_SW        = 0; //1; // TX_GPIO
const byte  HW_SW        = 3; // RX_GPIO
const unsigned int MAX_RUN_TIME = 5400;

// Globals
bool ENABLED = true;
float MAX_HW_TEMP = 55.0;

// Server
ESP8266WebServer server(80);

// Time stuff
/// TODO:  I was going to use wall time for scheduling, but in the end
/// I decided to move all of that logic out to something else.  I could
/// either build in some scheduling system so that if the outside controller
/// is down we can still turn on and off at certain times, or...
/// more likely....
/// just remove all of this NTP stuff and fall back to millis where I'm using epoch.
/// Other systems in my house which use this do deal with epoch though, so meh.
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "time"); //, utcOffsetInSeconds);
unsigned long current_epoch;
unsigned long hw_off_epoch = 0;
unsigned long ch_off_epoch = 0;
unsigned long deenergise_epoch = 0;
unsigned long millis_1s = 0;
unsigned long millis_5m = 0;
unsigned long millis_10m = 0;
bool deenergise_motor = false;

// MQTT Stuff
WiFiClient wifiClient;
MqttClient mqttClient(wifiClient);
const char broker[] = "smarthome";
const int  port     = 1883;


// One Wire Stuff
OneWire ds(ONEWIRE_PIN);
DallasTemperature sensors(&ds);
byte temperature_device_count = 0;

// Device mappings
String device_tank_top = "287c220e070000ef";
String device_tank_mid = "28f72d0e07000092";
String device_tank_btm = "28f33e0e0700007c";
int16_t temperature_top = 0;
int16_t temperature_mid = 0;
int16_t temperature_btm = 0;

// LittleFS stuff
File f;

void logger(String message){
  unsigned int fsize = f.size();
  if (fsize > 5000) {
    f.close();
    LittleFS.remove("/log.txt");
    f = LittleFS.open("/log.txt", "a+");
    f.println("Log cleared");
  }

  f.print(timeClient.getFormattedTime());
  f.print(" : ");
  f.print(message);
  f.println();
}

void sendMqtt(String topic, String message){
  if (!mqttClient.connected()) {
    int c = mqttClient.connect(broker, port);
    if (!c) {
      logger("MQTT connect failed trying to send message");
      return;
    }
  }
  mqttClient.beginMessage(topic);
  mqttClient.print(message);
  mqttClient.endMessage();
};

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
      float degreesC = sensors.getTempC(tempDeviceAddress);
      if (degreesC > max_temp) max_temp = degreesC;
      if (degreesC == DEVICE_DISCONNECTED_C) {
        String message = "Failed to read one wire sensor: " + device_address_str;
        logger(message);
        return DEVICE_DISCONNECTED_C;
      }
      //float water_temp_c = raw_temp / 1000.0;
      String json_string = "{\"temperature\":"+String(degreesC)+"}";
      //logger(json_string);

      // Send MQTT message
      String device_name;
      if (device_address_str == device_tank_top) device_name = "top";
      if (device_address_str == device_tank_mid) device_name = "mid";
      if (device_address_str == device_tank_btm) device_name = "btm";
      String topic = "sensors/hwc/"+device_name;
      sendMqtt(topic, json_string);
      // Save temperature for web site
      store_temperature(device_address_str, degreesC);
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
  MAX_HW_TEMP = new_max_temp.toFloat();
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
  String topic;
  String json_string;

  bool s = (action == "on") ? 1 : 0;
  String state = (s) ? "true" : "false";

  if (actor=="psu"){
    topic = "sensors/chpsu/state";
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
    topic = "sensors/hw/state";
    //logger("In HW section");
    digitalWrite(HW_RELAY, s);
    digitalWrite(HW_LED, !s);
    if (s) {
      hw_off_epoch = off_epoch;
      offtime = ",\"offtime\":" + String(hw_off_epoch);
    }
    else hw_off_epoch = 0;
    logger("Changing HW state: " + action);
  }

  else if ((actor == "ch") && (ENABLED)) {
    topic = "sensors/ch/state";
    //logger("In CH section");
    digitalWrite(CH_RELAY, s);
    digitalWrite(CH_LED, !s);
    if (s) {
      ch_off_epoch = off_epoch;
      offtime = ",\"offtime\":" + String(ch_off_epoch);
    }
    else {
      ch_off_epoch = 0;
      if (digitalRead(HW_RELAY) == LOW){
        // CH has turned off, and HW is off so we need to de-energise
        logger("De-energising via CH off through API");
        deenergise_epoch = current_epoch + 30;
      }
    }
    logger("Changing CH state: " + action);
  }

  else if (actor == "roomstat") {
    topic = "sensors/chroomstat/state";
    digitalWrite(THERMO_RELAY, s);
    logger("Changed room state: "+ action);
  }

  else {
    if (!ENABLED) server.send(404, "application/json", F("{\"state\":false,\"message\":\"Controller not enabled\",\"result\":true}"));
    else server.send(404, "application/json", F("{\"status\":false,\"message\":\"Failed to parse POSTed options\",\"result\":false}"));
    return;
  }
  //logger("Preparing to send reply to HTTP");
  message = "{\"result\":true, \"state\":" + state + offtime + "}";
  server.send(200, "application/json", message);
  //logger("Sent HTTP 200 back");
  json_string = "{\"state\":" + state + "}";
  sendMqtt(topic, json_string);
}

void handleNotFound() {
  String message = F("File Not Found\n");
  server.send(404, "text/plain", message);
}

void setup() {
  //Serial.begin(115200); // XX
  // Open the log file early
  if (LittleFS.begin()) {
    f = LittleFS.open("/log.txt", "a+");
  } else {
    pinMode(CH_SW, FUNCTION_0); // XX
    pinMode(HW_SW, FUNCTION_0);  // XX
    Serial.begin(115200);//xx
    Serial.println("Failed to open SPIFFS for log file.  Can't continue.");
    delay(5000); //xx
    //ESP.restart(); //xx
  }
  logger("\n\n\n\n----------\nBooting");
  
  WiFi.mode(WIFI_STA);
  WiFi.hostname(HOSTNAME);
  WiFi.setAutoConnect(true);
  WiFi.setSleep(WIFI_PS_NONE);
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
  digitalWrite(PWR_RELAY, HIGH); // enable at power on
  digitalWrite(CH_RELAY, LOW);
  digitalWrite(HW_RELAY, LOW);
  digitalWrite(THERMO_RELAY, LOW);
  pinMode(PWR_LED, OUTPUT);
  pinMode(CH_LED, OUTPUT);
  pinMode(HW_LED, OUTPUT);
  pinMode(CH_SW, FUNCTION_3);//xx
  pinMode(HW_SW, FUNCTION_3);//xx
  pinMode(CH_SW, INPUT_PULLUP);//xx
  pinMode(HW_SW, INPUT_PULLUP);//xx

  
  // Set up OTA updates
  ArduinoOTA.setHostname(HOSTNAME);
  ArduinoOTA.onStart([]() {
    String type;
    if (ArduinoOTA.getCommand() == U_FLASH) {
      type = "sketch";
    } else { // U_FS
      type = "filesystem";
    }
    f.close();
    // SPIFFS.gc();
    LittleFS.end();
  });
  ArduinoOTA.begin();

  // NTP
  timeClient.begin();
  timeClient.update();
  logger(F("Connected to NTP server"));

  // MQTT
  logger(F("Connecting to the MQTT broker"));
  if (!mqttClient.connect(broker, port)) logger("MQTT connection failed!");

  // OneWire
  sensors.begin();
  temperature_device_count = sensors.getDeviceCount();
  String message = F("Found 1wire temperature devices: ");
  message += String(temperature_device_count);
  logger(message);
  DeviceAddress tempDeviceAddress;
  for(int i=0;i<temperature_device_count; i++)  {
    if(sensors.getAddress(tempDeviceAddress, i)) {
      String message = F("Found 1wire temperature device: ");
      String device_address_str = getDeviceAddressString(tempDeviceAddress);
      message += device_address_str;
      logger(message);
      sensors.setResolution(tempDeviceAddress, TEMPERATURE_PRECISION);
    }
  }
  onewire_reading();
    
  // Set up HTTP server responders
  server.on("/", HTTP_GET, handleRoot);
  server.on("/log", HTTP_GET, handleLog);
  server.on("/get/max_hw_temp", HTTP_GET, []() {
    String message = F("{\"max_hw_temp\":");
    message += String(MAX_HW_TEMP);
    message += "}";
    //logger(message);
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

  server.on("/reboot", HTTP_POST, []() {
    server.send(200, "application/json", F("{\"result\":true,\"message\":\"Rebooting...\"}"));
    ESP.restart();
  });
  server.onNotFound(handleNotFound);
  logger(F("Starting web server"));
  server.begin();
  
  if (MDNS.begin(HOSTNAME)) {
    logger(F("MDNS responder started"));
  }
  logger(F("Hostname..."));
  logger(HOSTNAME);
  logger(F("Setup complete. Ready."));
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
      logger(F("CH time up.  Turning off"));
      //digitalWrite(CH_RELAY, LOW);
      controllerAction("ch", "off", "1");
      ch_off_epoch = 0;
      if (digitalRead(HW_RELAY) == LOW) {
        logger(F("Setting de-energise epoch"));
        deenergise_epoch = current_epoch + 30;
      }
    }

  if ((current_epoch > hw_off_epoch) && (hw_off_epoch > 0)) {
      logger(F("HW time up.  Turning off"));
      // digitalWrite(HW_RELAY, LOW);
      controllerAction("hw", "off", "1");
      hw_off_epoch = 0;
  }

  if ((current_epoch > deenergise_epoch) && (deenergise_epoch > 0)) {
      logger(F("De-energising motor"));
      controllerAction("hw", "on", "1");
      deenergise_epoch = 0;
      //deenergise_motor = false;
  }
  // Status LEDs are set by looking at the relays.
  // Logic is inverted because LEDs are pull down on
  digitalWrite(CH_LED, !digitalRead(CH_RELAY));
  digitalWrite(HW_LED, !digitalRead(HW_RELAY));
  

  // Switch stuff // xxV
  // /// HW Switch
  // if (digitalRead(HW_SW) == LOW) {
  //   logger(F("HW Button pressed"));
  //   unsigned int st = millis();
  //   while (!digitalRead(HW_SW) && (millis()-st < 5000)) {
  //     delay(10);
  //   };
  //   logger(F("HW button released or timer exceeded"));
  //   if ((millis() - st >= 500) && (ENABLED)) {
  //     String s = (!digitalRead(HW_RELAY)) ? "on" : "off";
  //     controllerAction("hw", s);
  //     delay(1000);
  //   }
  // };

  // /// CH Switch
  // /// CH switch is on GPIO0, so hold it at power on to enable serial flashing
  // if (digitalRead(CH_SW) == LOW) {
  //   logger(F("CH Button pressed"));
  //   unsigned int st = millis();
  //   while (!digitalRead(CH_SW) && (millis()-st < 2000)) {
  //     delay(10);
  //   };
  //   logger(F("CH button released or timer exceeded"));
  //   if (millis() - st > 2000) {
  //     String s = (!ENABLED) ? "on" : "off";
  //     controllerAction("psu", s);
  //     delay(1000);
  //   };
  //   if ((millis() - st < 1500) && (ENABLED)) {
  //     String s = (!digitalRead(CH_RELAY)) ? "on" : "off";
  //     controllerAction("ch", s);
  //     delay(1000);
  //   }
  // }; // xx ^

  // 1 second pulse
  if (millis() - millis_1s > 1000) {
    millis_1s = millis();
    current_epoch = timeClient.getEpochTime();
    if (!ENABLED) {
      // We are off, so flash the PWR light to show standby mode.
      digitalWrite(PWR_LED, !digitalRead(PWR_LED));
    }
  }

  // 5 minute pulse
  // Hack this to be every one minute
  //if (millis() - millis_5m > 300000) {
  if (millis() - millis_5m >   60000) {
    millis_5m = millis();
    float max_temp = onewire_reading();
    if ((max_temp > MAX_HW_TEMP) && (digitalRead(HW_RELAY))) {
      logger(F("HW is up to temperature.  Turning off."));
      digitalWrite(HW_RELAY, LOW);
      hw_off_epoch = 0;
    }
  }

  // 10 minute pulse
  if (millis() - millis_10m > 600000) {
    timeClient.update();
    millis_10m = millis();
  }
} 

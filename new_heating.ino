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
#define MAX_HW_TEMP 5600

// Define your wifi creds in wificreds.h


const char* ssid = STASSID;
const char* password = STAPSK;

const byte PWR_RELAY = 16;
const byte CH_RELAY = 14;
const byte HW_RELAY = 12;
const byte THERMO_RELAY = 13;
const byte PWR_LED = 15;
const byte CH_LED = 2;
const byte HW_LED = 4;
const byte PWR_SW = 0;
bool enabled = false;
bool ch_on = false;
bool hw_on = false;
const unsigned int max_run_time = 5800;


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
  if (fsize > 5000) {
    f.close();
    SPIFFS.gc();
    SPIFFS.remove("/log.txt");
    f = SPIFFS.open("/log.txt", "a+");
  }
  f.seek(0,SeekEnd);
  f.print(timeClient.getFormattedTime());
  f.print(" : ");
  f.println(message);
  Serial.println(message);
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
      //String device_address_str = "";
      // for (uint8_t i = 0; i < 8; i++) {
      //   if (tempDeviceAddress[i] < 16) device_address_str += "0";
      //   device_address_str += String(tempDeviceAddress[i], HEX);
      // }
      int16_t raw_temp = sensors.getTemp(tempDeviceAddress);
      raw_temp = raw_temp * 100 / 128; // Convert to c
      if (raw_temp > max_temp) max_temp = raw_temp;
      if (raw_temp == DEVICE_DISCONNECTED_RAW) {
        logger("Failed to read sensor");
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
  unsigned int s = f.size();
  Serial.print("Log bytes: ");
  Serial.println(s);
  if (f) {
    f.seek(0, SeekSet);
    size_t sent = server.streamFile(f, "text/plain");
  } else {
    Serial.println("Failed to send log");
  }
}

void handleNotFound() {
  String message = "File Not Found\n\n";
  message += "URI: ";
  message += server.uri();
  message += "\nMethod: ";
  message += (server.method() == HTTP_GET) ? "GET" : "POST";
  message += "\nArguments: ";
  message += server.args();
  message += "\n";
  for (uint8_t i = 0; i < server.args(); i++) {
    message += " " + server.argName(i) + ": " + server.arg(i) + "\n";
  }
  server.send(404, "text/plain", message);
}

void handleRoot() {
  server.send_P(200, "text/html", html_blob);
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


void controllerAction(String actor, String action, String dur = String(max_run_time)){
  unsigned int duration = dur.toInt();
  // We expect duration in minutes from the old system.  Need to convert to seconds
  if (duration != max_run_time) duration = duration * 60;
  if (duration > max_run_time) duration = max_run_time;
  String offtime = "";
  String state   = "";

  if ((actor == "psu") && (action == "on") && (enabled == false)) {
    logger("Enabling PSU and Controller");
    digitalWrite(PWR_RELAY, HIGH);
    digitalWrite(PWR_LED, LOW);
    enabled = true;
    state = "true";
  }
  else if ((actor == "psu") && (action == "off")) {
    logger("Disabling PSU and Controller");
    digitalWrite(PWR_RELAY, LOW);
    digitalWrite(HW_RELAY, LOW);
    digitalWrite(CH_RELAY, LOW);
    ch_off_epoch = 0;
    hw_off_epoch = 0;
    enabled = false;
    state = "false";
  }
  else if ((actor == "hw") && (action == "on") && (enabled == true)) {
    logger("Turn on hot water relay");
    hw_off_epoch = current_epoch + duration;
    digitalWrite(HW_RELAY, HIGH);
    offtime = ",\"offtime\":" + String(hw_off_epoch);
    state = "true";
  }
  else if ((actor == "hw") && (action == "off")) {
    digitalWrite(HW_RELAY, LOW);
    hw_off_epoch = 0;
    state = "false";
    logger("Switching HW Relay Off");
  }
  else if ((actor == "ch") && (action == "on") && (enabled == true)) {
    logger("Turn on heating relay");
    ch_off_epoch = current_epoch + duration;
    digitalWrite(CH_RELAY, HIGH);
    offtime = ",\"offtime\":" + String(ch_off_epoch);
    state = "true";
  }
  else if ((actor == "ch") && (action == "off")) {
    digitalWrite(CH_RELAY, LOW);
    ch_off_epoch = 0;
    state = "false";
    logger("Switching CH relay off");
  }
  else {
    if (!enabled) server.send(404, "application/json", "{\"state\":false,\"message\":\"Controller not enabled\",\"result\":true}");
    else server.send(404, "application/json", "{\"status\":false,\"message\":\"Failed to parse POSTed options\",\"result\":false}");
    return;
  }
  String message = "{\"result\":true, \"state\":" + state + offtime + "}";
  server.send(200, "application/json", message);
}

void setup() {
  Serial.begin(115200);
  // Open the log file early
  if (SPIFFS.begin()) {
    f = SPIFFS.open("/log.txt", "a+");
    if (!f) Serial.println("Failed to open log file");
  } else {
    Serial.begin(115200);
    Serial.println("Failed to open SPIFFS");
    ESP.restart();
  }
  logger("Booting");
  
  WiFi.mode(WIFI_STA);
  WiFi.hostname("new_heat");
  WiFi.begin(ssid, password);
  while (WiFi.waitForConnectResult() != WL_CONNECTED) {
    logger("Connection Failed! Rebooting...");
    delay(5000);
    ESP.restart();
  }

  // Set up ins and outs
  pinMode(PWR_RELAY, OUTPUT);
  digitalWrite(PWR_RELAY, LOW);
  pinMode(CH_RELAY, OUTPUT);
  pinMode(HW_RELAY, OUTPUT);
  pinMode(THERMO_RELAY, OUTPUT);
  pinMode(PWR_LED, OUTPUT);
  pinMode(CH_LED, OUTPUT);
  pinMode(HW_LED, OUTPUT);
  pinMode(PWR_SW, INPUT_PULLUP);
  digitalWrite(CH_RELAY, LOW);
  digitalWrite(HW_RELAY, LOW);
  digitalWrite(THERMO_RELAY, LOW);
  
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
    logger("Start updating " + type);
  });
  ArduinoOTA.onEnd([]() {
    logger("\nEnd");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    logger(String(progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    logger("OTA Error");
    if (error == OTA_AUTH_ERROR) {
      logger("Auth Failed");
    } else if (error == OTA_BEGIN_ERROR) {
      logger("Begin Failed");
    } else if (error == OTA_CONNECT_ERROR) {
      logger("Connect Failed");
    } else if (error == OTA_RECEIVE_ERROR) {
      logger("Receive Failed");
    } else if (error == OTA_END_ERROR) {
      logger("End Failed");
    }
  });
  ArduinoOTA.begin();

  // NTP
  timeClient.begin();
  timeClient.update();

  // MQTT
  logger("Attempting to connect to the MQTT broker: ");
  if (!mqttClient.connect(broker, port)) {
    logger("MQTT connection failed!");
  }

  // OneWire
  sensors.begin();
  temperature_device_count = sensors.getDeviceCount();
  DeviceAddress tempDeviceAddress;
  for(int i=0;i<temperature_device_count; i++)  {
    // Search the wire for address
    if(sensors.getAddress(tempDeviceAddress, i)) {
      sensors.setResolution(tempDeviceAddress, TEMPERATURE_PRECISION);
    }
  }
    
  server.on("/", HTTP_GET, handleRoot);
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
  server.on("/log", HTTP_GET, handleLog);
  server.onNotFound(handleNotFound);
  server.begin();
  
  if (MDNS.begin("new-heat")) {
    logger("MDNS responder started");
  }

  logger("Ready");  
}



void loop() {
  ArduinoOTA.handle();
  server.handleClient();
  mqttClient.poll();
  timeClient.update();
  
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
  if (digitalRead(PWR_SW) == LOW) {
    logger("Button pressed");
    unsigned long st = millis();
    while (!digitalRead(PWR_SW) && (millis()-st < 2000)) {
      //logger("Button held");
      delay(10);
    };
    logger("Button released");
    if (millis() - st > 2000) {
      enabled = !enabled;
      digitalWrite(PWR_LED, !enabled); // Logic is reversed for LEDs since they are pull down
      digitalWrite(PWR_RELAY, enabled); // Relays are normal logic
      delay(2000);  // Give the user time to let go of the button when they here the click
    }
  };

  // 1 second pulse
  if (millis() - millis_1s > 1000) {
    millis_1s = millis();
    current_epoch = timeClient.getEpochTime();
    if (!enabled) {
      // We are off, so flash the PWR light to show standby mode.
      digitalWrite(PWR_LED, !digitalRead(PWR_LED));
    }
  }

  // 10 minute pulse
  if (millis() - millis_10m > 600000) {
    logger("10m tick");
    millis_10m = millis();
    int16_t max_temp = onewire_reading();
    if (max_temp > MAX_HW_TEMP) {
      // Hot water tank should be full now, turn off HW
      digitalWrite(HW_RELAY, LOW);
      hw_off_epoch = 0;
    }
    String t = String(timeClient.getFormattedTime());
    logger(t);
  }
} 

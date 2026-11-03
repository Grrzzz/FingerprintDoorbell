/***************************************************
  Main of FingerprintDoorbell 
 ****************************************************/

#include <WiFi.h>
#include <DNSServer.h>
#include <time.h>
#include <ESPAsyncWebServer.h>
#include <AsyncElegantOTA.h>
#include <SPIFFS.h>
#include <PubSubClient.h>
#include "FingerprintManager.h"
#include "SettingsManager.h"
#include "HomeAssistantDiscovery.h"
#include <esp_timer.h>
#include "global.h"

enum class Mode { scan, enroll, wificonfig, maintenance };

const char* VersionInfo = "2.0 Grrzzz";

// ===================================================================================================================
// Caution: below are not the credentials for connecting to your home network, they are for the Access Point mode!!!
// ===================================================================================================================
const char* WifiConfigSsid = "FingerprintDoorbell-Config"; // SSID used for WiFi when in Access Point mode for configuration
const char* WifiConfigPassword = "12345678"; // password used for WiFi when in Access Point mode for configuration. Min. 8 chars needed!
IPAddress   WifiConfigIp(192, 168, 4, 1); // IP of access point in wifi config mode

const long  gmtOffset_sec = 0; // UTC Time
const int   daylightOffset_sec = 0; // UTC Time
const int   doorbellOutputPin = 19; // pin connected to the doorbell (when using hardware connection instead of mqtt to ring the bell)

#ifdef CUSTOM_GPIOS
  const int   customOutput1 = 18; // not used internally, but can be set over MQTT
  const int   customOutput2 = 26; // not used internally, but can be set over MQTT
  const int   customInput1 = 21; // not used internally, but changes are published over MQTT
  const int   customInput2 = 22; // not used internally, but changes are published over MQTT
  bool customInput1Value = false;
  bool customInput2Value = false;
#endif

const int logMessagesCount = 20;
String logMessages[logMessagesCount]; // log messages, 0=most recent log message
bool shouldReboot = false;
unsigned long wifiReconnectPreviousMillis = 0;
unsigned long mqttReconnectPreviousMillis = 0;
unsigned long mqttStatusPreviousMillis = 0;
const unsigned long mqttStatusIntervalMillis = 60000ul; // interval for publishing uptime, rssi, ...

String enrollId;
String enrollName;
String numIterations;
Mode currentMode = Mode::scan;

FingerprintManager fingerManager;
SettingsManager settingsManager;
bool needMaintenanceMode = false;

const byte DNS_PORT = 53;
DNSServer dnsServer;
AsyncWebServer webServer(80); // AsyncWebServer  on port 80
AsyncEventSource events("/events"); // event source (Server-Sent events)

WiFiClient espClient;
PubSubClient mqttClient(espClient);
long lastMsg = 0;
char msg[50];
int value = 0;
bool mqttConfigValid = true;


Match lastMatch;

// last doorbell event (shown on the web dashboard)
String lastEventType = ""; // "match" or "ring"
String lastEventName = "";
int lastEventId = 0;
int lastEventConfidence = 0;
String lastEventTime = "";

String jsonEscape(const String& in) {
  String out;
  for (unsigned int i = 0; i < in.length(); i++) {
    char c = in[i];
    if (c == '"' || c == '\\')
      out += '\\';
    if (c >= 32)
      out += c;
  }
  return out;
}

void sendJson(AsyncWebServerRequest *request, const String& json) {
  AsyncWebServerResponse *response = request->beginResponse(200, "application/json", json);
  response->addHeader("Cache-Control", "no-store");
  request->send(response);
}

// publishes the state of the switches (shared topic for state and command) retained
void publishControlState() {
  if (!mqttClient.connected())
    return;
  String rootTopic = settingsManager.getAppSettings().mqttRootTopic;
  mqttClient.publish((rootTopic + "/ignoreTouchRing").c_str(), fingerManager.getIgnoreTouchRing() ? "on" : "off", true);
  mqttClient.publish((rootTopic + "/ringEnabled").c_str(), settingsManager.getRingSettings().ringEnabled ? "on" : "off", true);
}

void setRingEnabled(bool enabled) {
  RingSettings settings = settingsManager.getRingSettings();
  settings.ringEnabled = enabled;
  settingsManager.saveRingSettings(settings);
  if (mqttClient.connected())
    publishControlState(); // the log message is written when the retained message arrives
  else
    notifyClients(String("ringEnabled is now '") + (enabled ? "on" : "off") + "'");
}

// changes an LED setting by its R503 value (1-7 colors, 1-4 sequence), persists and applies it immediately
void applyLedSettingValue(const String& setting, int value) {
  AppSettings settings = settingsManager.getAppSettings();
  if (setting == "touchRingColor" && value >= 1 && value <= 7)
    settings.touchRingActiveColor = value;
  else if (setting == "touchRingSequence" && value >= 1 && value <= 4)
    settings.touchRingActiveSequence = value;
  else if (setting == "scanColor" && value >= 1 && value <= 7)
    settings.scanColor = value;
  else if (setting == "matchColor" && value >= 1 && value <= 7)
    settings.matchColor = value;
  else
    return;

  settingsManager.saveAppSettings(settings);
  fingerManager.configLedRing(settings.touchRingActiveColor, settings.touchRingActiveSequence, settings.scanColor, settings.matchColor);
  if (currentMode == Mode::scan && fingerManager.connected)
    fingerManager.setLedRingReady();
  if (mqttClient.connected())
    publishLedSettings(mqttClient, settings.mqttRootTopic, settings.touchRingActiveColor, settings.touchRingActiveSequence, settings.scanColor, settings.matchColor);
}

String getStatusJson() {
  const AppSettings s = settingsManager.getAppSettings();
  const char* modeName = "scan";
  if (currentMode == Mode::enroll) modeName = "enroll";
  else if (currentMode == Mode::maintenance) modeName = "maintenance";
  else if (currentMode == Mode::wificonfig) modeName = "wificonfig";
  const char* mqttState = s.mqttServer.isEmpty() ? "unconfigured" : (mqttClient.connected() ? "connected" : "disconnected");

  String json = "{";
  json += "\"hostname\":\"" + jsonEscape(settingsManager.getWifiSettings().hostname) + "\"";
  json += ",\"version\":\"" + jsonEscape(VersionInfo) + "\"";
  json += ",\"uptime\":" + String((uint32_t)(esp_timer_get_time() / 1000000ULL));
  json += ",\"rssi\":" + String(WiFi.RSSI());
  json += ",\"ssid\":\"" + jsonEscape(WiFi.SSID()) + "\"";
  json += ",\"ip\":\"" + WiFi.localIP().toString() + "\"";
  json += ",\"heap\":" + String(ESP.getFreeHeap());
  json += String(",\"mode\":\"") + modeName + "\"";
  json += String(",\"sensorConnected\":") + (fingerManager.connected ? "true" : "false");
  json += String(",\"pairingValid\":") + (s.sensorPairingValid ? "true" : "false");
  json += String(",\"mqtt\":\"") + mqttState + "\"";
  json += ",\"mqttServer\":\"" + jsonEscape(s.mqttServer) + "\"";
  json += String(",\"haDiscovery\":") + (s.haDiscovery ? "true" : "false");
  json += String(",\"ignoreTouch\":") + (fingerManager.getIgnoreTouchRing() ? "true" : "false");
  json += String(",\"ringEnabled\":") + (settingsManager.getRingSettings().ringEnabled ? "true" : "false");
  json += ",\"fingerCount\":" + String(fingerManager.getFingerCount());
  json += ",\"lastEvent\":{\"type\":\"" + lastEventType + "\",\"name\":\"" + jsonEscape(lastEventName) + "\",\"id\":" + String(lastEventId)
          + ",\"confidence\":" + String(lastEventConfidence) + ",\"time\":\"" + jsonEscape(lastEventTime) + "\"}";
  // log messages, oldest first
  json += ",\"log\":[";
  bool firstLog = true;
  for (int i = logMessagesCount - 1; i >= 0; i--) {
    if (logMessages[i] != "") {
      if (!firstLog)
        json += ",";
      json += "\"" + jsonEscape(logMessages[i]) + "\"";
      firstLog = false;
    }
  }
  json += "]";
  return json + "}";
}

String getSettingsJson() {
  const AppSettings s = settingsManager.getAppSettings();
  const RingSettings r = settingsManager.getRingSettings();
  String json = "{";
  json += "\"mqttServer\":\"" + jsonEscape(s.mqttServer) + "\"";
  json += ",\"mqttUsername\":\"" + jsonEscape(s.mqttUsername) + "\"";
  json += ",\"mqttPassword\":\"" + jsonEscape(s.mqttPassword) + "\"";
  json += ",\"mqttRootTopic\":\"" + jsonEscape(s.mqttRootTopic) + "\"";
  json += ",\"ntpServer\":\"" + jsonEscape(s.ntpServer) + "\"";
  json += String(",\"haDiscovery\":") + (s.haDiscovery ? "true" : "false");
  json += ",\"haDiscoveryPrefix\":\"" + jsonEscape(s.haDiscoveryPrefix) + "\"";
  json += ",\"touchRingActiveColor\":" + String(s.touchRingActiveColor);
  json += ",\"touchRingActiveSequence\":" + String(s.touchRingActiveSequence);
  json += ",\"scanColor\":" + String(s.scanColor);
  json += ",\"matchColor\":" + String(s.matchColor);
  json += String(",\"ringEnabled\":") + (r.ringEnabled ? "true" : "false");
  json += ",\"ringDuration\":" + String(r.ringDuration);
  return json + "}";
}

void addLogMessage(const String& message) {
  // shift all messages in array by 1, oldest message will die
  for (int i=logMessagesCount-1; i>0; i--)
    logMessages[i]=logMessages[i-1];
  logMessages[0]=message;
}

String getLogMessagesAsHtml() {
  String html = "";
  for (int i=logMessagesCount-1; i>=0; i--) {
    if (logMessages[i]!="")
      html = html + logMessages[i] + "<br>";
  }
  return html;
}

String getTimestampString(){
  struct tm timeinfo;
  if(!getLocalTime(&timeinfo)){
    Serial.println("Failed to obtain time");
    return "no time";
  }
  
  char buffer[25];
  strftime(buffer,sizeof(buffer),"%Y-%m-%d %H:%M:%S %Z", &timeinfo);
  String datetime = String(buffer);
  return datetime;
}

/* wait for maintenance mode or timeout 5s */
bool waitForMaintenanceMode() {
  needMaintenanceMode = true;
  unsigned long startMillis = millis();
  while (currentMode != Mode::maintenance) {
    if ((millis() - startMillis) >= 5000ul) {
      needMaintenanceMode = false;
      return false;
    }
    delay(50);
  }
  needMaintenanceMode = false;
  return true;
}

// Replaces placeholder in HTML pages
// returns attribute ("selected"/"checked") if the numeric suffix of the template variable equals the current value
String selectedIf(int currentValue, const String& var, const char* prefix, const char* attribute) {
  return (var.substring(strlen(prefix)).toInt() == currentValue) ? attribute : "";
}

String processor(const String& var){
  if(var == "LOGMESSAGES"){
    return getLogMessagesAsHtml();
  } else if (var == "FINGERLIST") {
    return fingerManager.getFingerListAsHtmlOptionList();
  } else if (var == "HOSTNAME") {
    return settingsManager.getWifiSettings().hostname;
  } else if (var == "VERSIONINFO") {
    return VersionInfo;
  } else if (var == "WIFI_SSID") {
    return settingsManager.getWifiSettings().ssid;
  } else if (var == "WIFI_PASSWORD") {
    if (settingsManager.getWifiSettings().password.isEmpty())
      return "";
    else
      return "********"; // for security reasons the wifi password will not left the device once configured
  } else if (var == "MQTT_SERVER") {
    return settingsManager.getAppSettings().mqttServer;
  } else if (var == "MQTT_USERNAME") {
    return settingsManager.getAppSettings().mqttUsername;
  } else if (var == "MQTT_PASSWORD") {
    return settingsManager.getAppSettings().mqttPassword;
  } else if (var == "MQTT_ROOTTOPIC") {
    return settingsManager.getAppSettings().mqttRootTopic;
  } else if (var == "NTP_SERVER") {
    return settingsManager.getAppSettings().ntpServer;
  } else if (var == "HA_DISCOVERY") {
    return settingsManager.getAppSettings().haDiscovery ? "checked" : "";
  } else if (var == "HA_DISCOVERY_PREFIX") {
    return settingsManager.getAppSettings().haDiscoveryPrefix;
  } else if (var.startsWith("TOUCH_RING_ACTIVE_COLOR_")) {
    return selectedIf(settingsManager.getAppSettings().touchRingActiveColor, var, "TOUCH_RING_ACTIVE_COLOR_", "selected");
  } else if (var.startsWith("TOUCH_RING_ACTIVE_SEQUENCE_")) {
    return selectedIf(settingsManager.getAppSettings().touchRingActiveSequence, var, "TOUCH_RING_ACTIVE_SEQUENCE_", "checked");
  } else if (var.startsWith("SCAN_COLOR_")) {
    return selectedIf(settingsManager.getAppSettings().scanColor, var, "SCAN_COLOR_", "selected");
  } else if (var.startsWith("MATCH_COLOR_")) {
    return selectedIf(settingsManager.getAppSettings().matchColor, var, "MATCH_COLOR_", "selected");
} else if (var == "RING_ENABLED") {    
    return settingsManager.getRingSettings().ringEnabled ? "checked" : "";
} else if (var == "RING_DURATION") {
    return (String)settingsManager.getRingSettings().ringDuration;
}    
  return String();
}


// send LastMessage to websocket clients
void notifyClients(String message) {
  String messageWithTimestamp = "[" + getTimestampString() + "]: " + message;
  Serial.println(messageWithTimestamp);
  addLogMessage(messageWithTimestamp);
  events.send(getLogMessagesAsHtml().c_str(),"message",millis(),1000);
  
  String mqttRootTopic = settingsManager.getAppSettings().mqttRootTopic;
  mqttClient.publish((String(mqttRootTopic) + "/lastLogMessage").c_str(), message.c_str());
}

void updateClientsFingerlist(String fingerlist) {
  Serial.println("New fingerlist was sent to clients");
  events.send(fingerlist.c_str(),"fingerlist",millis(),1000);
}


bool doPairing() {
  String newPairingCode = settingsManager.generateNewPairingCode();

  if (fingerManager.setPairingCode(newPairingCode)) {
    AppSettings settings = settingsManager.getAppSettings();
    settings.sensorPairingCode = newPairingCode;
    settings.sensorPairingValid = true;
    settingsManager.saveAppSettings(settings);
    notifyClients("Pairing successful.");
    return true;
  } else {
    notifyClients("Pairing failed.");
    return false;
  }

}


bool checkPairingValid() {
  AppSettings settings = settingsManager.getAppSettings();

   if (!settings.sensorPairingValid) {
     if (settings.sensorPairingCode.isEmpty()) {
       // first boot, do pairing automatically so the user does not have to do this manually
       return doPairing();
     } else {
      Serial.println("Pairing has been invalidated previously.");   
      return false;
     }
   }

  String actualSensorPairingCode = fingerManager.getPairingCode();
  //Serial.println("Awaited pairing code: " + settings.sensorPairingCode);
  //Serial.println("Actual pairing code: " + actualSensorPairingCode);

  if (actualSensorPairingCode.equals(settings.sensorPairingCode))
    return true;
  else {
    if (!actualSensorPairingCode.isEmpty()) { 
      // An empty code means there was a communication problem. So we don't have a valid code, but maybe next read will succeed and we get one again.
      // But here we just got an non-empty pairing code that was different to the awaited one. So don't expect that will change in future until repairing was done.
      // -> invalidate pairing for security reasons
      AppSettings settings = settingsManager.getAppSettings();
      settings.sensorPairingValid = false;
      settingsManager.saveAppSettings(settings);
    }
    return false;
  }
}


bool initWifi() {
  // Connect to Wi-Fi
  WifiSettings wifiSettings = settingsManager.getWifiSettings();
  WiFi.mode(WIFI_STA);
  WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE, INADDR_NONE);
  WiFi.setHostname(wifiSettings.hostname.c_str()); //define hostname
  WiFi.begin(wifiSettings.ssid.c_str(), wifiSettings.password.c_str());
  int counter = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(1000);
    Serial.println("Waiting for WiFi connection...");
    counter++;
    if (counter > 30)
      return false;
  }
  Serial.println("Connected!");

  // Print ESP32 Local IP Address
  Serial.println(WiFi.localIP());

  return true;
}

void initWiFiAccessPointForConfiguration() {
  WiFi.softAPConfig(WifiConfigIp, WifiConfigIp, IPAddress(255, 255, 255, 0));
  WiFi.softAP(WifiConfigSsid, WifiConfigPassword);

  // if DNSServer is started with "*" for domain name, it will reply with
  // provided IP to all DNS request
  dnsServer.start(DNS_PORT, "*", WifiConfigIp);

  Serial.print("AP IP address: ");
  Serial.println(WifiConfigIp); 
}


void startWebserver(){
  
  // Initialize SPIFFS
  if(!SPIFFS.begin(true)){
    Serial.println("An Error has occurred while mounting SPIFFS");
    return;
  }

  // Init time by NTP Client
  configTime(gmtOffset_sec, daylightOffset_sec, settingsManager.getAppSettings().ntpServer.c_str());
  
  // webserver for normal operating or wifi config?
  if (currentMode == Mode::wificonfig)
  {
    // =================
    // WiFi config mode
    // =================

    webServer.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
      request->send(SPIFFS, "/wificonfig.html", String(), false, processor);
    });

    webServer.on("/save", HTTP_GET, [](AsyncWebServerRequest *request){
      if(request->hasArg("hostname"))
      {
        Serial.println("Save wifi config");
        WifiSettings settings = settingsManager.getWifiSettings();
        settings.hostname = request->arg("hostname");
        settings.ssid = request->arg("ssid");
        if (request->arg("password").equals("********")) // password is replaced by wildcards when given to the browser, so if the user didn't changed it, don't save it
          settings.password = settingsManager.getWifiSettings().password; // use the old, already saved, one
        else
          settings.password = request->arg("password");
        settingsManager.saveWifiSettings(settings);
        shouldReboot = true;
      }
      request->redirect("/");
    });


    webServer.onNotFound([](AsyncWebServerRequest *request){
      AsyncResponseStream *response = request->beginResponseStream("text/html");
      response->printf("<!DOCTYPE html><html><head><title>FingerprintDoorbell</title><meta http-equiv=\"refresh\" content=\"0; url=http://%s\" /></head><body>", WiFi.softAPIP().toString().c_str());
      response->printf("<p>Please configure your WiFi settings <a href='http://%s'>here</a> to connect FingerprintDoorbell to your home network.</p>", WiFi.softAPIP().toString().c_str());
      response->print("</body></html>");
      request->send(response);
    });

  }
  else
  {
    // =======================
    // normal operating mode
    // =======================
    events.onConnect([](AsyncEventSourceClient *client){
      if(client->lastId()){
        Serial.printf("Client reconnected! Last message ID it got was: %u\n", client->lastId());
      }
      //send event with message "ready", id current millis
      // and set reconnect delay to 1 second
      client->send(getLogMessagesAsHtml().c_str(),"message",millis(),1000);
    });
    webServer.addHandler(&events);

    
    // Route for root / web page
    webServer.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
      AsyncWebServerResponse *response = request->beginResponse(SPIFFS, "/app.html", "text/html");
      response->addHeader("Cache-Control", "no-cache");
      request->send(response);
    });

    // JSON API used by the web interface
    webServer.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *request){
      sendJson(request, getStatusJson());
    });

    webServer.on("/api/fingerprints", HTTP_GET, [](AsyncWebServerRequest *request){
      sendJson(request, fingerManager.getFingerListAsJson());
    });

    webServer.on("/api/settings", HTTP_GET, [](AsyncWebServerRequest *request){
      sendJson(request, getSettingsJson());
    });

    // change a LED setting without restart: /api/led?setting=touchRingColor&value=3
    webServer.on("/api/led", HTTP_GET, [](AsyncWebServerRequest *request){
      if (request->hasArg("setting") && request->hasArg("value"))
        applyLedSettingValue(request->arg("setting"), request->arg("value").toInt());
      sendJson(request, getSettingsJson());
    });

    // switch ignoreTouchRing / ringEnabled without restart: /api/control?ignoreTouchRing=on
    webServer.on("/api/control", HTTP_GET, [](AsyncWebServerRequest *request){
      if (request->hasArg("ignoreTouchRing")) {
        fingerManager.setIgnoreTouchRing(request->arg("ignoreTouchRing") == "on");
        publishControlState();
      }
      if (request->hasArg("ringEnabled"))
        setRingEnabled(request->arg("ringEnabled") == "on");
      sendJson(request, getStatusJson());
    });

    webServer.on("/enroll", HTTP_GET, [](AsyncWebServerRequest *request){
      if(request->hasArg("startEnrollment"))
      {
        enrollId = request->arg("newFingerprintId");
        enrollName = request->arg("newFingerprintName");
        numIterations = request->arg("newFingerprintIteration");
        currentMode = Mode::enroll;
      }
      request->redirect("/");
    });

    webServer.on("/editFingerprints", HTTP_GET, [](AsyncWebServerRequest *request){
      if(request->hasArg("selectedFingerprint"))
      {
        if(request->hasArg("btnDelete"))
        {
          int id = request->arg("selectedFingerprint").toInt();
          waitForMaintenanceMode();
          fingerManager.deleteFinger(id);
          currentMode = Mode::scan;
          updateClientsFingerlist(fingerManager.getFingerListAsJson());
        }
        else if (request->hasArg("btnRename"))
        {
          int id = request->arg("selectedFingerprint").toInt();
          String newName = request->arg("renameNewName");
          fingerManager.renameFinger(id, newName);
          updateClientsFingerlist(fingerManager.getFingerListAsJson());
        }
      }
      request->redirect("/");  
    });

    webServer.on("/settings", HTTP_GET, [](AsyncWebServerRequest *request){
      if(request->hasArg("btnSaveSettings"))
      {
        Serial.println("Save settings");
        AppSettings settings = settingsManager.getAppSettings();
        settings.mqttServer = request->arg("mqtt_server");
        settings.mqttUsername = request->arg("mqtt_username");
        settings.mqttPassword = request->arg("mqtt_password");
        settings.mqttRootTopic = request->arg("mqtt_rootTopic");
        settings.ntpServer = request->arg("ntpServer");
        settings.haDiscovery = request->hasArg("haDiscovery");
        settings.haDiscoveryPrefix = request->arg("haDiscoveryPrefix");
        settings.haDiscoveryPrefix.trim();
        if (settings.haDiscoveryPrefix.isEmpty())
          settings.haDiscoveryPrefix = "homeassistant";
        settings.touchRingActiveColor = constrain(request->arg("touchRingActiveColor").toInt(), 1, 7);
        settings.touchRingActiveSequence = constrain(request->arg("touchRingActiveSequence").toInt(), 1, 4);
        settings.scanColor = constrain(request->arg("scanColor").toInt(), 1, 7);
        settings.matchColor = constrain(request->arg("matchColor").toInt(), 1, 7);
        settingsManager.saveAppSettings(settings);
        request->redirect("/");  
        shouldReboot = true;
      } else {
        request->redirect("/#settings");
      }
    });

    webServer.on("/ring", HTTP_GET, [](AsyncWebServerRequest *request){
      if(request->hasArg("btnSaveRing"))
      {
        Serial.println("Save ring settings");
        RingSettings settings = settingsManager.getRingSettings();
        settings.ringEnabled = request->arg("ringEnabled").equals("on");
        settings.ringDuration = request->arg("ringDuration").toInt();        
        settingsManager.saveRingSettings(settings);
        publishControlState();
      }
      request->redirect("/#settings");
    });

    webServer.on("/pairing", HTTP_GET, [](AsyncWebServerRequest *request){
      if(request->hasArg("btnDoPairing"))
      {
        Serial.println("Do (re)pairing");
        doPairing();
        request->redirect("/");  
      } else {
        request->redirect("/#settings");
      }
    });



    webServer.on("/factoryReset", HTTP_GET, [](AsyncWebServerRequest *request){
      if(request->hasArg("btnFactoryReset"))
      {
        notifyClients("Factory reset initiated...");
        
        if (!fingerManager.deleteAll())
          notifyClients("Finger database could not be deleted.");
        
        if (!settingsManager.deleteAppSettings())
          notifyClients("App settings could not be deleted.");

        if (!settingsManager.deleteWifiSettings())
          notifyClients("Wifi settings could not be deleted.");
        
        request->redirect("/");  
        shouldReboot = true;
      } else {
        request->redirect("/#settings");
      }
    });


    webServer.on("/deleteAllFingerprints", HTTP_GET, [](AsyncWebServerRequest *request){
      if(request->hasArg("btnDeleteAllFingerprints"))
      {
        notifyClients("Deleting all fingerprints...");
        
        if (!fingerManager.deleteAll())
          notifyClients("Finger database could not be deleted.");
        
        request->redirect("/");  
        
      } else {
        request->redirect("/#settings");
      }
    });


    webServer.onNotFound([](AsyncWebServerRequest *request){
      request->send(404);
    });

    
  } // end normal operating mode


  // common url callbacks
  webServer.on("/reboot", HTTP_GET, [](AsyncWebServerRequest *request){
    request->redirect("/");
    shouldReboot = true;
  });

  webServer.on("/style.css", HTTP_GET, [](AsyncWebServerRequest *request){
    AsyncWebServerResponse *response = request->beginResponse(SPIFFS, "/style.css", "text/css");
    response->addHeader("Cache-Control", "no-cache");
    request->send(response);
  });

  webServer.on("/app.js", HTTP_GET, [](AsyncWebServerRequest *request){
    AsyncWebServerResponse *response = request->beginResponse(SPIFFS, "/app.js", "application/javascript");
    response->addHeader("Cache-Control", "no-cache");
    request->send(response);
  });


  // Enable Over-the-air updates at http://<IPAddress>/update
  AsyncElegantOTA.begin(&webServer);
  
  // Start server
  webServer.begin();

  notifyClients("System booted successfully!");

}


// publishes Home Assistant discovery, device status and LED settings
void publishMqttState() {
  const AppSettings settings = settingsManager.getAppSettings();
  publishHomeAssistantDiscovery(mqttClient, settings.haDiscovery, settings.haDiscoveryPrefix, settings.mqttRootTopic,
                                settingsManager.getWifiSettings().hostname, VersionInfo);
  publishDeviceStatus(mqttClient, settings.mqttRootTopic, fingerManager.connected);
  publishLedSettings(mqttClient, settings.mqttRootTopic, settings.touchRingActiveColor, settings.touchRingActiveSequence, settings.scanColor, settings.matchColor);
  mqttStatusPreviousMillis = millis();
}

// changes an LED setting received over MQTT (Home Assistant select entities)
void applyLedSetting(const String& setting, const String& value) {
  int option = (setting == "touchRingSequence") ? parseLedSequence(value) : parseLedColor(value);
  if (option > 0)
    applyLedSettingValue(setting, option);
}

void mqttCallback(char* topic, byte* message, unsigned int length) {
  Serial.print("Message arrived on topic: ");
  Serial.print(topic);
  Serial.print(". Message: ");
  String messageTemp;
  
  for (int i = 0; i < length; i++) {
    Serial.print((char)message[i]);
    messageTemp += (char)message[i];
  }
  Serial.println();

  // Home Assistant (re)started: announce entities again
  if (String(topic) == settingsManager.getAppSettings().haDiscoveryPrefix + "/status") {
    if (messageTemp == "online")
      publishMqttState();
    return;
  }

  // LED settings from Home Assistant: "<rootTopic>/<setting>/set"
  String ledPrefix = String(settingsManager.getAppSettings().mqttRootTopic) + "/";
  String topicString = String(topic);
  if (topicString.startsWith(ledPrefix) && topicString.endsWith("/set")) {
    applyLedSetting(topicString.substring(ledPrefix.length(), topicString.length() - 4), messageTemp);
    return;
  }

  // Check incomming message for interesting topics
  if (String(topic) == String(settingsManager.getAppSettings().mqttRootTopic) + "/reboot") {
    if (messageTemp == "press") {
      shouldReboot = true;
    }
  }

  if (String(topic) == String(settingsManager.getAppSettings().mqttRootTopic) + "/ignoreTouchRing") {
    if(messageTemp == "on"){
      fingerManager.setIgnoreTouchRing(true);
    }
    else if(messageTemp == "off"){
      fingerManager.setIgnoreTouchRing(false);
    }
  }
  if (String(topic) == String(settingsManager.getAppSettings().mqttRootTopic) + "/ringEnabled") {
              
    if(messageTemp == "on"){
      RingSettings settings = settingsManager.getRingSettings();  
      settings.ringEnabled = true;       
      settingsManager.saveRingSettings(settings);
       notifyClients("ringEnabled is now 'on'");
    }
    else if(messageTemp == "off"){
      RingSettings settings = settingsManager.getRingSettings();  
      settings.ringEnabled = false;       
      settingsManager.saveRingSettings(settings);
      notifyClients("ringEnabled is now 'off'");
    }
  }


  #ifdef CUSTOM_GPIOS
    if (String(topic) == String(settingsManager.getAppSettings().mqttRootTopic) + "/customOutput1") {
      if(messageTemp == "on"){
        digitalWrite(customOutput1, HIGH); 
      }
      else if(messageTemp == "off"){
        digitalWrite(customOutput1, LOW); 
      }
    }
    if (String(topic) == String(settingsManager.getAppSettings().mqttRootTopic) + "/customOutput2") {
      if(messageTemp == "on"){
        digitalWrite(customOutput2, HIGH); 
      }
      else if(messageTemp == "off"){
        digitalWrite(customOutput2, LOW); 
      }
    }
  #endif  

}

void connectMqttClient() {
  if (!mqttClient.connected() && mqttConfigValid) {
    Serial.print("(Re)connect to MQTT broker...");
    // Attempt to connect
    bool connectResult;
    
    // connect with or witout authentication
    // last will: availability topic (used by Home Assistant to mark all entities as unavailable)
    String lastWillTopic = settingsManager.getAppSettings().mqttRootTopic + "/connection";
    String lastWillMessage = "offline";
    if (settingsManager.getAppSettings().mqttUsername.isEmpty() || settingsManager.getAppSettings().mqttPassword.isEmpty())
      connectResult = mqttClient.connect(settingsManager.getWifiSettings().hostname.c_str(),lastWillTopic.c_str(), 1, true, lastWillMessage.c_str());
    else
      connectResult = mqttClient.connect(settingsManager.getWifiSettings().hostname.c_str(), settingsManager.getAppSettings().mqttUsername.c_str(), settingsManager.getAppSettings().mqttPassword.c_str(), lastWillTopic.c_str(), 1, true, lastWillMessage.c_str());

    if (connectResult) {
      // success
      Serial.println("connected");
      // Retain and ...
      String ignore = fingerManager.getIgnoreTouchRing() ? "on" : "off";
      String ring = settingsManager.getRingSettings().ringEnabled ? "on" : "off";

      mqttClient.publish((settingsManager.getAppSettings().mqttRootTopic + "/ignoreTouchRing").c_str(), ignore.c_str(), true);
      mqttClient.publish((settingsManager.getAppSettings().mqttRootTopic + "/ringEnabled").c_str(), ring.c_str(), true);
      mqttClient.publish((settingsManager.getAppSettings().mqttRootTopic + "/connection").c_str(), "online", true);

      // Home Assistant MQTT auto discovery, device status and LED settings
      publishMqttState();

      // ...Subscribe
      mqttClient.subscribe((settingsManager.getAppSettings().mqttRootTopic + "/ignoreTouchRing").c_str(), 1); // QoS = 1 (at least once)
      mqttClient.subscribe((settingsManager.getAppSettings().mqttRootTopic + "/ringEnabled").c_str(), 1); // QoS = 1 (at least once)
      mqttClient.subscribe((settingsManager.getAppSettings().mqttRootTopic + "/reboot").c_str(), 1); // QoS = 1 (at least once)
      mqttClient.subscribe((settingsManager.getAppSettings().mqttRootTopic + "/+/set").c_str(), 1); // LED settings
      mqttClient.subscribe((settingsManager.getAppSettings().haDiscoveryPrefix + "/status").c_str(), 1); // Home Assistant birth message
    
      #ifdef CUSTOM_GPIOS
        mqttClient.subscribe((settingsManager.getAppSettings().mqttRootTopic + "/customOutput1").c_str(), 1); // QoS = 1 (at least once)
        mqttClient.subscribe((settingsManager.getAppSettings().mqttRootTopic + "/customOutput2").c_str(), 1); // QoS = 1 (at least once)
      #endif



    } else {
      if (mqttClient.state() == 4 || mqttClient.state() == 5) {
        mqttConfigValid = false;
        notifyClients("Failed to connect to MQTT Server: bad credentials or not authorized. Will not try again, please check your settings.");
      } else {
        notifyClients(String("Failed to connect to MQTT Server, rc=") + mqttClient.state() + ", try again in 30 seconds");
      }
    }
  }
}


void doScan()
{
  Match match = fingerManager.scanFingerprint();
  String mqttRootTopic = settingsManager.getAppSettings().mqttRootTopic;
  switch(match.scanResult)
  {
    case ScanResult::noFinger:
      // standard case, occurs every iteration when no finger touchs the sensor
      if (match.scanResult != lastMatch.scanResult) {
        Serial.println("no finger");
        mqttClient.publish((String(mqttRootTopic) + "/ring").c_str(), "off");
        mqttClient.publish((String(mqttRootTopic) + "/matchId").c_str(), "-1");
        mqttClient.publish((String(mqttRootTopic) + "/matchName").c_str(), "");
        mqttClient.publish((String(mqttRootTopic) + "/matchConfidence").c_str(), "-1");
      }
      break; 
    case ScanResult::matchFound:
      notifyClients( String("Match Found: ") + match.matchId + " - " + match.matchName  + " with confidence of " + match.matchConfidence );
      if (match.scanResult != lastMatch.scanResult) {
        lastEventType = "match";
        lastEventName = match.matchName;
        lastEventId = match.matchId;
        lastEventConfidence = match.matchConfidence;
        lastEventTime = getTimestampString();
        if (checkPairingValid()) {
          mqttClient.publish((String(mqttRootTopic) + "/ring").c_str(), "off");
          mqttClient.publish((String(mqttRootTopic) + "/matchId").c_str(), String(match.matchId).c_str());
          mqttClient.publish((String(mqttRootTopic) + "/matchName").c_str(), match.matchName.c_str());
          mqttClient.publish((String(mqttRootTopic) + "/matchConfidence").c_str(), String(match.matchConfidence).c_str());
          Serial.println("MQTT message sent: Open the door!");
        } else {
          notifyClients("Security issue! Match was not sent by MQTT because of invalid sensor pairing! This could potentially be an attack! If the sensor is new or has been replaced by you do a (re)pairing in settings page.");
        }
      }
      delay(3000); // wait some time before next scan to let the LED blink
      break;
    case ScanResult::noMatchFound:
      notifyClients(String("No Match Found (Code ") + match.returnCode + ")");
      if (match.scanResult != lastMatch.scanResult) {
        lastEventType = "ring";
        lastEventName = "";
        lastEventId = 0;
        lastEventConfidence = 0;
        lastEventTime = getTimestampString();
        if(settingsManager.getRingSettings().ringEnabled) {
          digitalWrite(doorbellOutputPin, HIGH);
        }
        
        mqttClient.publish((String(mqttRootTopic) + "/ring").c_str(), "on");
        mqttClient.publish((String(mqttRootTopic) + "/matchId").c_str(), "-1");
        mqttClient.publish((String(mqttRootTopic) + "/matchName").c_str(), "");
        mqttClient.publish((String(mqttRootTopic) + "/matchConfidence").c_str(), "-1");
        Serial.println("MQTT message sent: ring the bell!");
        delay(settingsManager.getRingSettings().ringDuration);        
        digitalWrite(doorbellOutputPin, LOW); 
      } else {
        delay(500); // wait some time before next scan to let the LED blink
      }
      break;
    case ScanResult::error:
      notifyClients(String("ScanResult Error (Code ") + match.returnCode + ")");
      break;
  };
  lastMatch = match;

}

void doEnroll()
{
  int id = enrollId.toInt();
  if (id < 1 || id > 200) {
    notifyClients("Invalid memory slot id '" + enrollId + "'");
    return;
  }

  int iterations = numIterations.toInt();
  if (iterations < 1 || iterations > 5) {
    notifyClients("Invalid iterations '" + numIterations + "'");
    return;
  }

  NewFinger finger = fingerManager.enrollFinger(id, iterations, enrollName);
  if (finger.enrollResult == EnrollResult::ok) {
    notifyClients("Enrollment successfull. You can now use your new finger for scanning.");
    updateClientsFingerlist(fingerManager.getFingerListAsJson());
  }  else if (finger.enrollResult == EnrollResult::error) {
    notifyClients(String("Enrollment failed. (Code ") + finger.returnCode + ")");
  }
}



// splits "host" or "host:port" into hostname and port (default 1883)
void splitHostAndPort(const String& serverConfig, String& hostname, uint16_t& port) {
  int positionOfColon = serverConfig.indexOf(':');
  if (positionOfColon >= 0) {
    hostname = serverConfig.substring(0, positionOfColon);
    long parsedPort = serverConfig.substring(positionOfColon + 1).toInt();
    port = (parsedPort > 0 && parsedPort <= 65535) ? parsedPort : 1883;
  } else {
    hostname = serverConfig;
    port = 1883;
  }
}

void reboot()
{
  notifyClients("System is rebooting now...");
  delay(1000);
    
  mqttClient.disconnect();
  espClient.stop();
  dnsServer.stop();
  webServer.end();
  WiFi.disconnect();
  ESP.restart();
}


void setup()
{
  // open serial monitor for debug infos
  Serial.begin(115200);
  while (!Serial);  // For Yun/Leo/Micro/Zero/...
  delay(100);

  // initialize GPIOs
  pinMode(doorbellOutputPin, OUTPUT); 
  #ifdef CUSTOM_GPIOS
    pinMode(customOutput1, OUTPUT); 
    pinMode(customOutput2, OUTPUT); 
    pinMode(customInput1, INPUT_PULLDOWN);
    pinMode(customInput2, INPUT_PULLDOWN);
  #endif  

  settingsManager.loadWifiSettings();
  settingsManager.loadAppSettings();
  settingsManager.loadRingSettings();

  fingerManager.connect();
  
  if (!checkPairingValid())
    notifyClients("Security issue! Pairing with sensor is invalid. This could potentially be an attack! If the sensor is new or has been replaced by you do a (re)pairing in settings page. MQTT messages regarding matching fingerprints will not been sent until pairing is valid again.");

  if (fingerManager.isFingerOnSensor() || !settingsManager.isWifiConfigured())
  {
    // ring touched during startup or no wifi settings stored -> wifi config mode
    currentMode = Mode::wificonfig;
    Serial.println("Started WiFi-Config mode");
    fingerManager.setLedRingWifiConfig();
    initWiFiAccessPointForConfiguration();
    startWebserver();

  } else {
    Serial.println("Started normal operating mode");
    currentMode = Mode::scan;
    if (initWifi()) {
      startWebserver();
      if (settingsManager.getAppSettings().mqttServer.isEmpty()) {
        mqttConfigValid = false;
        notifyClients("Error: No MQTT Broker is configured! Please go to settings and enter your server URL + user credentials.");
      } else {
        delay(5000);
        String mqttHostname;
        uint16_t mqttPort;
        splitHostAndPort(settingsManager.getAppSettings().mqttServer, mqttHostname, mqttPort);

        IPAddress mqttServerIp;
        if (WiFi.hostByName(mqttHostname.c_str(), mqttServerIp))
        {
          mqttConfigValid = true;
          Serial.println("IP used for MQTT server: " + mqttServerIp.toString() + ":" + String(mqttPort));
          mqttClient.setServer(mqttServerIp , mqttPort);
          mqttClient.setBufferSize(1280); // default (256 bytes) is too small for Home Assistant discovery messages
          mqttClient.setCallback(mqttCallback);
          connectMqttClient();
        }
        else {
          mqttConfigValid = false;
          notifyClients("MQTT Server '" + settingsManager.getAppSettings().mqttServer + "' not found. Please check your settings.");
        }
      }
      if (fingerManager.connected) {
        const AppSettings& s = settingsManager.getAppSettings();
        fingerManager.configLedRing(s.touchRingActiveColor, s.touchRingActiveSequence, s.scanColor, s.matchColor);
        fingerManager.setLedRingReady();
      }
      else
        fingerManager.setLedRingError();
    }  else {
      fingerManager.setLedRingError();
      shouldReboot = true;
    }

  }
  
}

void loop()
{
  // shouldReboot flag for supporting reboot through webui
  if (shouldReboot) {
    reboot();
  }
  
  // Reconnect handling
  if (currentMode != Mode::wificonfig)
  {
    unsigned long currentMillis = millis();
    // reconnect WiFi if down for 30s
    if ((WiFi.status() != WL_CONNECTED) && (currentMillis - wifiReconnectPreviousMillis >= 30000ul)) {
      Serial.println("Reconnecting to WiFi...");
      WiFi.disconnect();
      WiFi.reconnect();
      wifiReconnectPreviousMillis = currentMillis;
    }

    // reconnect mqtt if down
    if (!settingsManager.getAppSettings().mqttServer.isEmpty()) {
      if (!mqttClient.connected() && (currentMillis - mqttReconnectPreviousMillis >= 30000ul)) {
        connectMqttClient();
        mqttReconnectPreviousMillis = currentMillis;
      }
      mqttClient.loop();

      // publish uptime, wifi signal etc. periodically
      if (mqttClient.connected() && (currentMillis - mqttStatusPreviousMillis >= mqttStatusIntervalMillis)) {
        publishDeviceStatus(mqttClient, settingsManager.getAppSettings().mqttRootTopic, fingerManager.connected);
        mqttStatusPreviousMillis = currentMillis;
      }
    }
  }


  // the LED ring shows when the hardware gong is disabled, no matter where the setting was changed (web, MQTT)
  if (fingerManager.setGongEnabled(settingsManager.getRingSettings().ringEnabled) && currentMode == Mode::scan && fingerManager.connected)
    fingerManager.setLedRingReady();

  // do the actual loop work
  switch (currentMode)
  {
  case Mode::scan:
    if (fingerManager.connected)
      doScan();
    break;
  
  case Mode::enroll:
    doEnroll();
    currentMode = Mode::scan; // switch back to scan mode after enrollment is done
    break;
  
  case Mode::wificonfig:
    dnsServer.processNextRequest(); // used for captive portal redirect
    break;

  case Mode::maintenance:
    // do nothing, give webserver exclusive access to sensor (not thread-safe for concurrent calls)
    break;

  }

  // enter maintenance mode (no continous scanning) if requested
  if (needMaintenanceMode)
    currentMode = Mode::maintenance;

  #ifdef CUSTOM_GPIOS
    // read custom inputs and publish by MQTT
    bool i1;
    bool i2;
    i1 = (digitalRead(customInput1) == HIGH);
    i2 = (digitalRead(customInput2) == HIGH);

    String mqttRootTopic = settingsManager.getAppSettings().mqttRootTopic;
    if (i1 != customInput1Value) {
        if (i1)
          mqttClient.publish((String(mqttRootTopic) + "/customInput1").c_str(), "on");      
        else
          mqttClient.publish((String(mqttRootTopic) + "/customInput1").c_str(), "off");      
    }

    if (i2 != customInput2Value) {
        if (i2)
          mqttClient.publish((String(mqttRootTopic) + "/customInput2").c_str(), "on");      
        else
          mqttClient.publish((String(mqttRootTopic) + "/customInput2").c_str(), "off");  
    }

    customInput1Value = i1;
    customInput2Value = i2;

  #endif  

}


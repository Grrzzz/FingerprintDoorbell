#include "HomeAssistantDiscovery.h"
#include <WiFi.h>
#include <esp_timer.h>

const char* const LedColorNames[7] = {"Red", "Blue", "Purple", "Green", "Yellow", "Cyan", "White"};
const char* const LedSequenceNames[4] = {"Breathing", "Flashing", "On", "Off"};

namespace {

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

// unique per device, also stable when the hostname is changed
String getNodeId() {
  String mac = WiFi.macAddress(); // AA:BB:CC:DD:EE:FF
  mac.replace(":", "");
  mac.toLowerCase();
  return "fingerprintdoorbell_" + mac;
}

int parseOption(const char* const* names, int count, const String& name) {
  for (int i = 0; i < count; i++) {
    if (name.equalsIgnoreCase(names[i]))
      return i + 1;
  }
  return 0;
}

String optionsJson(const char* const* names, int count) {
  String json = "\"options\":[";
  for (int i = 0; i < count; i++) {
    if (i > 0)
      json += ",";
    json += String("\"") + names[i] + "\"";
  }
  return json + "]";
}

enum class Options { none, colors, sequences };

struct Entity {
  const char* component;     // Home Assistant platform: sensor, binary_sensor, switch, button, select
  const char* objectId;      // used for unique_id and discovery topic
  const char* name;          // friendly name (device name is prepended by Home Assistant)
  const char* stateSuffix;   // state topic below the root topic (nullptr = none)
  const char* commandSuffix; // command topic below the root topic (nullptr = none)
  Options options;           // option list for select entities
  const char* extra;         // additional JSON fields (without enclosing braces)
};

const Entity entities[] = {
  // fingerprint events
  {"binary_sensor", "ring",             "Ring",              "ring",            nullptr, Options::none, "\"payload_on\":\"on\",\"payload_off\":\"off\",\"icon\":\"mdi:doorbell\""},
  {"sensor",        "match_name",       "Match",             "matchName",       nullptr, Options::none, "\"icon\":\"mdi:fingerprint\""},
  {"sensor",        "match_confidence", "Confidence",        "matchConfidence", nullptr, Options::none, "\"icon\":\"mdi:percent\""},
  {"sensor",        "match_id",         "Match ID",          "matchId",         nullptr, Options::none, "\"icon\":\"mdi:identifier\""},
  // controls (switches use one topic for state and command)
  {"switch",        "ignore_touch",     "Ignore Touch",      "ignoreTouchRing", "ignoreTouchRing", Options::none, "\"payload_on\":\"on\",\"payload_off\":\"off\",\"state_on\":\"on\",\"state_off\":\"off\",\"icon\":\"mdi:hand-back-left-off\""},
  {"switch",        "ring_enabled",     "Ring Enabled",      "ringEnabled",     "ringEnabled",     Options::none, "\"payload_on\":\"on\",\"payload_off\":\"off\",\"state_on\":\"on\",\"state_off\":\"off\",\"icon\":\"mdi:bell-ring\""},
  {"button",        "reboot",           "Reboot",            nullptr,           "reboot",          Options::none, "\"payload_press\":\"press\",\"device_class\":\"restart\",\"entity_category\":\"config\""},
  // LED settings (same as in the web interface)
  {"select",        "led_ring_color",   "Touch Ring Color",    "touchRingColor",    "touchRingColor/set",    Options::colors,    "\"icon\":\"mdi:palette\",\"entity_category\":\"config\""},
  {"select",        "led_ring_sequence","Touch Ring Sequence", "touchRingSequence", "touchRingSequence/set", Options::sequences, "\"icon\":\"mdi:led-on\",\"entity_category\":\"config\""},
  {"select",        "led_scan_color",   "Finger Detected Color","scanColor",        "scanColor/set",         Options::colors,    "\"icon\":\"mdi:palette\",\"entity_category\":\"config\""},
  {"select",        "led_match_color",  "Match Color",         "matchColor",        "matchColor/set",        Options::colors,    "\"icon\":\"mdi:palette\",\"entity_category\":\"config\""},
  // diagnostics
  {"sensor",        "uptime",           "Uptime",            "uptime",          nullptr, Options::none, "\"device_class\":\"duration\",\"unit_of_measurement\":\"s\",\"state_class\":\"total_increasing\",\"entity_category\":\"diagnostic\""},
  {"sensor",        "wifi_rssi",        "WiFi Signal",       "wifiRssi",        nullptr, Options::none, "\"device_class\":\"signal_strength\",\"unit_of_measurement\":\"dBm\",\"state_class\":\"measurement\",\"entity_category\":\"diagnostic\""},
  {"sensor",        "ip_address",       "IP Address",        "ipAddress",       nullptr, Options::none, "\"icon\":\"mdi:ip-network\",\"entity_category\":\"diagnostic\""},
  {"sensor",        "free_heap",        "Free Heap",         "freeHeap",        nullptr, Options::none, "\"device_class\":\"data_size\",\"unit_of_measurement\":\"B\",\"state_class\":\"measurement\",\"entity_category\":\"diagnostic\",\"enabled_by_default\":false"},
  {"binary_sensor", "sensor_connected", "Sensor Connected",  "sensorConnected", nullptr, Options::none, "\"payload_on\":\"on\",\"payload_off\":\"off\",\"device_class\":\"connectivity\",\"entity_category\":\"diagnostic\""},
};

} // namespace

int parseLedColor(const String& name) {
  return parseOption(LedColorNames, 7, name);
}

int parseLedSequence(const String& name) {
  return parseOption(LedSequenceNames, 4, name);
}

void publishHomeAssistantDiscovery(PubSubClient& mqttClient, bool enabled, const String& discoveryPrefix, const String& rootTopic,
                                   const String& deviceName, const String& firmwareVersion) {
  String nodeId = getNodeId();

  String device = "\"device\":{\"identifiers\":[\"" + nodeId + "\"]"
                  ",\"name\":\"" + jsonEscape(deviceName) + "\""
                  ",\"manufacturer\":\"FingerprintDoorbell\""
                  ",\"model\":\"ESP32 + R503\""
                  ",\"sw_version\":\"" + jsonEscape(firmwareVersion) + "\""
                  ",\"configuration_url\":\"http://" + WiFi.localIP().toString() + "\"}";

  for (const Entity& e : entities) {
    String configTopic = discoveryPrefix + "/" + e.component + "/" + nodeId + "/" + e.objectId + "/config";

    // an empty retained payload removes the entity from Home Assistant
    String payload = "";
    if (enabled) {
      payload = String("{\"name\":\"") + e.name + "\""
                ",\"unique_id\":\"" + nodeId + "_" + e.objectId + "\""
                ",\"has_entity_name\":true"
                ",\"availability_topic\":\"" + rootTopic + "/connection\""
                ",\"payload_available\":\"online\",\"payload_not_available\":\"offline\"";
      if (e.stateSuffix)
        payload += String(",\"state_topic\":\"") + rootTopic + "/" + e.stateSuffix + "\"";
      if (e.commandSuffix)
        payload += String(",\"command_topic\":\"") + rootTopic + "/" + e.commandSuffix + "\"";
      if (e.options == Options::colors)
        payload += "," + optionsJson(LedColorNames, 7);
      else if (e.options == Options::sequences)
        payload += "," + optionsJson(LedSequenceNames, 4);
      payload += String(",") + e.extra + "," + device + "}";
    }
    mqttClient.publish(configTopic.c_str(), payload.c_str(), true);
  }
}

// state is published retained: Home Assistant subscribes to the state topics only after it processed the discovery
// message, so non-retained values published right after the discovery would be lost (entities stay "unknown")
void publishDeviceStatus(PubSubClient& mqttClient, const String& rootTopic, bool sensorConnected) {
  mqttClient.publish((rootTopic + "/uptime").c_str(), String((uint32_t)(esp_timer_get_time() / 1000000ULL)).c_str(), true);
  mqttClient.publish((rootTopic + "/wifiRssi").c_str(), String(WiFi.RSSI()).c_str(), true);
  mqttClient.publish((rootTopic + "/ipAddress").c_str(), WiFi.localIP().toString().c_str(), true);
  mqttClient.publish((rootTopic + "/freeHeap").c_str(), String(ESP.getFreeHeap()).c_str(), true);
  mqttClient.publish((rootTopic + "/sensorConnected").c_str(), sensorConnected ? "on" : "off", true);
}

void publishLedSettings(PubSubClient& mqttClient, const String& rootTopic, int touchRingColor, int touchRingSequence, int scanColor, int matchColor) {
  if (touchRingColor >= 1 && touchRingColor <= 7)
    mqttClient.publish((rootTopic + "/touchRingColor").c_str(), LedColorNames[touchRingColor - 1], true);
  if (touchRingSequence >= 1 && touchRingSequence <= 4)
    mqttClient.publish((rootTopic + "/touchRingSequence").c_str(), LedSequenceNames[touchRingSequence - 1], true);
  if (scanColor >= 1 && scanColor <= 7)
    mqttClient.publish((rootTopic + "/scanColor").c_str(), LedColorNames[scanColor - 1], true);
  if (matchColor >= 1 && matchColor <= 7)
    mqttClient.publish((rootTopic + "/matchColor").c_str(), LedColorNames[matchColor - 1], true);
}

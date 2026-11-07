#ifndef HOMEASSISTANTDISCOVERY_H
#define HOMEASSISTANTDISCOVERY_H

#include <PubSubClient.h>
#include <WString.h>

// Names of the LED options as shown in Home Assistant. Index = R503 value - 1
// colors: 1=red 2=blue 3=purple 4=green 5=yellow 6=cyan 7=white
// sequences: 1=breathing 2=flashing 3=on 4=off
extern const char* const LedColorNames[7];
extern const char* const LedSequenceNames[4];

// Returns the R503 value (1-based) for an option name received from Home Assistant, 0 if unknown.
int parseLedColor(const String& name);
int parseLedSequence(const String& name);

// Publishes (or removes, if enabled=false) the retained Home Assistant MQTT discovery configs for all entities.
// All entities share the availability topic "<rootTopic>/connection" ("online"/"offline", last will of the MQTT connection).
void publishHomeAssistantDiscovery(PubSubClient& mqttClient, bool enabled, const String& discoveryPrefix, const String& rootTopic,
                                   const String& deviceName, const String& firmwareVersion);

// Publishes the diagnostic values (uptime, wifi rssi, ip, free heap, sensor connection) to "<rootTopic>/..."
void publishDeviceStatus(PubSubClient& mqttClient, const String& rootTopic, bool sensorConnected);

// Publishes the current LED settings (state of the select entities) to "<rootTopic>/..."
void publishLedSetting(PubSubClient& mqttClient, const String& rootTopic, const String& name, int value, bool isColor);

#endif

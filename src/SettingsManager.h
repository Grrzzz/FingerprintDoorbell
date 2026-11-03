#ifndef SETTINGSMANAGER_H
#define SETTINGSMANAGER_H

#include <Preferences.h>
#include "global.h"

struct WifiSettings {    
    String ssid = "";
    String password = "";
    String hostname = "";
};

struct AppSettings {
    String mqttServer = "";
    String mqttUsername = "";
    String mqttPassword = "";
    String mqttRootTopic = "fingerprintDoorbell";
    String ntpServer = "pool.ntp.org";
    String timezone = "CET-1CEST,M3.5.0,M10.5.0/3"; // POSIX TZ string, default Europe/Berlin
    String sensorPin = "00000000";
    String sensorPairingCode = "";
    bool   sensorPairingValid = false;
    // LED settings (R503 values: colors 1=red 2=blue 3=purple 4=green 5=yellow 6=cyan 7=white; sequences 1=breathing 2=flashing 3=on 4=off)
    int8_t touchRingActiveColor = 7;
    int8_t touchRingActiveSequence = 3;
    int8_t scanColor = 7;
    int8_t matchColor = 4;
    bool   haDiscovery = true; // publish Home Assistant MQTT discovery messages
    String haDiscoveryPrefix = "homeassistant";
};

struct RingSettings {    
    bool ringEnabled = true;
    int ringDuration = 275;
};

class SettingsManager {       
  private:
    WifiSettings wifiSettings;
    AppSettings appSettings;
    RingSettings ringSettings;

    void saveWifiSettings();
    void saveAppSettings();
    void saveRingSettings();

public:
    bool loadWifiSettings();
    bool loadAppSettings();
    bool loadRingSettings();

    WifiSettings getWifiSettings();
    void saveWifiSettings(WifiSettings newSettings);
    
    AppSettings getAppSettings();
    void saveAppSettings(AppSettings newSettings);

    RingSettings getRingSettings();
    void saveRingSettings(RingSettings newSettings);

    bool isWifiConfigured();

    bool deleteAppSettings();
    bool deleteWifiSettings();
    bool deleteRingSettings();

    String generateNewPairingCode();

};

#endif
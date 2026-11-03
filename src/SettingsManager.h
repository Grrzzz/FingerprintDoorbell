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
    String sensorPin = "00000000";
    String sensorPairingCode = "";
    bool   sensorPairingValid = false;    
};

struct RingSettings {    
    bool ringEnabled = true;
    int ringDuration = 500;
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
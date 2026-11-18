#include "SettingsManager.h"
#include <mbedtls/sha256.h>

bool SettingsManager::loadWifiSettings() {
    Preferences preferences;
    if (preferences.begin("wifiSettings", true)) {
        wifiSettings.ssid = preferences.getString("ssid", String(""));
        wifiSettings.password = preferences.getString("password", String(""));
        wifiSettings.hostname = preferences.getString("hostname", String("FingerprintDoorbell"));
        wifiSettings.staticIp = preferences.getBool("staticIp", false);
        wifiSettings.ip = preferences.getString("ip", String(""));
        wifiSettings.subnet = preferences.getString("subnet", String(""));
        wifiSettings.gateway = preferences.getString("gateway", String(""));
        wifiSettings.dns = preferences.getString("dns", String(""));
        preferences.end();
        return true;
    } else {
        return false;
    }
}

bool SettingsManager::loadAppSettings() {
    Preferences preferences;
    if (preferences.begin("appSettings", true)) {
        appSettings.mqttServer = preferences.getString("mqttServer", String(""));
        appSettings.mqttEnabled = preferences.getBool("mqttEnabled", !appSettings.mqttServer.isEmpty()); // older versions: a broker means on
        appSettings.mqttUsername = preferences.getString("mqttUsername", String(""));
        appSettings.mqttPassword = preferences.getString("mqttPassword", String(""));
        appSettings.mqttRootTopic = preferences.getString("mqttRootTopic", String("fingerprintDoorbell"));
        appSettings.ntpServer = preferences.getString("ntpServer", String("pool.ntp.org"));
        appSettings.timezone = preferences.getString("timezone", String("CET-1CEST,M3.5.0,M10.5.0/3"));
        appSettings.sensorPin = preferences.getString("sensorPin", "00000000");
        appSettings.sensorPairingCode = preferences.getString("pairingCode", "");
        appSettings.sensorPairingValid = preferences.getBool("pairingValid", false);
        appSettings.touchRingActiveColor = preferences.getShort("ringActCol", 7);
        appSettings.touchRingActiveSequence = preferences.getShort("ringActSeq", 3);
        appSettings.scanColor = preferences.getShort("scanColor", 7);
        appSettings.matchColor = preferences.getShort("matchColor", 4);
        appSettings.scanSequence = preferences.getShort("scanSeq", 1);
        appSettings.matchSequence = preferences.getShort("matchSeq", 3);
        appSettings.ignoreColor = preferences.getShort("ignCol", 6);
        appSettings.ignoreSequence = preferences.getShort("ignSeq", 3);
        appSettings.gongOffColor = preferences.getShort("gongCol", 1);
        appSettings.gongOffSequence = preferences.getShort("gongSeq", 3);
        appSettings.haDiscovery = preferences.getBool("haDiscovery", true);
        appSettings.haDiscoveryPrefix = preferences.getString("haPrefix", String("homeassistant"));
        preferences.end();
        return true;
    } else {
        return false;
    }
}

bool SettingsManager::loadRingSettings() {
    Preferences preferences;
    if (preferences.begin("ringSettings", true)) {
        ringSettings.ringDuration = preferences.getInt("ringDuration", 275);
        ringSettings.ringEnabled = preferences.getBool("ringEnabled", true);
        preferences.end();
        return true;
    } else {
        return false;
    }
}
   
void SettingsManager::saveWifiSettings() {
    Preferences preferences;
    preferences.begin("wifiSettings", false); 
    preferences.putString("ssid", wifiSettings.ssid);
    preferences.putString("password", wifiSettings.password);
    preferences.putString("hostname", wifiSettings.hostname);
    preferences.putBool("staticIp", wifiSettings.staticIp);
    preferences.putString("ip", wifiSettings.ip);
    preferences.putString("subnet", wifiSettings.subnet);
    preferences.putString("gateway", wifiSettings.gateway);
    preferences.putString("dns", wifiSettings.dns);
    preferences.end();
}

void SettingsManager::saveAppSettings() {
    Preferences preferences;
    preferences.begin("appSettings", false); 
    preferences.putBool("mqttEnabled", appSettings.mqttEnabled);
    preferences.putString("mqttServer", appSettings.mqttServer);
    preferences.putString("mqttUsername", appSettings.mqttUsername);
    preferences.putString("mqttPassword", appSettings.mqttPassword);
    preferences.putString("mqttRootTopic", appSettings.mqttRootTopic);
    preferences.putString("ntpServer", appSettings.ntpServer);
    preferences.putString("timezone", appSettings.timezone);
    preferences.putString("sensorPin", appSettings.sensorPin);
    preferences.putString("pairingCode", appSettings.sensorPairingCode);
    preferences.putBool("pairingValid", appSettings.sensorPairingValid);
    preferences.putShort("ringActCol", appSettings.touchRingActiveColor);
    preferences.putShort("ringActSeq", appSettings.touchRingActiveSequence);
    preferences.putShort("scanColor", appSettings.scanColor);
    preferences.putShort("matchColor", appSettings.matchColor);
    preferences.putShort("scanSeq", appSettings.scanSequence);
    preferences.putShort("matchSeq", appSettings.matchSequence);
    preferences.putShort("ignCol", appSettings.ignoreColor);
    preferences.putShort("ignSeq", appSettings.ignoreSequence);
    preferences.putShort("gongCol", appSettings.gongOffColor);
    preferences.putShort("gongSeq", appSettings.gongOffSequence);
    preferences.putBool("haDiscovery", appSettings.haDiscovery);
    preferences.putString("haPrefix", appSettings.haDiscoveryPrefix);
    preferences.end();
}

void SettingsManager::saveRingSettings() {
    Preferences preferences;
    preferences.begin("ringSettings", false); 
    preferences.putInt("ringDuration", ringSettings.ringDuration);
    preferences.putBool("ringEnabled", ringSettings.ringEnabled);    
    preferences.end();
}

WifiSettings SettingsManager::getWifiSettings() {
    return wifiSettings;
}

void SettingsManager::saveWifiSettings(WifiSettings newSettings) {
    wifiSettings = newSettings;
    saveWifiSettings();
}

AppSettings SettingsManager::getAppSettings() {
    return appSettings;
}

void SettingsManager::saveAppSettings(AppSettings newSettings) {
    appSettings = newSettings;
    saveAppSettings();
}

RingSettings SettingsManager::getRingSettings() {
    return ringSettings;
}

void SettingsManager::saveRingSettings(RingSettings newSettings) {
    ringSettings = newSettings;
    saveRingSettings();
}

bool SettingsManager::isWifiConfigured() {
    if (wifiSettings.ssid.isEmpty() || wifiSettings.password.isEmpty())
        return false;
    else
        return true;
}

bool SettingsManager::deleteAppSettings() {
    bool rc;
    Preferences preferences;
    rc = preferences.begin("appSettings", false); 
    if (rc)
        rc = preferences.clear();
    preferences.end();
    return rc;
}

bool SettingsManager::deleteWifiSettings() {
    bool rc;
    Preferences preferences;
    rc = preferences.begin("wifiSettings", false); 
    if (rc)
        rc = preferences.clear();
    preferences.end();
    return rc;
}

bool SettingsManager::deleteRingSettings() {
    bool rc;
    Preferences preferences;
    rc = preferences.begin("ringSettings", false); 
    if (rc)
        rc = preferences.clear();
    preferences.end();
    return rc;
}

String SettingsManager::generateNewPairingCode() {

    /* Create a SHA256 hash */
    mbedtls_sha256_context hasher;
    mbedtls_sha256_init(&hasher);
    mbedtls_sha256_starts_ret(&hasher, 0);
    auto feed = [&hasher](const String& s) { mbedtls_sha256_update_ret(&hasher, (const unsigned char*)s.c_str(), s.length()); };

    /* Put some unique values as input in our new hash */
    feed(String(esp_random())); // random number
    feed(String(millis())); // time since boot
    feed(getTimestampString()); // current time (if NTP is available)
    feed(appSettings.mqttUsername);
    feed(appSettings.mqttPassword);
    feed(wifiSettings.ssid);
    feed(wifiSettings.password);

    /* Compute the final hash */
    byte hash[32];
    mbedtls_sha256_finish_ret(&hasher, hash);
    mbedtls_sha256_free(&hasher);
    
    // Convert our 32 byte hash to 32 chars long hex string. When converting the entire hash to hex we would need a length of 64 chars.
    // But because we only want a length of 32 we only use the first 16 bytes of the hash. I know this will increase possible collisions,
    // but for detecting a sensor replacement (which is the use-case here) it will still be enough.
    char hexString[33];
    hexString[32] = 0; // null terminatation byte for converting to string later
    for (byte i=0; i < 16; i++) // use only the first 16 bytes of hash
    {
        sprintf(&hexString[i*2], "%02x", hash[i]);
    }

    return String((char*)hexString);
}


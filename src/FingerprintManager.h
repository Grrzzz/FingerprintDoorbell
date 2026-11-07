#ifndef FINGERPRINTMANAGER_H
#define FINGERPRINTMANAGER_H

#include <Adafruit_Fingerprint.h>
#include <Preferences.h>
#include "global.h"

#define mySerial Serial2

#define FINGERPRINT_WRITENOTEPAD 0x18 // Write Notepad on sensor
#define FINGERPRINT_READNOTEPAD 0x19 // Read Notepad from sensor


/*
  By using the touch ring as an additional input to the image sensor the sensitivity is much higher for door bell ring events. Unfortunately
  we cannot differ between touches on the ring by fingers or rain drops, so rain on the ring will cause false alarms.
*/
const int touchRingPin = 5;     // touch/wakeup pin connected to fingerprint sensor

enum class ScanResult { noFinger, matchFound, noMatchFound, error };
enum class EnrollResult { ok, error };

struct LedStyle {
  uint8_t color;    // R503 color value (FINGERPRINT_LED_*)
  uint8_t sequence; // R503 control value (breathing/flashing/on/off)
  uint8_t speed;    // animation speed for breathing/flashing
};

// per fingerprint: manual on/off switch and an optional weekly schedule made of up to 4 time windows
const int MAX_FINGER_WINDOWS = 4;

struct FingerWindow {
  uint8_t days = 0;     // bit 0 = Monday ... bit 6 = Sunday
  uint16_t from = 0;    // minutes since midnight
  uint16_t to = 0;      // minutes since midnight (exclusive); to < from wraps over midnight
  bool allDay = false;  // ignore from/to, the selected days count completely
};

struct FingerPolicy {
  bool enabled = true;
  bool scheduled = false; // when set, the windows decide (the manual switch is ignored)
  uint8_t windowCount = 0;
  FingerWindow windows[MAX_FINGER_WINDOWS];
};

struct Match {
  ScanResult scanResult = ScanResult::noFinger;
  uint16_t matchId = 0;
  String matchName = "unknown";
  uint16_t matchConfidence = 0;
  uint8_t returnCode = 0;
  // set when a known fingerprint was recognized but is switched off / outside its time window
  uint16_t disabledId = 0;
  String disabledName = "";
  String disabledReason = "";
};

struct NewFinger {
  EnrollResult enrollResult = EnrollResult::error;
  uint8_t returnCode = 0;
};

class FingerprintManager {       
  private:
    Adafruit_Fingerprint finger = Adafruit_Fingerprint(&mySerial);
    bool lastTouchState = false;
    String fingerList[201];
    int fingerCountOnSensor = 0;
    bool ignoreTouchRing = false; // set to true when the sensor is usually exposed to rain to avoid false ring events. Can also be set conditional by a rain sensor over MQTT
    bool lastIgnoreTouchRing = false;

    // LED settings, defaults can be overridden with configLedRing()
    LedStyle ledReady   = {FINGERPRINT_LED_WHITE, FINGERPRINT_LED_ON, 250};        // ring ready (touch ring active)
    LedStyle ledScan    = {FINGERPRINT_LED_WHITE, FINGERPRINT_LED_BREATHING, 25};  // finger detected on sensor
    LedStyle ledMatch   = {FINGERPRINT_LED_GREEN, FINGERPRINT_LED_ON, 25};         // fingerprint matched
    LedStyle ledIgnore  = {FINGERPRINT_LED_CYAN, FINGERPRINT_LED_ON, 250};         // touch ring ignored
    LedStyle ledGongOff = {FINGERPRINT_LED_RED, FINGERPRINT_LED_ON, 250};          // hardware gong disabled
    void applyLed(const LedStyle& style);
    FingerPolicy policies[201];
    bool persistGroup(int id);          // writes the storage group that contains this fingerprint
    void migrateLegacyStorage();       // older versions stored one key per fingerprint
    bool isInSchedule(const FingerPolicy& policy);
    bool gongEnabled = true; // when disabled the ring shows "on + red" (highest priority)
    
    void updateTouchState(bool touched);
    bool isRingTouched();
    void loadFingerListFromPrefs();
    void disconnect();
    bool enrollAborted();
    uint8_t writeNotepad(uint8_t pageNumber, const char *text, uint8_t length);
    uint8_t readNotepad(uint8_t pageNumber, char *text, uint8_t length);
    


  public:
    bool connected;
    volatile bool enrollAbort = false; // set from the web server task to cancel a running enrollment
    bool connect();
    Match scanFingerprint();
    NewFinger enrollFinger(int id, int numIterations, String name);
    void deleteFinger(int id);
    void renameFinger(int id, String newName);
    String getFingerListAsHtmlOptionList();
    String getFingerListAsJson();
    int getFingerCount();
    bool getIgnoreTouchRing();
    void setIgnoreTouchRing(bool state);
    bool isFingerOnSensor();
    void setLedRingError();
    void setLedRingWifiConfig();
    void setLedRingReady();
    bool setGongEnabled(bool enabled); // returns true if the state changed
    String getPairingCode();
    bool setPairingCode(String pairingCode);
    void configLedRing(const LedStyle& ready, const LedStyle& scan, const LedStyle& match, const LedStyle& ignore, const LedStyle& gongOff);
    
    static String limitName(const String& name);               // names are limited to 48 bytes
    static void storageStats(uint32_t& used, uint32_t& total);
    String storageDetailsJson();                               // entries per category for the storage chart // entries of the settings flash area
    bool isFingerActive(int id); // switched on and (if scheduled) inside its time window
    FingerPolicy getFingerPolicy(int id);
    bool setFingerPolicy(int id, const FingerPolicy& policy);
    bool deleteAll();

    
    // functions for sensor replacement
    void exportSensorDB();
    void importSensorDB();

};

#endif
#include "FingerprintManager.h"
#include <nvs.h>
#include "global.h"

#include <Adafruit_Fingerprint.h>

bool FingerprintManager::connect() {
  
    // initialize input pins
    pinMode(touchRingPin, INPUT_PULLDOWN);

    Serial.println("\n\nAdafruit finger detect test");

    // set the data rate for the sensor serial port
    finger.begin(57600);
    delay(50);
    if (finger.verifyPassword()) {
        Serial.println("Found fingerprint sensor!");
    } else {
        delay(5000); // wait a bit longer for sensor to start before 2nd try (usually after a OTA-Update the esp32 is faster with startup than the fingerprint sensor)
        if (finger.verifyPassword()) { 
          Serial.println("Found fingerprint sensor!");
        } else {
          Serial.println("Did not find fingerprint sensor :(");
          connected = false;
          return connected;
        }
    }
    finger.LEDcontrol(FINGERPRINT_LED_FLASHING, 25, FINGERPRINT_LED_BLUE, 0); // sensor connected signal

    Serial.println(F("Reading sensor parameters"));
    finger.getParameters();
    Serial.print(F("Status: 0x")); Serial.println(finger.status_reg, HEX);
    Serial.print(F("Sys ID: 0x")); Serial.println(finger.system_id, HEX);
    Serial.print(F("Capacity: ")); Serial.println(finger.capacity);
    Serial.print(F("Security level: ")); Serial.println(finger.security_level);
    Serial.print(F("Device address: ")); Serial.println(finger.device_addr, HEX);
    Serial.print(F("Packet len: ")); Serial.println(finger.packet_len);
    Serial.print(F("Baud rate: ")); Serial.println(finger.baud_rate);

    finger.getTemplateCount();
    Serial.print("Sensor contains "); Serial.print(finger.templateCount); Serial.println(" templates");

    loadFingerListFromPrefs();

    connected = true;
    return connected;

    //updateTouchState(false);
}

void FingerprintManager::updateTouchState(bool touched)
{
  if ((touched != lastTouchState) || (ignoreTouchRing != lastIgnoreTouchRing)) {
      // check if sensor or ring is touched
      if (touched) {
        // turn touch indicator on:
        applyLed(ledScan);
      } else {
        // turn touch indicator off:
        setLedRingReady();
     }
  }
  lastTouchState = touched;
  lastIgnoreTouchRing = ignoreTouchRing;
  
}


Match FingerprintManager::scanFingerprint() {
  
  Match match;
  match.scanResult = ScanResult::error;

  if (!connected) {
      return match;
  }


  // finger detection by capacitive touchRing state (increased sensitivy but error prone due to rain)
  bool ringTouched = false;
  if (!ignoreTouchRing)
  {
    if (isRingTouched())
      ringTouched = true;
    if (ringTouched || lastTouchState) { 
        updateTouchState(true);
        //Serial.println("touched");
    } else {
        updateTouchState(false);
        match.scanResult = ScanResult::noFinger;
        return match;
    }

  }
  

  bool doAnotherScan = true;
  int scanPass = 0;
  while (doAnotherScan)
  {
    doAnotherScan = false;
    scanPass++;

    ///////////////////////////////////////////////////////////
    // STEP 1: Get Image from Sensor
    ///////////////////////////////////////////////////////////
    bool doImaging = true;
    int imagingPass = 0;
    while (doImaging)
    {
      doImaging = false;
      imagingPass++;
      //Serial.println(String("Get Image try ") + imagingPass);
      match.returnCode = finger.getImage();
      switch (match.returnCode) {
        case FINGERPRINT_OK:
          // Important: do net set touch state to true yet! Reason:
          // - if touchRing is NOT ignored, updateTouchState(true) was already called a few lines up, ring is already flashing red
          // - if touchRing IS ignored, wait for next step because image still can be "too messy" (=raindrop on sensor), and we don't want to flash red in this case
          //updateTouchState(true);
          //Serial.println("Image taken");
          break;
        case FINGERPRINT_NOFINGER:
        case FINGERPRINT_PACKETRECIEVEERR: // occurs from time to time, handle it like a "nofinger detected but touched" situation
          if (ringTouched) {
            // no finger on sensor but ring was touched -> ring event
            //Serial.println("ring touched");
            updateTouchState(true);
            if (imagingPass < 15) // up to x image passes in a row are taken after touch ring was touched until noFinger will raise a noMatchFound event
            {
              doImaging = true; // scan another image
              //delay(50);
              break;
            } else {
              //Serial.println("15 times no image after touching ring");
              match.scanResult = ScanResult::noMatchFound;
              return match;
            }
          } else  {
            if (ignoreTouchRing && scanPass > 1) {
              // the scan(s) in last iteration(s) have not found any match, now the finger was released (=no finger) -> return "no match" as result
              match.scanResult = ScanResult::noMatchFound;
            } else {
              match.scanResult = ScanResult::noFinger;
              updateTouchState(false);
            }
            return match;
          }
        case FINGERPRINT_IMAGEFAIL:
          Serial.println("Imaging error");
          updateTouchState(true);
          return match;
        default:
          Serial.println("Unknown error");
          return match;
      }
    
    }

    ///////////////////////////////////////////////////////////
    // STEP 2: Convert Image to feature map
    ///////////////////////////////////////////////////////////
    match.returnCode = finger.image2Tz();
    switch (match.returnCode) {
      case FINGERPRINT_OK:
        //Serial.println("Image converted");
        updateTouchState(true);
        break;
      case FINGERPRINT_IMAGEMESS:
        Serial.println("Image too messy");
        return match;
      case FINGERPRINT_PACKETRECIEVEERR:
        Serial.println("Communication error");
        return match;
      case FINGERPRINT_FEATUREFAIL:
        Serial.println("Could not find fingerprint features");
        return match;
      case FINGERPRINT_INVALIDIMAGE:
        Serial.println("Could not find fingerprint features");
        return match;
      default:
        Serial.println("Unknown error");
        return match;
    }

    ///////////////////////////////////////////////////////////
    // STEP 3: Search DB for matching features
    ///////////////////////////////////////////////////////////
    match.returnCode = finger.fingerSearch();
        if (match.returnCode == FINGERPRINT_OK && !isFingerActive(finger.fingerID)) {
        // known finger, but switched off or outside its time window: behaves like an unknown finger
        match.disabledId = finger.fingerID;
        match.disabledName = fingerList[finger.fingerID];
        match.disabledReason = policies[finger.fingerID].scheduled ? "outside its time window" : "switched off";
        match.matchConfidence = finger.confidence;
        notifyClients(String("Disabled fingerprint detected: #") + finger.fingerID + " - " + match.disabledName + " (" + match.disabledReason +
                      ", confidence " + finger.confidence + "). Treated as unknown finger.");
        match.scanResult = ScanResult::noMatchFound;

    } else if (match.returnCode == FINGERPRINT_OK) {
        // found a match!
        applyLed(ledMatch);

        match.scanResult = ScanResult::matchFound;
        match.matchId = finger.fingerID;
        match.matchConfidence = finger.confidence;
        match.matchName = fingerList[finger.fingerID];
      
    } else if (match.returnCode == FINGERPRINT_PACKETRECIEVEERR) {
        Serial.println("Communication error");

    } else if (match.returnCode == FINGERPRINT_NOTFOUND) {
        Serial.println(String("Did not find a match. (Scan #") + scanPass + String(" of 5)"));
        match.scanResult = ScanResult::noMatchFound;
        if (scanPass < 5) // max 5 Scans until no match found is given back as result
          doAnotherScan = true;

    } else {
        Serial.println("Unknown error");
    }

  } //while

  return match;

}



static FingerPolicy policyFromString(const String& str) {
  FingerPolicy p;
  if (str.startsWith("W")) {
    int pos = 1, field = 0;
    while (pos <= (int)str.length()) {
      int end = str.indexOf(';', pos);
      if (end < 0) end = str.length();
      String token = str.substring(pos, end);
      if (field == 0) p.enabled = token.toInt() != 0;
      else if (field == 1) p.scheduled = token.toInt() != 0;
      else if (p.windowCount < MAX_FINGER_WINDOWS) {
        int d = 0, f = 0, t = 0, a = 0;
        sscanf(token.c_str(), "%d,%d,%d,%d", &d, &f, &t, &a);
        FingerWindow& w = p.windows[p.windowCount++];
        w.days = d & 0x7F;
        w.from = constrain(f, 0, 1439);
        w.to = constrain(t, 0, 1439);
        w.allDay = a != 0;
      }
      field++;
      pos = end + 1;
    }
  } else {
    int e = 1, sc = 0, d = 0, f = 0, t = 0, a = 0;
    sscanf(str.c_str(), "%d;%d;%d;%d;%d;%d", &e, &sc, &d, &f, &t, &a);
    p.enabled = e != 0;
    p.scheduled = sc != 0;
    if (sc != 0 || d != 0) {
      p.windowCount = 1;
      p.windows[0].days = d & 0x7F;
      p.windows[0].from = constrain(f, 0, 1439);
      p.windows[0].to = constrain(t, 0, 1439);
      p.windows[0].allDay = a != 0;
    }
  }
  return p;
}

// ---------- storage ----------
// Names and schedules of all fingerprints live in 8 compact blobs (25 fingerprints each) in the settings flash area.
// One key per fingerprint would need 3-7 of the ~500 available entries each, which does not fit 200 fingerprints.
static const int GROUP_SIZE = 25;
static const int GROUP_COUNT = 8;
static const size_t MAX_NAME_LEN = 48;
static const size_t MAX_RECORD_LEN = 4 + MAX_NAME_LEN + 2 + 4 * MAX_FINGER_WINDOWS;

String FingerprintManager::limitName(const String& name) {
  String n = name;
  n.trim();
  if (n.length() <= MAX_NAME_LEN) return n;
  int cut = MAX_NAME_LEN;
  while (cut > 0 && (n[cut] & 0xC0) == 0x80) cut--; // do not cut inside a UTF-8 character
  return n.substring(0, cut);
}

void FingerprintManager::storageStats(uint32_t& used, uint32_t& total) {
  nvs_stats_t stats;
  if (nvs_get_stats(NULL, &stats) == ESP_OK) {
    used = stats.used_entries;
    total = stats.total_entries;
  } else {
    used = total = 0;
  }
}

// record: id, nameLen, name, flags (bit0 enabled, bit1 scheduled), windowCount, windows (3 bytes + days/allDay byte each)
bool FingerprintManager::persistGroup(int id) {
  if (id < 1 || id > 200) return false;
  int group = (id - 1) / GROUP_SIZE;
  int first = group * GROUP_SIZE + 1;
  uint8_t* buffer = (uint8_t*)malloc(GROUP_SIZE * MAX_RECORD_LEN);
  if (!buffer) return false;
  size_t len = 0;
  for (int i = first; i < first + GROUP_SIZE && i <= 200; i++) {
    if (fingerList[i].compareTo("@empty") == 0) continue;
    const FingerPolicy& p = policies[i];
    size_t nameLen = min(fingerList[i].length(), (unsigned int)MAX_NAME_LEN);
    buffer[len++] = i;
    buffer[len++] = nameLen;
    memcpy(buffer + len, fingerList[i].c_str(), nameLen);
    len += nameLen;
    buffer[len++] = (p.enabled ? 1 : 0) | (p.scheduled ? 2 : 0);
    buffer[len++] = p.windowCount;
    for (int w = 0; w < p.windowCount; w++) {
      uint32_t times = (p.windows[w].from & 0x7FF) | ((uint32_t)(p.windows[w].to & 0x7FF) << 11);
      buffer[len++] = (p.windows[w].days & 0x7F) | (p.windows[w].allDay ? 0x80 : 0);
      buffer[len++] = times & 0xFF;
      buffer[len++] = (times >> 8) & 0xFF;
      buffer[len++] = (times >> 16) & 0xFF;
    }
  }
  Preferences preferences;
  bool ok = preferences.begin("fingers", false);
  String key = String("g") + group;
  if (ok) {
    if (len == 0)
      preferences.remove(key.c_str());
    else
      ok = preferences.putBytes(key.c_str(), buffer, len) == len;
    if (ok && !preferences.isKey("ver"))
      preferences.putUChar("ver", 1);
    preferences.end();
  }
  free(buffer);
  if (!ok) notifyClients("Storage full: the fingerprint settings could not be saved. Delete fingerprints or names you do not need.");
  return ok;
}

void FingerprintManager::migrateLegacyStorage() {
  Preferences oldNames, oldPolicies;
  if (oldNames.begin("fingerList", true)) {
    for (int i = 1; i <= 200; i++) {
      String key = String(i);
      if (oldNames.isKey(key.c_str()))
        fingerList[i] = limitName(oldNames.getString(key.c_str(), String("@empty")));
    }
    oldNames.end();
  }
  if (oldPolicies.begin("fingerPolicy", true)) {
    for (int i = 1; i <= 200; i++) {
      String key = String(i);
      if (oldPolicies.isKey(key.c_str()))
        policies[i] = policyFromString(oldPolicies.getString(key.c_str(), ""));
    }
    oldPolicies.end();
  }
  bool ok = true;
  for (int g = 0; g < GROUP_COUNT; g++)
    ok = persistGroup(g * GROUP_SIZE + 1) && ok;
  Preferences marker;
  if (marker.begin("fingers", false)) {
    if (ok) marker.putUChar("ver", 1);
    marker.end();
  }
  if (ok) { // everything is in the new format, free the old entries
    Preferences clearer;
    if (clearer.begin("fingerList", false)) { clearer.clear(); clearer.end(); }
    if (clearer.begin("fingerPolicy", false)) { clearer.clear(); clearer.end(); }
    Serial.println("Fingerprint storage converted to the compact format.");
  } else {
    notifyClients("Warning: converting the fingerprint storage failed, the old data was kept.");
  }
}

void FingerprintManager::loadFingerListFromPrefs() {
  for (int i = 1; i <= 200; i++) {
    fingerList[i] = String("@empty");
    policies[i] = FingerPolicy();
  }
  Preferences preferences;
  bool hasNew = preferences.begin("fingers", false) && preferences.isKey("ver");
  if (hasNew) {
    for (int g = 0; g < GROUP_COUNT; g++) {
      String key = String("g") + g;
      size_t len = preferences.getBytesLength(key.c_str());
      if (len == 0 || len > GROUP_SIZE * MAX_RECORD_LEN) continue;
      uint8_t* buffer = (uint8_t*)malloc(len);
      if (!buffer) continue;
      preferences.getBytes(key.c_str(), buffer, len);
      size_t pos = 0;
      while (pos + 4 <= len) {
        int id = buffer[pos++];
        size_t nameLen = buffer[pos++];
        if (id < 1 || id > 200 || pos + nameLen + 2 > len) break;
        char name[MAX_NAME_LEN + 1];
        memcpy(name, buffer + pos, min(nameLen, MAX_NAME_LEN));
        name[min(nameLen, MAX_NAME_LEN)] = 0;
        pos += nameLen;
        FingerPolicy p;
        p.enabled = buffer[pos] & 1;
        p.scheduled = (buffer[pos] & 2) != 0;
        pos++;
        int count = min((int)buffer[pos++], MAX_FINGER_WINDOWS);
        if (pos + count * 4 > len) break;
        p.windowCount = count;
        for (int w = 0; w < count; w++) {
          p.windows[w].days = buffer[pos] & 0x7F;
          p.windows[w].allDay = (buffer[pos] & 0x80) != 0;
          uint32_t times = buffer[pos + 1] | ((uint32_t)buffer[pos + 2] << 8) | ((uint32_t)buffer[pos + 3] << 16);
          p.windows[w].from = min((uint32_t)1439, times & 0x7FF);
          p.windows[w].to = min((uint32_t)1439, (times >> 11) & 0x7FF);
          pos += 4;
        }
        fingerList[id] = String(name);
        policies[id] = p;
      }
      free(buffer);
    }
    preferences.end();
  } else {
    preferences.end();
    migrateLegacyStorage();
  }

  int counter = 0;
  for (int i = 1; i <= 200; i++)
    if (fingerList[i].compareTo("@empty") != 0) counter++;
  Serial.println(String(counter) + " fingers loaded from preferences.");
  if (counter != finger.templateCount)
    notifyClients(String("Warning: Fingerprint count mismatch! ") + finger.templateCount + " fingerprints stored on sensor, but we are aware of " + counter + " fingerprints.");
}


extern bool shouldReboot;
static const unsigned long ENROLL_TIMEOUT_MS = 120000; // give up if no finger is placed for 2 minutes

// a pending reboot also ends an enrollment, otherwise loop() would never get to restart the device
bool FingerprintManager::enrollAborted() {
  return enrollAbort || shouldReboot;
}

// Add/Enroll fingerprint
NewFinger FingerprintManager::enrollFinger(int id, int numIterations, String name) {

  NewFinger newFinger;
  newFinger.enrollResult = EnrollResult::error;

  lastTouchState = true; // after enrollment, scan mode kicks in again. Force update of the ring light back to normal on first iteration of scan mode.

  
  notifyClients(String("Enrollment for id #") + id + " started. We need to scan your finger 5 times until enrollment is completed.");

  
  // Repeat n times to get better resulting templates (as stated in R503 documentation up to 6 combined image samples possible, but I got an communication error when trying more than 5 samples, so dont go >5)
  for (int nTimes=1; nTimes<=numIterations; nTimes++)
  {
      notifyClients(String("Take #" + String(nTimes))+ " (place your finger on the sensor until led ring stops flashing, then remove it).");

      if (nTimes != 1) // not on first run
      {
        //delay(2000);
        newFinger.returnCode = 0xFF;
        unsigned long waitStart = millis();
        while (newFinger.returnCode != FINGERPRINT_NOFINGER) {
          if (enrollAborted() || millis() - waitStart > ENROLL_TIMEOUT_MS) return newFinger;
          newFinger.returnCode = finger.getImage();
          delay(10);
        }
      }

      Serial.print("Taking image sample "); Serial.print(nTimes); Serial.print(": ");
      finger.LEDcontrol(FINGERPRINT_LED_FLASHING, 25, FINGERPRINT_LED_PURPLE, 0);
      newFinger.returnCode = 0xFF;
      unsigned long sampleStart = millis();
      while (newFinger.returnCode != FINGERPRINT_OK) {
        if (enrollAborted() || millis() - sampleStart > ENROLL_TIMEOUT_MS) return newFinger;
        newFinger.returnCode = finger.getImage();
        delay(10);
        switch (newFinger.returnCode) {
        case FINGERPRINT_OK:
          Serial.print("taken, ");
          break;
        case FINGERPRINT_NOFINGER:
          break;
        case FINGERPRINT_PACKETRECIEVEERR:
          Serial.print("Communication error, ");
          break;
        case FINGERPRINT_IMAGEFAIL:
          Serial.print("Imaging error, ");
          break;
        default:
          Serial.print("Unknown error, ");
          break;
        }
      }
    
      // OK success!
    
      newFinger.returnCode = finger.image2Tz(nTimes);
      switch (newFinger.returnCode) {
        case FINGERPRINT_OK:
          Serial.print("converted");
          break;
        case FINGERPRINT_IMAGEMESS:
          Serial.print("too messy");
          return newFinger;
        case FINGERPRINT_PACKETRECIEVEERR:
          Serial.print("Communication error");
          return newFinger;
        case FINGERPRINT_FEATUREFAIL:
          Serial.print("Could not find fingerprint features");
          return newFinger;
        case FINGERPRINT_INVALIDIMAGE:
          Serial.print("Could not find fingerprint features");
          return newFinger;
        default:
          Serial.print("Unknown error");
          return newFinger;
      }
      finger.LEDcontrol(FINGERPRINT_LED_ON, 0, FINGERPRINT_LED_GREEN);

  }

  

  // OK converted!
  Serial.println();
  Serial.print("Creating model for #");  Serial.println(id);

  newFinger.returnCode = finger.createModel();
  if (newFinger.returnCode == FINGERPRINT_OK) {
    Serial.println("Prints matched!");
  } else if (newFinger.returnCode == FINGERPRINT_PACKETRECIEVEERR) {
    Serial.println("Communication error");
    return newFinger;
  } else if (newFinger.returnCode == FINGERPRINT_ENROLLMISMATCH) {
    Serial.println("Fingerprints did not match");
    return newFinger;
  } else {
    Serial.println("Unknown error");
    return newFinger;
  }

  Serial.print("ID "); Serial.println(id);
  newFinger.returnCode = finger.storeModel(id);
  if (newFinger.returnCode == FINGERPRINT_OK) {
    Serial.println("Stored!");
    newFinger.enrollResult = EnrollResult::ok;
    // save to prefs
        fingerList[id] = limitName(name);
    policies[id] = FingerPolicy(); // the slot may have been used before
    persistGroup(id);


  } else if (newFinger.returnCode == FINGERPRINT_PACKETRECIEVEERR) {
    Serial.println("Communication error");
    return newFinger;
  } else if (newFinger.returnCode == FINGERPRINT_BADLOCATION) {
    Serial.println("Could not store in that location");
    return newFinger;
  } else if (newFinger.returnCode == FINGERPRINT_FLASHERR) {
    Serial.println("Error writing to flash");
    return newFinger;
  } else {
    Serial.println("Unknown error");
    return newFinger;
  }

  //finger.LEDcontrol(FINGERPRINT_LED_OFF, 0, FINGERPRINT_LED_RED);

  return newFinger;

}


void FingerprintManager::deleteFinger(int id) {
          
  if ((id > 0) && (id <= 200)) {
    int8_t result = finger.deleteModel(id);
    if (result != FINGERPRINT_OK) {
      notifyClients(String("Delete of finger template #") + id + " from sensor failed with code " + result);
      return;

    } else {
            fingerList[id] = "@empty";
      policies[id] = FingerPolicy();
      persistGroup(id);
      Serial.println(String("Finger template #") + id + " deleted from sensor and prefs.");

    }
  }

}


void FingerprintManager::renameFinger(int id, String newName) {
  if ((id > 0) && (id <= 200)) {
    String oldName = fingerList[id];
    fingerList[id] = limitName(newName);
    if (persistGroup(id))
      Serial.println(String("Finger template #") + id + " renamed from " + oldName + " to " + fingerList[id]);
    else
      fingerList[id] = oldName; // not saved, keep the old name
  }
}

String FingerprintManager::getFingerListAsHtmlOptionList() {
  String htmlOptions = "";
  int counter = 0;
  for (int i=1; i<=200; i++) {
    if (fingerList[i].compareTo("@empty") != 0) {
      String option;
      if (counter == 0)
        option = "<option value=\"" + String(i) + "\" selected>" + String(i) + " - " + fingerList[i] + "</option>";
      else 
        option = "<option value=\"" + String(i) + "\">" + String(i) + " - " + fingerList[i] + "</option>";
      htmlOptions += option;
      counter++;
    }
  }
  return htmlOptions;
}
static void appendWindows(String& json, const FingerPolicy& p) {
  json += "[";
  for (int w = 0; w < p.windowCount; w++) {
    if (w) json += ",";
    json += "[" + String(p.windows[w].days) + "," + String(p.windows[w].from) + "," + String(p.windows[w].to) + "," + (p.windows[w].allDay ? "1" : "0") + "]";
  }
  json += "]";
}

String FingerprintManager::getFingerListAsJson() {
  if (!connected) // without sensor the finger list was never loaded
    return "[]";
  String json;
  json.reserve(80 + getFingerCount() * 200); // worst case about 40 KB, one allocation instead of repeated growing
  json = "[";
  bool first = true;
  for (int i=1; i<=200; i++) {
    if (fingerList[i].compareTo("@empty") != 0) {
      String name = fingerList[i];
      name.replace("\\", "\\\\");
      name.replace("\"", "\\\"");
      if (!first)
        json += ",";
            const FingerPolicy& p = policies[i];
      json += "{\"id\":" + String(i) + ",\"name\":\"" + name + "\""
              ",\"enabled\":" + (p.enabled ? "true" : "false") +
              ",\"scheduled\":" + (p.scheduled ? "true" : "false") +
              ",\"active\":" + (isFingerActive(i) ? "true" : "false") + ",\"windows\":";
      appendWindows(json, p);
      json += "}";
      first = false;
    }
  }
  return json + "]";
}

int FingerprintManager::getFingerCount() {
  if (!connected)
    return 0;
  int counter = 0;
  for (int i=1; i<=200; i++) {
    if (fingerList[i].compareTo("@empty") != 0)
      counter++;
  }
  return counter;
}

bool FingerprintManager::getIgnoreTouchRing() {
  return ignoreTouchRing;
}

void FingerprintManager::setIgnoreTouchRing(bool state) {
  if (ignoreTouchRing != state) {
    ignoreTouchRing = state;
    if (state == true)
      notifyClients("IgnoreTouchRing is now 'on'");
    else
      notifyClients("IgnoreTouchRing is now 'off'");
  }
}


bool FingerprintManager::isRingTouched() {
  if (digitalRead(touchRingPin) == LOW) // LOW = touched. Caution: touchSignal on this pin occour only once (at beginning of touching the ring, not every iteration if you keep your finger on the ring)
      return true;
  else 
      return false;
}

bool FingerprintManager::isFingerOnSensor() {
  // get an image
  uint8_t returnCode = finger.getImage();
  if (returnCode == FINGERPRINT_OK) {
    // try to find fingerprint features in image, because image taken does not already means finger on sensor, could also be a raindrop
    returnCode = finger.image2Tz();
    if (returnCode == FINGERPRINT_OK)
      return true;
  }
  return false;
}
  
void FingerprintManager::setLedRingError() {
  finger.LEDcontrol(FINGERPRINT_LED_ON, 0, FINGERPRINT_LED_RED);
}

void FingerprintManager::setLedRingWifiConfig() {
  finger.LEDcontrol(FINGERPRINT_LED_BREATHING, 250, FINGERPRINT_LED_RED);
}

void FingerprintManager::applyLed(const LedStyle& style) {
  finger.LEDcontrol(style.sequence, style.speed, style.color);
}

// priority: gong disabled > ignore touch ring > ready (each with its own configurable color and sequence)
void FingerprintManager::setLedRingReady() {
  if (!gongEnabled)
    applyLed(ledGongOff);
  else if (ignoreTouchRing)
    applyLed(ledIgnore);
  else
    applyLed(ledReady);
}

bool FingerprintManager::setGongEnabled(bool enabled) {
  if (gongEnabled == enabled)
    return false;
  gongEnabled = enabled;
  return true;
}

bool FingerprintManager::deleteAll() {
  if (finger.emptyDatabase() == FINGERPRINT_OK)
  {
    bool rc = true;
    Preferences preferences;
    if (preferences.begin("fingers", false)) {
      rc = preferences.clear();
      preferences.end();
    }
    for (int i=1; i<=200; i++) {
      fingerList[i] = String("@empty");
      policies[i] = FingerPolicy();
    }
    return rc;
  }
  else
    return false;
}



uint8_t FingerprintManager::writeNotepad(uint8_t pageNumber, const char *text, uint8_t length) {
  uint8_t data[34];
  
  if (length>32)
    length = 32;

  data[0] = FINGERPRINT_WRITENOTEPAD;
  data[1] = pageNumber;
  for (int i=0; i<length; i++)
    data[i+2] = text[i];

  Adafruit_Fingerprint_Packet packet(FINGERPRINT_COMMANDPACKET, sizeof(data), data);                                    
  finger.writeStructuredPacket(packet);
  if (finger.getStructuredPacket(&packet) != FINGERPRINT_OK)
    return FINGERPRINT_PACKETRECIEVEERR;
  if (packet.type != FINGERPRINT_ACKPACKET)
    return FINGERPRINT_PACKETRECIEVEERR;
  return packet.data[0];
}


uint8_t FingerprintManager::readNotepad(uint8_t pageNumber, char *text, uint8_t length) {
  uint8_t data[2];

  data[0] = FINGERPRINT_READNOTEPAD;
  data[1] = pageNumber;

  Adafruit_Fingerprint_Packet packet(FINGERPRINT_COMMANDPACKET, sizeof(data), data);                                    
  finger.writeStructuredPacket(packet);
  if (finger.getStructuredPacket(&packet) != FINGERPRINT_OK)
    return FINGERPRINT_PACKETRECIEVEERR;
  if (packet.type != FINGERPRINT_ACKPACKET)
    return FINGERPRINT_PACKETRECIEVEERR;
  
  if (packet.data[0] == FINGERPRINT_OK) {
    // read data payload
    for (uint8_t i=0; i<length; i++) {
      text[i] = packet.data[i+1];
    }
  }

  return packet.data[0];

}


String FingerprintManager::getPairingCode() {
  char buffer[33];
  buffer[32] = 0; // null termination needed for convertion to string at the end
  if (readNotepad(0, (char*)buffer, 32) == FINGERPRINT_OK)
    return String((char*)buffer);
  else
    return "";
}


bool FingerprintManager::setPairingCode(String pairingCode) {
  if (writeNotepad(0, pairingCode.c_str(), 32) == FINGERPRINT_OK)
    return true;
  else
    return false;
}


// ToDo: support sensor replacement by enable transferring of sensor DB to another sensor
void FingerprintManager::exportSensorDB() {

}
    
void FingerprintManager::importSensorDB() {

}

void FingerprintManager::configLedRing(const LedStyle& ready, const LedStyle& scan, const LedStyle& match, const LedStyle& ignore, const LedStyle& gongOff) {
  ledReady = ready;
  ledScan = scan;
  ledMatch = match;
  ledIgnore = ignore;
  ledGongOff = gongOff;
}



// ---------- per fingerprint enable / schedule ----------

// legacy format of older versions (one key per fingerprint), only read during the conversion

FingerPolicy FingerprintManager::getFingerPolicy(int id) {
  if (id < 1 || id > 200)
    return FingerPolicy();
  return policies[id];
}

bool FingerprintManager::setFingerPolicy(int id, const FingerPolicy& policy) {
  if (id < 1 || id > 200)
    return false;
  FingerPolicy previous = policies[id];
  policies[id] = policy;
  if (policies[id].windowCount > MAX_FINGER_WINDOWS)
    policies[id].windowCount = MAX_FINGER_WINDOWS;
  if (persistGroup(id))
    return true;
  policies[id] = previous; // not saved
  return false;
}

static bool windowMatches(const FingerWindow& w, int day, int prevDay, int now) {
  bool today = (w.days >> day) & 1;
  if (w.allDay)
    return today;
  if (w.from == w.to)
    return false; // empty window
  if (w.from < w.to)
    return today && now >= w.from && now < w.to;
  // window wraps over midnight and belongs to the day it starts on
  return (today && now >= w.from) || (((w.days >> prevDay) & 1) && now < w.to);
}

// without a valid time (NTP not reachable) a scheduled finger stays inactive
bool FingerprintManager::isInSchedule(const FingerPolicy& policy) {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 0))
    return false;
  int day = (timeinfo.tm_wday + 6) % 7; // 0 = Monday
  int prevDay = (day + 6) % 7;
  int now = timeinfo.tm_hour * 60 + timeinfo.tm_min;
  for (int w = 0; w < policy.windowCount; w++)
    if (windowMatches(policy.windows[w], day, prevDay, now))
      return true;
  return false;
}

bool FingerprintManager::isFingerActive(int id) {
  if (id < 1 || id > 200)
    return false;
  const FingerPolicy& policy = policies[id];
  // with a schedule the schedule alone decides, the manual switch only applies without one
  return policy.scheduled ? isInSchedule(policy) : policy.enabled;
}


// Reads the raw settings partition and counts the used entries per owner: fingerprints, settings, everything else.
// One page (126 entries) always stays free, the flash system needs it to reorganize.
String FingerprintManager::storageDetailsJson() {
  uint32_t used, total;
  storageStats(used, total);
  int perNs[256];
  memset(perNs, 0, sizeof(perNs));
  char nsNames[256][16];
  memset(nsNames, 0, sizeof(nsNames));
  int overhead = 0;
  const esp_partition_t* part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, NULL);
  uint8_t* page = part ? (uint8_t*)malloc(4096) : NULL;
  if (page) {
    for (uint32_t off = 0; off + 4096 <= part->size; off += 4096) {
      if (esp_partition_read(part, off, page, 4096) != ESP_OK) continue;
      if (page[0] == 0xFF && page[1] == 0xFF && page[2] == 0xFF && page[3] == 0xFF) continue; // empty page
      for (int e = 0; e < 126;) {
        int state = (page[32 + e / 4] >> ((e % 4) * 2)) & 3;
        if (state != 2) { e++; continue; }                       // only written entries
        const uint8_t* entry = page + 64 + e * 32;
        int ns = entry[0], span = entry[2];
        if (span < 1) span = 1;
        if (ns == 0) {                                           // namespace definition: key = name, data[0] = index
          memcpy(nsNames[entry[24]], entry + 8, 15);
          overhead += span;
        } else {
          perNs[ns] += span;
        }
        e += span;
      }
    }
    free(page);
  }
  int fingers = 0, settings = 0, other = overhead;
  for (int n = 1; n < 256; n++) {
    if (!perNs[n]) continue;
    String name = String(nsNames[n]);
    if (name == "fingers" || name == "fingerList" || name == "fingerPolicy") fingers += perNs[n];
    else if (name == "wifiSettings" || name == "appSettings" || name == "ringSettings") settings += perNs[n];
    else other += perNs[n];
  }
  return String("{\"total\":") + total + ",\"used\":" + used + ",\"reserved\":126,\"fingers\":" + fingers + ",\"settings\":" + settings +
         ",\"other\":" + other + ",\"fingerCount\":" + getFingerCount() + "}";
}

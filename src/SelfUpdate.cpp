#include "SelfUpdate.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>
#include <SPIFFS.h>
#include <time.h>
#include "GithubRoots.h"
#include <esp_heap_caps.h>
#include <esp_partition.h>

extern bool shouldReboot;
void notifyClients(const String& message);

static const char* RELEASE_BASE = "https://github.com/Grrzzz/FingerprintDoorbell/releases/download/";

// survives a crash/restart (RTC memory) so the last step of an interrupted update can be shown afterwards
RTC_NOINIT_ATTR static uint32_t rtcMagic;
RTC_NOINIT_ATTR static char rtcStep[64];
static void trace(const String& step) {
  rtcMagic = 0x55504454;
  snprintf(rtcStep, sizeof(rtcStep), "%s | heap %u block %u", step.c_str(), (unsigned)ESP.getFreeHeap(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

static volatile bool running = false;
static volatile int percent = 0;
static String state = "idle";
static String message = "";
static String tagToInstall = "";

static void setState(const char* s, const String& m) {
  state = s;
  message = m;
  trace(String("state ") + s);
}

static bool addressAllowed(const String& url) {
  if (!url.startsWith("https://")) return false;
  int hostEnd = url.indexOf('/', 8);
  if (hostEnd < 0) return false;
  String host = url.substring(8, hostEnd);
  return host == "github.com" || host.endsWith(".githubusercontent.com");
}

// opens the download (following the redirects to the GitHub CDN) and leaves the response ready to be read
static bool openDownload(String url, HTTPClient& http, WiFiClientSecure*& client, int& length, String& error) {
  for (int hop = 0; hop < 6; hop++) {
    if (!addressAllowed(url)) { error = "unexpected download address"; return false; }
    if (client) { http.end(); delete client; client = nullptr; }
    trace(String("connect ") + url.substring(8, 40));
    client = new WiFiClientSecure();
    client->setCACert(GITHUB_ROOT_CAS);
    client->setTimeout(15);
    http.setReuse(false);
    http.setTimeout(15000);
    if (!http.begin(*client, url)) { error = "could not connect to GitHub"; return false; }
    const char* headers[] = {"Location"};
    http.collectHeaders(headers, 1);
    int code = http.GET();
    if (code == 200) {
      length = http.getSize();
      return true;
    }
    if (code == 301 || code == 302 || code == 303 || code == 307 || code == 308) {
      url = http.header("Location");
      continue;
    }
    error = code > 0 ? String("GitHub answered HTTP ") + code : String("connection to GitHub failed (") + HTTPClient::errorToString(code) + ")";
    return false;
  }
  error = "too many redirects";
  return false;
}

static void closeDownload(HTTPClient& http, WiFiClientSecure*& client) {
  http.end();
  if (client) { delete client; client = nullptr; }
}

// Streaming reader for update.zip: spiffs.bin first, then firmware.bin, both STORED (not compressed).
// Every chunk goes straight into the update partition, nothing is buffered, so it needs no extra heap.
static int rawTarget = -1; // -1 zip, else U_FLASH / U_SPIFFS for a single image
static bool rawBegun = false;
static volatile bool uploadActive = false;
static volatile uint32_t uploadLastChunk = 0;

struct ZipInstaller {
  enum Phase { HEADER, NAME, EXTRA, DATA, DONE, FAILED };
  Phase phase = HEADER;
  uint8_t hdr[30];
  uint8_t have = 0;
  uint32_t need = 0;
  uint16_t nameLen = 0, extraLen = 0;
  uint32_t dataSize = 0;
  char name[16];
  uint8_t nameFill = 0;
  int stage = 0;        // 0 nothing, 1 filesystem written, 2 firmware written
  int current = -2;     // -2 none, -1 skip entry, else U_SPIFFS / U_FLASH
  uint32_t consumed = 0;
  uint32_t total = 0;
  String error;

  bool fail(const String& e) {
    error = e;
    phase = FAILED;
    if (Update.isRunning()) Update.abort();
    if (stage == 0 || current == U_SPIFFS) SPIFFS.begin();
    return false;
  }

  bool beginEntry() {
    if (strcmp(name, "spiffs.bin") == 0) {
      if (stage != 0) return fail("spiffs.bin must come first in update.zip");
      current = U_SPIFFS;
      setState("filesystem", "Installing the web interface");
      SPIFFS.end();
    } else if (strcmp(name, "firmware.bin") == 0) {
      if (stage != 1) return fail("update.zip needs spiffs.bin before firmware.bin");
      current = U_FLASH;
      setState("firmware", "Installing the firmware");
    } else {
      current = -1;
      return true;
    }
    if (dataSize == 0) return fail(String(name) + " is empty");
    if (!Update.begin(dataSize, current)) return fail(String("no space for ") + name + ": " + Update.errorString());
    return true;
  }

  bool finishEntry() {
    if (current == U_FLASH || current == U_SPIFFS) {
      if (!Update.end()) return fail(String("image rejected: ") + Update.errorString());
      stage++;
      if (current == U_SPIFFS) SPIFFS.begin();
    }
    current = -2;
    phase = HEADER;
    have = 0;
    return true;
  }

  bool feed(const uint8_t* d, size_t n) {
    consumed += n;
    if (total) percent = 2 + (int)((uint64_t)96 * consumed / total);
    while (n > 0 && phase != DONE) {
      if (phase == FAILED) return false;
      if (phase == HEADER) {
        size_t k = min((size_t)(30 - have), n);
        memcpy(hdr + have, d, k);
        have += k; d += k; n -= k;
        if (have < 30) break;
        uint32_t sig = hdr[0] | hdr[1] << 8 | hdr[2] << 16 | (uint32_t)hdr[3] << 24;
        if (sig == 0x02014b50 || sig == 0x06054b50) { phase = DONE; break; }
        if (sig != 0x04034b50) return fail("this is not a valid update.zip");
        if ((hdr[6] | hdr[7] << 8) & 8) return fail("update.zip entries must not use data descriptors");
        if ((hdr[8] | hdr[9] << 8) != 0) return fail("update.zip must be stored without compression");
        dataSize = hdr[18] | hdr[19] << 8 | hdr[20] << 16 | (uint32_t)hdr[21] << 24;
        nameLen = hdr[26] | hdr[27] << 8;
        extraLen = hdr[28] | hdr[29] << 8;
        nameFill = 0;
        need = nameLen;
        phase = NAME;
      }
      if (phase == NAME) {
        while (need > 0 && n > 0) {
          if (nameFill < sizeof(name) - 1) name[nameFill++] = (char)*d;
          d++; n--; need--;
        }
        if (need > 0) break;
        name[nameFill] = 0;
        if (nameLen >= sizeof(name)) name[0] = 0;
        need = extraLen;
        phase = EXTRA;
      }
      if (phase == EXTRA) {
        size_t k = min((size_t)need, n);
        d += k; n -= k; need -= k;
        if (need > 0) break;
        if (!beginEntry()) return false;
        need = dataSize;
        phase = DATA;
        if (need == 0 && !finishEntry()) return false;
      }
      if (phase == DATA && n > 0) {
        size_t k = min((size_t)need, n);
        if (current != -1 && Update.write((uint8_t*)d, k) != k) return fail(String("write failed: ") + Update.errorString());
        d += k; n -= k; need -= k;
        if (need == 0 && !finishEntry()) return false;
      }
    }
    return true;
  }

  bool complete() { return phase != FAILED && stage == 2; }
};

static ZipInstaller* zip = nullptr;

static void updateTask(void*) {
  String error;
  HTTPClient http;
  WiFiClientSecure* client = nullptr;
  int length = 0;
  String url = String(RELEASE_BASE) + tagToInstall + "/update.zip";
  bool ok = false;
  ZipInstaller* z = new ZipInstaller();

  do {
    if (!z) { error = "not enough memory"; break; }
    if (time(nullptr) < 1700000000) { error = "device time not set yet (NTP), try again in a minute"; break; }
    setState("checking", "Downloading update.zip");
    percent = 1;
    if (!openDownload(url, http, client, length, error)) { error = "update.zip: " + error; break; }
    if (length <= 0) { error = "update.zip: unknown file size"; break; }
    z->total = length;
    WiFiClient* stream = http.getStreamPtr();
    uint8_t buffer[1024];
    unsigned long lastData = millis();
    while (z->consumed < (uint32_t)length && z->phase != ZipInstaller::DONE) {
      int available = stream->available();
      if (available > 0) {
        int n = stream->readBytes(buffer, min(min(available, (int)sizeof(buffer)), length - (int)z->consumed));
        if (n <= 0) continue;
        lastData = millis();
        if (!z->feed(buffer, n)) break;
      } else {
        if (millis() - lastData > 20000 || !http.connected()) { error = "connection lost during download"; z->fail(error); break; }
        delay(5);
      }
    }
    if (z->phase == ZipInstaller::FAILED) { error = z->error; break; }
    if (!z->complete()) { error = "update.zip is incomplete"; break; }
    ok = true;
  } while (false);

  closeDownload(http, client);
  delete z;
  if (ok) {
    percent = 100;
    setState("restarting", "Update installed, restarting");
    delay(2500); // let the web interface pick up the state
    shouldReboot = true;
  } else {
    if (Update.isRunning()) Update.abort();
    notifyClients("Update failed: " + error);
    setState("error", error);
    running = false;
  }
  vTaskDelete(NULL);
}

// manual upload (POST /api/update/upload): update.zip, or a single image (firmware.bin / spiffs.bin)
// a plain .bin is recognized by its name ("spiff"/"filesystem" = web interface, "firmware" = firmware),
// any other name by the image header (firmware starts with 0xE9)
bool selfUpdateUploadChunk(const String& filename, size_t index, uint8_t* data, size_t len, bool final, size_t total, String& error) {
  if (index == 0) {
    if (running) { error = "an update is already running"; return false; }
    String lower = filename;
    lower.toLowerCase();
    bool isZip = lower.endsWith(".zip");
    if (!isZip && !lower.endsWith(".bin") && !lower.endsWith(".spiffs") && !lower.endsWith(".spiff")) { error = "choose a .zip, firmware.bin or spiffs.bin"; return false; }
    rawTarget = -1;
    if (!isZip) {
      if (lower.indexOf("spiff") >= 0 || lower.indexOf("filesystem") >= 0) rawTarget = U_SPIFFS;
      else if (lower.indexOf("firmware") >= 0) rawTarget = U_FLASH;
      else if (len > 0 && data[0] == 0xE9) rawTarget = U_FLASH;
      else { error = "cannot tell firmware from web interface, name the file firmware.bin or spiffs.bin"; return false; }
    }
    if (zip) { delete zip; zip = nullptr; }
    zip = new ZipInstaller();
    if (!zip) { error = "not enough memory"; return false; }
    zip->total = total;
    rawBegun = false;
    running = true;
    uploadActive = true;
    percent = 0;
    setState("upload", "Receiving the update");
  }
  uploadLastChunk = millis();
  if (!zip) { error = message.length() ? message : "no update in progress"; return false; }
  if (rawTarget != -1) {
    if (!rawBegun) {
      size_t size = UPDATE_SIZE_UNKNOWN;
      if (rawTarget == U_SPIFFS) {
        const esp_partition_t* p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, NULL);
        if (!p) { error = "no filesystem partition"; return false; }
        size = p->size;
        SPIFFS.end();
      }
      if (!Update.begin(size, rawTarget)) { error = String("no space for the update: ") + Update.errorString(); if (rawTarget == U_SPIFFS) SPIFFS.begin(); return false; }
      rawBegun = true;
      setState(rawTarget == U_SPIFFS ? "filesystem" : "firmware", rawTarget == U_SPIFFS ? "Installing the web interface" : "Installing the firmware");
    }
    zip->consumed += len;
    if (total) percent = (int)((uint64_t)98 * zip->consumed / total);
    if (Update.write(data, len) != len) { error = String("write failed: ") + Update.errorString(); Update.abort(); return false; }
    if (final) {
      if (!Update.end(true)) { error = String("image rejected: ") + Update.errorString(); return false; }
      if (rawTarget == U_SPIFFS) SPIFFS.begin();
      zip->stage = 2;
    }
  } else {
    if (!zip->feed(data, len)) { error = zip->error; return false; }
  }
  return true;
}

// called when the upload request ends; true if everything was installed (the caller reboots)
bool selfUpdateUploadFinish(String& error) {
  uploadActive = false;
  bool ok = zip && zip->complete() && !(rawTarget != -1 && !rawBegun);
  if (!ok) {
    if (error.length() == 0) error = (zip && zip->error.length()) ? zip->error : "the upload is not a complete update";
    if (Update.isRunning()) Update.abort();
    SPIFFS.begin();
    setState("error", error);
    running = false;
  } else {
    percent = 100;
    setState("restarting", "Update installed, restarting");
  }
  delete zip;
  zip = nullptr;
  return ok;
}
// a browser upload that stops delivering data (connection lost) must not block the next attempt until reboot
bool selfUpdateRunning() {
  if (running && uploadActive && millis() - uploadLastChunk > 30000) {
    uploadActive = false;
    if (Update.isRunning()) Update.abort();
    SPIFFS.begin();
    setState("error", "the upload stalled and was cancelled");
    running = false;
  }
  return running;
}

bool selfUpdateStart(const String& tag, String& error) {
  if (running) { error = "an update is already running"; return false; }
  // tags look like v2.1 or v2.0.1
  bool valid = tag.length() >= 2 && tag.length() <= 12 && tag[0] == 'v';
  for (unsigned i = 1; valid && i < tag.length(); i++)
    valid = isdigit(tag[i]) || tag[i] == '.';
  if (!valid) { error = "invalid release tag"; return false; }
  if (WiFi.status() != WL_CONNECTED) { error = "no Wi-Fi connection"; return false; }

  tagToInstall = tag;
  percent = 0;
  setState("checking", "Starting");
  running = true;
  notifyClients("Update to " + tag + " started");
  if (xTaskCreatePinnedToCore(updateTask, "selfupdate", 16384, NULL, 1, NULL, 1) != pdPASS) {
    running = false;
    setState("error", "not enough memory to start the update");
    error = message;
    return false;
  }
  return true;
}

String selfUpdateStatusJson() {
  String m = message;
  m.replace("\\", "\\\\");
  m.replace("\"", "\\\"");
  String json = String("{\"state\":\"") + state + "\",\"percent\":" + String((int)percent) + ",\"message\":\"" + m + "\"";
  if (state == "idle" && rtcMagic == 0x55504454) {
    rtcStep[sizeof(rtcStep) - 1] = 0;
    json += String(",\"lastStep\":\"") + rtcStep + "\"";
  }
  return json + "}";
}

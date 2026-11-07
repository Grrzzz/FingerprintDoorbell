#include "SelfUpdate.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>
#include <SPIFFS.h>
#include <time.h>
#include "GithubRoots.h"
#include <esp_heap_caps.h>

extern bool shouldReboot;
void notifyClients(String message);

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

// streams the open response into the update partition (command: U_FLASH or U_SPIFFS)
static bool installStream(HTTPClient& http, int length, int command, int percentFrom, int percentTo, String& error) {
  if (length <= 0) { error = "unknown file size"; return false; }
  trace(String("Update.begin ") + length);
  if (!Update.begin(length, command)) { error = String("no space for the update: ") + Update.errorString(); return false; }
  WiFiClient* stream = http.getStreamPtr();
  uint8_t buffer[1024];
  int written = 0;
  unsigned long lastData = millis();
  while (written < length) {
    int available = stream->available();
    if (available > 0) {
      int n = stream->readBytes(buffer, min(min(available, (int)sizeof(buffer)), length - written));
      if (n <= 0) continue;
      if ((int)Update.write(buffer, n) != n) { error = String("write failed: ") + Update.errorString(); Update.abort(); return false; }
      written += n;
      lastData = millis();
      percent = percentFrom + (int)((long)(percentTo - percentFrom) * written / length);
      if ((written & 0x3FFFF) < 1024) trace(String("written ") + written);
    } else {
      if (millis() - lastData > 20000 || !http.connected()) { error = "connection lost during download"; Update.abort(); return false; }
      delay(5);
    }
  }
  if (!Update.end()) { error = String("image rejected: ") + Update.errorString(); return false; }
  return true;
}

static void updateTask(void*) {
  String error;
  HTTPClient http;
  WiFiClientSecure* client = nullptr;
  int length = 0;
  String base = String(RELEASE_BASE) + tagToInstall + "/";
  bool ok = false;

  do {
    if (time(nullptr) < 1700000000) { error = "device time not set yet (NTP), try again in a minute"; break; }

    // check that both files can be downloaded before anything is overwritten
    setState("checking", "Checking the release files");
    percent = 2;
    if (!openDownload(base + "firmware.bin", http, client, length, error)) { error = "firmware.bin: " + error; break; }
    closeDownload(http, client);
    percent = 5;
    if (!openDownload(base + "spiffs.bin", http, client, length, error)) { error = "spiffs.bin: " + error; break; }
    trace("spiffs.bin opened");

    // filesystem first: a failure afterwards can simply be retried, the installed firmware stays the same
    setState("filesystem", "Installing the web interface");
    trace("SPIFFS.end");
    SPIFFS.end();
    bool fsOk = installStream(http, length, U_SPIFFS, 5, 50, error);
    closeDownload(http, client);
    if (!fsOk) { error = "web interface: " + error; SPIFFS.begin(); break; }

    setState("firmware", "Installing the firmware");
    if (!openDownload(base + "firmware.bin", http, client, length, error)) { error = "firmware.bin: " + error; break; }
    bool fwOk = installStream(http, length, U_FLASH, 50, 98, error);
    closeDownload(http, client);
    if (!fwOk) { error = "firmware: " + error; break; }
    ok = true;
  } while (false);

  closeDownload(http, client);
  if (ok) {
    percent = 100;
    setState("restarting", "Update installed, restarting");
    delay(2500); // let the web interface pick up the state
    shouldReboot = true;
  } else {
    notifyClients("Update failed: " + error);
    setState("error", error);
    running = false;
  }
  vTaskDelete(NULL);
}

bool selfUpdateRunning() {
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

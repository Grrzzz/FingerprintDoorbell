#ifndef SELFUPDATE_H
#define SELFUPDATE_H

#include <Arduino.h>

// One-click update: downloads update.zip (spiffs.bin + firmware.bin, stored) of a release from the GitHub fork and installs it.
bool selfUpdateStart(const String& tag, String& error);
bool selfUpdateRunning();
String selfUpdateStatusJson(); // {"state":"idle|filesystem|firmware|restarting|error","percent":n,"message":"..."}

// Manual upload of update.zip or firmware.bin: feed every chunk, then call finish once (true = installed, reboot)
bool selfUpdateUploadChunk(const String& filename, size_t index, uint8_t* data, size_t len, bool final, size_t total, String& error);
bool selfUpdateUploadFinish(String& error);

#endif

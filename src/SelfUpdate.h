#ifndef SELFUPDATE_H
#define SELFUPDATE_H

#include <Arduino.h>

// One-click update: downloads spiffs.bin and firmware.bin of a release from the GitHub fork and installs both.
bool selfUpdateStart(const String& tag, String& error);
bool selfUpdateRunning();
String selfUpdateStatusJson(); // {"state":"idle|filesystem|firmware|restarting|error","percent":n,"message":"..."}

#endif

// ESP32: lists the root directory of an ext4 formatted SD card using the
// SDMMC interface (4 bit mode)
#include "TinyEXT4.h"
#include "tinyext4/driver/Esp32SdmmcIO.h"

using namespace tinyext4;

Esp32SdmmcIO sd(false, SDMMC_FREQ_DEFAULT);  // 4-bit, 20MHz
Ext4FS fs;

void setup() {
  Serial.begin(115200);
  // more cache: the buffers are allocated in PSRAM if available
  fs.setCacheSectors(32);
  if (!fs.begin(sd, true)) {
    Serial.print("mount failed: ");
    Serial.println(fs.lastErrorText());
    return;
  }
  File root = fs.open("/");
  DirEntry e;
  while (root.readDir(e)) {
    Serial.print(e.isDirectory() ? "d " : "- ");
    Serial.println(e.name);
  }
  fs.end();
}

void loop() {}

// Appends measurements to a log file on an ext4 formatted SD card (SPI).
// After the first write the file system is marked as "not clean" until
// fs.end() is called: Linux will then recommend to run e2fsck.
#include <SPI.h>

#include "TinyEXT4.h"
#include "tinyext4/driver/ArduinoSpiIO.h"

using namespace tinyext4;

ArduinoSpiIO sd(SS);  // CS pin
Ext4FS fs;

void setup() {
  Serial.begin(115200);
  while (!Serial);
  SPI.begin();
  if (!fs.begin(sd)) {
    Serial.print("mount failed: ");
    Serial.println(fs.lastErrorText());
    return;
  }
  if (fs.isReadOnly()) {
    Serial.println("file system can only be mounted read only");
    return;
  }
  if (!fs.exists("/logs")) fs.mkdir("/logs");

  File log = fs.open("/logs/data.csv", "a");
  for (int j = 0; j < 100; j++) {
    log.print(millis());
    log.print(";");
    log.println(analogRead(A0));
  }
  log.close();  // writes the inode and syncs

  log = fs.open("/logs/data.csv");
  Serial.print("size: ");
  Serial.println((unsigned long)log.size());
  log.close();

  fs.end();  // marks the file system as cleanly unmounted
  Serial.println("done");
}

void loop() {}

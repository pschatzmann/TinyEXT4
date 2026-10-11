// Lists all files of an ext4 formatted SD card (SPI) and prints a text file.
// Format the card on Linux with: mkfs.ext4 /dev/sdX1 (or the whole device)
#include <SPI.h>

#include "TinyEXT4.h"
#include "tinyext4/driver/ArduinoSpiIO.h"

using namespace tinyext4;

ArduinoSpiIO sd(SS);  // CS pin
Ext4FS fs;

void listDir(File dir, int indent) {
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    for (int j = 0; j < indent; j++) Serial.print("  ");
    Serial.print(f.name());
    if (f.isDirectory()) {
      Serial.println("/");
      listDir(f, indent + 1);
    } else {
      Serial.print(" ");
      Serial.println((unsigned long)f.size());
    }
  }
}

void setup() {
  Serial.begin(115200);
  while (!Serial);
  SPI.begin();
  if (!fs.begin(sd, true)) {  // read only
    Serial.print("mount failed: ");
    Serial.println(fs.lastErrorText());
    return;
  }
  Serial.print("free bytes: ");
  Serial.println((unsigned long)(fs.freeBytes() / 1024));
  listDir(fs.open("/"), 0);

  File f = fs.open("/hello.txt");
  if (f) {
    while (f.available()) Serial.write(f.read());
    f.close();
  }
  fs.end();
}

void loop() {}

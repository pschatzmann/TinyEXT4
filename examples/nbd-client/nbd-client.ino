// ESP32: mounts an ext4 image that is exported by an NBD server on the
// network, using the client of TinyNBD (https://github.com/pschatzmann/TinyNBD)
//
// Linux: create and export an image with
//   truncate -s 64M disk.img && mkfs.ext4 disk.img
//   nbdkit -f file disk.img          (or: qemu-nbd -x ram -t disk.img)
#include <WiFi.h>

#include "NBD.h"  // TinyNBD

#include "TinyEXT4.h"
#include "tinyext4/driver/NBDClientIO.h"

using namespace tinyext4;

const char* ssid = "ssid";
const char* password = "password";

WiFiClient wifi;
nbd::NBDClient nbd_client(wifi);
NBDClientIO drv{nbd_client, "192.168.1.10", nbd::NBD_DEFAULT_PORT, ""};
Ext4FS fs;

void setup() {
  Serial.begin(115200);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) delay(500);

  if (!fs.begin(drv)) {
    Serial.print("mount failed: ");
    Serial.println(fs.lastErrorText());
    return;
  }
  File f = fs.open("/from-esp32.txt", "w");
  f.println("hello from the ESP32");
  f.close();

  File root = fs.open("/");
  for (File e = root.openNextFile(); e; e = root.openNextFile()) {
    Serial.println(e.name());
  }
  fs.end();
}

void loop() {}

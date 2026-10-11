# Drivers

The block device drivers come from [TinyFATFS](https://github.com/pschatzmann/TinyFATFS) and use the same diskio-style interface (`disk_initialize`, `disk_read`, `disk_write`, `disk_ioctl` on 512-byte sectors), so a driver can be moved between the two libraries with minimal changes. All drivers are in [src/tinyext4/driver](../src/tinyext4/driver). Include the one you need after `TinyEXT4.h`.

| Driver | Header | Use |
|---|---|---|
| `ArduinoSpiIO` | `tinyext4/driver/ArduinoSpiIO.h` | SD card on the Arduino SPI API (any board) |
| `ArduinoSpiExtIO<GPIO>` | `tinyext4/driver/ArduinoSpiIOExt.h` | as above, with the CS pin on a port expander |
| `Esp32SdmmcIO` | `tinyext4/driver/Esp32SdmmcIO.h` | ESP32 SDMMC controller, 1 or 4 bit |
| `RamIO` | included | RAM disk, in PSRAM on ESP32 / RP2350 if available |
| `StreamIO<T>` | included | any object with `seek`, `readBytes`, `write`, ... |
| `NBDClientIO` | `tinyext4/driver/NBDClientIO.h` | remote disk over the network with [TinyNBD](https://github.com/pschatzmann/TinyNBD) |
| `TinyUsbMscIO` | `tinyext4/driver/TinyUsbMscIO.h` | exports a driver as a USB drive (Adafruit TinyUSB) |
| `FileIO` | included (desktop only) | image file or block device on Linux / macOS |

## SD card over SPI

```c++
#include <SPI.h>
#include "TinyEXT4.h"
#include "tinyext4/driver/ArduinoSpiIO.h"

ArduinoSpiIO sd(SS);                      // CS pin; optional: SPIClass, speed in Hz
...
SPI.begin();
fs.begin(sd);
```

## ESP32 SDMMC

```c++
#include "tinyext4/driver/Esp32SdmmcIO.h"
Esp32SdmmcIO sd(false, SDMMC_FREQ_DEFAULT);   // 4-bit, 20 MHz; true = 1-bit
fs.begin(sd);
```

## RAM disk

`RamIO` allocates its sectors with the [Allocator](memory.md), which uses PSRAM if available. Load an image created with `mkfs.ext4`:

```c++
RamIO ram(image_len / 512);
ram.load(image_data, image_len);
fs.begin(ram);
```

## NBD (TinyNBD)

With the client of [TinyNBD](https://github.com/pschatzmann/TinyNBD), an ext4 image on a server becomes the disk of the microcontroller. Install TinyNBD and include `NBD.h` before the driver:

```c++
#include <WiFi.h>
#include "NBD.h"                          // TinyNBD
#include "TinyEXT4.h"
#include "tinyext4/driver/NBDClientIO.h"

WiFiClient wifi;
nbd::NBDClient nbd_client(wifi);
NBDClientIO drv{nbd_client, "192.168.1.10", nbd::NBD_DEFAULT_PORT, ""};
...
fs.begin(drv);
```

On the server: `nbdkit -f file disk.img` or `qemu-nbd -t disk.img`. See [examples/nbd-client](../examples/nbd-client).

It also works the other way around: the TinyNBD *server* exports an SD card or a RAM disk of the microcontroller to Linux. Linux then mounts it as `/dev/nbd0` and does the ext4 work itself. Only one side may mount the file system with write access at a time.

## USB mass storage

`TinyUsbMscIO` exports any driver as a USB drive (Adafruit TinyUSB). The PC mounts the ext4 file system directly. While the PC has it mounted read/write, mount it on the microcontroller read-only, or not at all.

## Partitions

`begin(io, readOnly, partition)` first looks for a file system on the whole device. If there is none, it reads the MBR (primary partitions) or a GPT and uses the first partition that holds an ext file system. `partition = n` selects the n-th one (0-based).

## Writing your own driver

Derive from `tinyext4::IO` (or `BaseIO`) and implement the five functions for drive 0 with 512-byte sectors. `disk_status()` returns `STA_PROTECT` for write-protected media, and TinyEXT4 then mounts read-only. `disk_ioctl(CTRL_SYNC)` is called after each operation that changes data.

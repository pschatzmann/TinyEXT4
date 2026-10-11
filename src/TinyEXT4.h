// SPDX-License-Identifier: MIT
/**
 * @file TinyEXT4.h
 * @brief Header only ext2/ext3/ext4 file system (without journaling) for
 * microcontrollers and the desktop.
 *
 * @code
 * #include "TinyEXT4.h"
 * #include "tinyext4/driver/ArduinoSpiIO.h"
 * using namespace tinyext4;
 *
 * ArduinoSpiIO sd(SS);
 * Ext4FS fs;
 *
 * void setup() {
 *   fs.begin(sd);
 *   File f = fs.open("/hello.txt", "w");
 *   f.println("hello");
 *   f.close();
 *   fs.end();
 * }
 * @endcode
 */
#pragma once
#include "tinyext4/Config.h"
#include "tinyext4/Ext4FS.h"
#include "tinyext4/driver/BaseIO.h"
#include "tinyext4/driver/RamIO.h"
#include "tinyext4/driver/StreamIO.h"
#ifndef ARDUINO
#include "tinyext4/driver/FileIO.h"
#endif

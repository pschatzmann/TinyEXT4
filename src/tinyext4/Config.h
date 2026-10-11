// SPDX-License-Identifier: MIT
#pragma once

/**
 * @file Config.h
 * @brief Compile time settings: define them before including TinyEXT4.h
 */

/// Default number of 512 byte sectors in the metadata cache (RAM = n * ~520
/// bytes): can be changed at runtime with Ext4FS::setCacheSectors()
#ifndef TINYEXT4_CACHE_SECTORS
#define TINYEXT4_CACHE_SECTORS 4
#endif

/// Max length of a file name kept in File::name() and DirEntry (ext4: 255)
#ifndef TINYEXT4_NAME_MAX
#define TINYEXT4_NAME_MAX 255
#endif

/// Max number of sectors per driver call for bulk data transfers
#ifndef TINYEXT4_MAX_TRANSFER_SECTORS
#define TINYEXT4_MAX_TRANSFER_SECTORS 128
#endif

/// 0: compile without write support (smaller code)
#ifndef TINYEXT4_WRITE
#define TINYEXT4_WRITE 1
#endif

/// Groups whose bitmap checksums are updated lazily (at sync)
#ifndef TINYEXT4_DIRTY_GROUPS
#define TINYEXT4_DIRTY_GROUPS 8
#endif

/// 1: print log messages with printf (desktop) or Serial
#ifndef TINYEXT4_LOG
#define TINYEXT4_LOG 0
#endif

#if TINYEXT4_LOG
#ifdef ARDUINO
#include <Arduino.h>
#define TINYEXT4_LOGF(...) Serial.printf(__VA_ARGS__)
#else
#include <stdio.h>
#define TINYEXT4_LOGF(...) printf(__VA_ARGS__)
#endif
#else
#define TINYEXT4_LOGF(...)
#endif

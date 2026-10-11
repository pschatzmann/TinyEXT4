// SPDX-License-Identifier: MIT
#pragma once

#ifndef ARDUINO

#include <cstdint>
#include <cstdio>
#include <cstring>
#ifndef _WIN32
#include <sys/types.h>
#endif
#include "IO.h"

namespace tinyext4 {

/**
 * @brief Block device backed by a plain host OS file (a disk image), for
 * the desktop/native build only. Unlike RamIO, the backing store survives
 * past the lifetime of one process: the image can be created with
 * mkfs.ext4 and inspected or cross-checked with ordinary OS tools (e2fsck,
 * debugfs, dumpe2fs, a loopback mount, ...).
 *
 * TinyEXT4 does not format: an image that does not exist yet is created
 * empty (sparse) and must be formatted with mkfs.ext4 before mounting.
 * With sectorCount = 0 the size is taken from the existing file. Images that
 * are not writable are opened read only.
 * @ingroup io
 */
class FileIO : public IO {
 public:
  /// path is the disk image file; it is created (sectorCount * sectorSize
  /// bytes, sparse) if it does not exist yet, or opened as-is if it does.
  FileIO(const char* path, size_t sectorCount = 0, size_t sectorSize = 512)
      : path(path), sector_count(sectorCount), sector_size(sectorSize) {}

  ~FileIO() { end(); }

  /// closes the image file
  void end() {
    if (file != nullptr) fclose(file);
    file = nullptr;
    status = STA_NOINIT;
  }

  DSTATUS disk_initialize(BYTE pdrv) override {
    if (pdrv != 0) return STA_NODISK;
    if (file == nullptr && !open_or_create()) return STA_NODISK;
    status = read_only ? STA_PROTECT : STA_CLEAR;
    return status;
  }

  DSTATUS disk_status(BYTE pdrv) override {
    if (pdrv != 0) return STA_NODISK;
    return status;
  }

  DRESULT disk_read(BYTE pdrv, BYTE* buff, LBA_t sector, UINT count) override {
    if (pdrv != 0) return RES_NOTRDY;
    if (status == STA_NOINIT) return RES_NOTRDY;
    if (seek_to(file, (uint64_t)sector * sector_size) != 0) return RES_ERROR;
    size_t n = fread(buff, sector_size, count, file);
    return n == count ? RES_OK : RES_ERROR;
  }

  DRESULT disk_write(BYTE pdrv, const BYTE* buff, LBA_t sector,
                     UINT count) override {
    if (pdrv != 0) return RES_NOTRDY;
    if (status == STA_NOINIT) return RES_NOTRDY;
    if (read_only) return RES_WRPRT;
    if (seek_to(file, (uint64_t)sector * sector_size) != 0) return RES_ERROR;
    size_t n = fwrite(buff, sector_size, count, file);
    return n == count ? RES_OK : RES_ERROR;
  }

  DRESULT disk_ioctl(BYTE pdrv, ioctl_cmd_t cmd, void* buff) override {
    if (pdrv != 0) return RES_PARERR;
    switch (cmd) {
      case CTRL_SYNC:
        return fflush(file) == 0 ? RES_OK : RES_ERROR;

      case GET_SECTOR_COUNT: {
        if (buff == nullptr) return RES_PARERR;
        DWORD result = (DWORD)sector_count;
        memcpy(buff, &result, sizeof(result));
        return RES_OK;
      }

      case GET_SECTOR_SIZE: {
        if (buff == nullptr) return RES_PARERR;
        WORD result = (WORD)sector_size;
        memcpy(buff, &result, sizeof(result));
        return RES_OK;
      }

      case GET_BLOCK_SIZE: {
        if (buff == nullptr) return RES_PARERR;
        DWORD result = 1;
        memcpy(buff, &result, sizeof(result));
        return RES_OK;
      }

      default:
        return RES_PARERR;
    }
  }

 protected:
  const char* path;
  size_t sector_count;
  size_t sector_size;
  FILE* file = nullptr;
  DSTATUS status = STA_NOINIT;
  bool read_only = false;

  /// seek with a 64 bit offset so that images above 2GB work on all hosts
  static int seek_to(FILE* f, uint64_t pos) {
#ifdef _WIN32
    return _fseeki64(f, (__int64)pos, SEEK_SET);
#else
    return fseeko(f, (off_t)pos, SEEK_SET);
#endif
  }

  bool open_or_create() {
    read_only = false;
    file = fopen(path, "r+b");
    if (file == nullptr) {
      file = fopen(path, "rb");
      read_only = (file != nullptr);
    }
    if (file == nullptr) {
      if (sector_count == 0) return false;
      file = fopen(path, "w+b");
      if (file == nullptr) return false;
    }
    // grow (sparsely) to the requested size if the file is smaller, whether
    // freshly created or a pre-existing image that's too small
    uint64_t wanted_size = (uint64_t)sector_count * sector_size;
    if (seek_to(file, 0) != 0) return false;
    if (fseek(file, 0, SEEK_END) != 0) return false;
#ifdef _WIN32
    __int64 current_size = _ftelli64(file);
#else
    off_t current_size = ftello(file);
#endif
    if (current_size < 0) return false;
    if (sector_count == 0) {
      sector_count = (size_t)((uint64_t)current_size / sector_size);
      return sector_count > 0;
    }
    if (!read_only && (uint64_t)current_size < wanted_size) {
      if (seek_to(file, wanted_size - 1) != 0) return false;
      uint8_t zero = 0;
      if (fwrite(&zero, 1, 1, file) != 1) return false;
      fflush(file);
    }
    return true;
  }
};

}  // namespace tinyext4

#endif  // !ARDUINO

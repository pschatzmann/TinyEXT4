// SPDX-License-Identifier: MIT
#pragma once

#include <stdlib.h>
#include <cstring>
#include <vector>
#include "IO.h"
#include "../Allocator.h"

namespace tinyext4 {

/**
 * @brief The data is stored in RAM. The sectors are allocated with the
 * Allocator: on an ESP32 or RP2350 with PSRAM they are stored in PSRAM.
 * Use load() to copy an ext4 image (created with mkfs.ext4) into it.
 * @ingroup io
 */
class RamIO : public IO {
 public:
  //  valid sector sizes are 512, 1024, 2048 and 4096
  RamIO(int sectorCount, int sectorSize = 512,
        Allocator& allocator = defaultAllocator()) {
    sector_size = sectorSize;
    sector_count = sectorCount;
    p_alloc = &allocator;
  }

  ~RamIO() {
    for (auto* ptr : sectors) {
      p_alloc->release(ptr);
    }
  }

  /// Copies an ext4 image (e.g. created with mkfs.ext4) into the RAM disk
  bool load(const uint8_t* image, size_t len) {
    if (disk_initialize(0) != STA_CLEAR) return false;
    if (len > (size_t)sector_count * sector_size) return false;
    for (size_t pos = 0; pos < len; pos += sector_size) {
      size_t n = len - pos < (size_t)sector_size ? len - pos : sector_size;
      memcpy(sectors[pos / sector_size], image + pos, n);
    }
    return true;
  }

  DSTATUS disk_initialize(BYTE pdrv) override {
    if (pdrv != 0) return STA_NODISK;
    // allocate sectors
    if (sectors.size() == 0) {
      for (size_t j = 0; j < sector_count; j++) {
        uint8_t* ptr = (uint8_t*)p_alloc->allocate(sector_size);
        if (ptr == nullptr) {
          // out of memory: release what we got and report the failure
          for (auto* p : sectors) p_alloc->release(p);
          sectors.clear();
          status = STA_NOINIT;
          return status;
        }
        sectors.push_back(ptr);
      }
    }
    status = STA_CLEAR;
    return status;
  }

  DSTATUS disk_status(BYTE pdrv) override {
    if (pdrv != 0) return STA_NODISK;
    return status;
  }

  DRESULT disk_read(BYTE pdrv, BYTE* buffer, LBA_t sectorNo, UINT sectorCount) override {
    if (pdrv != 0) return RES_NOTRDY;
    if (status == STA_NOINIT) return RES_NOTRDY;
    if (sectors.size() == 0 || sectorNo + sectorCount > sector_count) return RES_ERROR;
    for (LBA_t j = sectorNo; j < sectorNo + sectorCount; j++) {
      int idx = j - sectorNo;
      uint8_t* source = sectors[j];
      memcpy(buffer + (idx * sector_size), source , sector_size);
    }
    return RES_OK;
  }

  DRESULT disk_write(BYTE pdrv, const BYTE* buffer, LBA_t sectorNo,
                     UINT sectorCount) override {
    if (pdrv != 0) return RES_NOTRDY;
    if (status == STA_NOINIT) return RES_NOTRDY;
    if (sectors.size() == 0 || sectorNo + sectorCount > sector_count) return RES_ERROR;
    for (LBA_t j = sectorNo; j < sectorNo + sectorCount; j++) {
      int idx = j - sectorNo;
      uint8_t* target = sectors[j];
      memcpy(target, buffer + (idx * sector_size), sector_size);
    }
    return RES_OK;
  }

  DRESULT disk_ioctl(BYTE pdrv, ioctl_cmd_t cmd, void* buffer) override {
    DRESULT res;
    if (pdrv) return RES_PARERR; /* Check parameter */
    switch (cmd) {
      case CTRL_SYNC: /* Wait for end of internal write process of the drive */
        res = RES_OK;
        break;

      case GET_SECTOR_COUNT: { /* Get drive capacity in unit of sector (DWORD)
                                */
        DWORD result = sector_count;
        memcpy(buffer, &result, (sizeof(result)));
        res = RES_OK;
      } break;

      case GET_BLOCK_SIZE: { /* Get erase block size in unit of sector (DWORD)
                              */
        DWORD result = 1;
        memcpy(buffer, &result, (sizeof(result)));
        res = RES_OK;
      } break;

      case CTRL_TRIM: { /* Erase a block of sectors (used when _USE_ERASE == 1)
                         */
        if (buffer == nullptr || status == STA_NOINIT) {
          res = RES_PARERR;
          break;
        }
        DWORD range[2];
        // determine range
        memcpy(&range, buffer, sizeof(range));
        // validate range: must be inside the drive and allocated
        if (range[0] > range[1] || range[1] >= sector_count ||
            sectors.size() != sector_count) {
          res = RES_PARERR;
          break;
        }
        // clear memory
        for (DWORD j = range[0]; j <= range[1]; j++) {
          memset(sectors[j], 0, sector_size);
        }
        res = RES_OK; 
      } break;

      default:
        res = RES_PARERR;
        break;
    }
    return res;
  }

 protected:
  std::vector<uint8_t*, AllocatorExt<uint8_t*>> sectors;
  Allocator* p_alloc = nullptr;
  DSTATUS status = STA_NOINIT;
  int sector_size = 512;
  size_t sector_count = 0;
};


}  // namespace tinyext4
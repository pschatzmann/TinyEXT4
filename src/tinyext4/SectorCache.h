// SPDX-License-Identifier: MIT
#pragma once
#include <string.h>

#include "Allocator.h"
#include "Config.h"
#include "Util.h"
#include "driver/IO.h"

namespace tinyext4 {

/**
 * @brief Small write back cache of 512 byte sectors with LRU replacement.
 *
 * All file system metadata is accessed through byte offsets, so we never need
 * a buffer of a full file system block (1-64 KB): this keeps the RAM use at
 * count * 512 bytes, allocated with the Allocator (PSRAM if available).
 * Bulk file data bypasses the cache for whole sectors.
 */
class SectorCache {
 public:
  static constexpr uint32_t SECTOR = 512;

  ~SectorCache() { release(); }

  /// number of cached sectors (min 2): call before begin()
  void setSize(uint16_t sectors) {
    if (sectors < 2) sectors = 2;
    if (sectors != count) release();
    count = sectors;
  }
  uint16_t size() const { return count; }

  void setAllocator(Allocator& alloc) {
    release();
    p_alloc = &alloc;
  }

  /// allocates the buffers
  bool begin(IO* io, uint32_t baseLba) {
    p_io = io;
    base = baseLba;
    if (entries == nullptr) {
      entries = (Entry*)p_alloc->allocate(sizeof(Entry) * count);
      if (entries == nullptr) return false;
    }
    invalidate();
    return true;
  }

  /// releases the buffers (without writing dirty sectors)
  void release() {
    if (entries != nullptr) p_alloc->release(entries);
    entries = nullptr;
  }

  /// drops all entries without writing them
  void invalidate() {
    for (auto& e : *this) {
      e.valid = false;
      e.dirty = false;
    }
  }

  bool read(uint64_t off, void* dst, size_t len) {
    uint8_t* d = (uint8_t*)dst;
    while (len > 0) {
      uint32_t o = (uint32_t)(off % SECTOR);
      size_t n = SECTOR - o < len ? SECTOR - o : len;
      Entry* e = get((uint32_t)(off / SECTOR), true);
      if (e == nullptr) return false;
      memcpy(d, e->data + o, n);
      d += n;
      off += n;
      len -= n;
    }
    return true;
  }

  bool write(uint64_t off, const void* src, size_t len) {
    return modify(off, len, (const uint8_t*)src, 0);
  }

  bool fill(uint64_t off, uint8_t value, size_t len) {
    return modify(off, len, nullptr, value);
  }

  bool read32(uint64_t off, uint32_t& v) {
    uint8_t b[4];
    if (!read(off, b, 4)) return false;
    v = get32(b);
    return true;
  }

  bool write32(uint64_t off, uint32_t v) {
    uint8_t b[4];
    put32(b, v);
    return write(off, b, 4);
  }

  bool write16(uint64_t off, uint16_t v) {
    uint8_t b[2];
    put16(b, v);
    return write(off, b, 2);
  }

  /// Reads a large range: whole sectors are transferred directly into dst
  bool readBulk(uint64_t off, void* dst, size_t len) {
    uint8_t* d = (uint8_t*)dst;
    while (len > 0) {
      if (off % SECTOR == 0 && len >= SECTOR) {
        uint32_t lba = (uint32_t)(off / SECTOR);
        uint32_t n = (uint32_t)(len / SECTOR);
        if (n > TINYEXT4_MAX_TRANSFER_SECTORS) n = TINYEXT4_MAX_TRANSFER_SECTORS;
        if (p_io->disk_read(0, d, base + lba, n) != RES_OK) return false;
        // cached sectors may be newer than the device
        for (auto& e : *this) {
          if (e.valid && e.lba >= lba && e.lba < lba + n)
            memcpy(d + (e.lba - lba) * SECTOR, e.data, SECTOR);
        }
        d += n * SECTOR;
        off += (uint64_t)n * SECTOR;
        len -= n * SECTOR;
      } else {
        size_t n = SECTOR - (size_t)(off % SECTOR);
        if (n > len) n = len;
        if (!read(off, d, n)) return false;
        d += n;
        off += n;
        len -= n;
      }
    }
    return true;
  }

  /// Writes a large range: whole sectors are written directly to the device
  bool writeBulk(uint64_t off, const void* src, size_t len) {
    const uint8_t* s = (const uint8_t*)src;
    while (len > 0) {
      if (off % SECTOR == 0 && len >= SECTOR) {
        uint32_t lba = (uint32_t)(off / SECTOR);
        uint32_t n = (uint32_t)(len / SECTOR);
        if (n > TINYEXT4_MAX_TRANSFER_SECTORS) n = TINYEXT4_MAX_TRANSFER_SECTORS;
        if (p_io->disk_write(0, s, base + lba, n) != RES_OK) return false;
        for (auto& e : *this) {
          if (e.valid && e.lba >= lba && e.lba < lba + n) {
            e.valid = false;
            e.dirty = false;
          }
        }
        s += n * SECTOR;
        off += (uint64_t)n * SECTOR;
        len -= n * SECTOR;
      } else {
        size_t n = SECTOR - (size_t)(off % SECTOR);
        if (n > len) n = len;
        if (!write(off, s, n)) return false;
        s += n;
        off += n;
        len -= n;
      }
    }
    return true;
  }

  /// Updates crc with the CRC32C of a byte range on the device
  bool crc32c(uint64_t off, size_t len, uint32_t& crc) {
    while (len > 0) {
      uint32_t o = (uint32_t)(off % SECTOR);
      size_t n = SECTOR - o < len ? SECTOR - o : len;
      Entry* e = get((uint32_t)(off / SECTOR), true);
      if (e == nullptr) return false;
      crc = tinyext4::crc32c(crc, e->data + o, n);
      off += n;
      len -= n;
    }
    return true;
  }

  /// writes all dirty sectors to the device
  bool flush() {
    bool ok = true;
    for (auto& e : *this) {
      if (e.valid && e.dirty) ok = writeEntry(e) && ok;
    }
    return ok;
  }

 protected:
  // zero initialized by the allocator
  struct Entry {
    uint32_t lba;
    uint32_t stamp;
    bool valid;
    bool dirty;
    uint8_t data[SECTOR];
  };
  Entry* entries = nullptr;
  uint16_t count = TINYEXT4_CACHE_SECTORS;
  Allocator* p_alloc = &defaultAllocator();
  IO* p_io = nullptr;

  // range based for over the entries
  Entry* begin() { return entries; }
  Entry* end() { return entries ? entries + count : nullptr; }
  uint32_t base = 0;
  uint32_t clock = 0;

  bool writeEntry(Entry& e) {
    if (p_io->disk_write(0, e.data, base + e.lba, 1) != RES_OK) return false;
    e.dirty = false;
    return true;
  }

  /// returns the cache entry for the sector, loading it if requested
  Entry* get(uint32_t lba, bool load) {
    if (entries == nullptr) return nullptr;
    Entry* victim = &entries[0];
    for (auto& e : *this) {
      if (e.valid && e.lba == lba) {
        e.stamp = ++clock;
        return &e;
      }
      if (!e.valid) {
        if (victim->valid) victim = &e;
      } else if (victim->valid && e.stamp < victim->stamp) {
        victim = &e;
      }
    }
    if (victim->valid && victim->dirty && !writeEntry(*victim)) return nullptr;
    victim->valid = false;
    victim->dirty = false;
    if (load && p_io->disk_read(0, victim->data, base + lba, 1) != RES_OK)
      return nullptr;
    victim->lba = lba;
    victim->valid = true;
    victim->stamp = ++clock;
    return victim;
  }

  bool modify(uint64_t off, size_t len, const uint8_t* src, uint8_t value) {
    while (len > 0) {
      uint32_t o = (uint32_t)(off % SECTOR);
      size_t n = SECTOR - o < len ? SECTOR - o : len;
      // a fully overwritten sector does not need to be read first
      Entry* e = get((uint32_t)(off / SECTOR), n != SECTOR);
      if (e == nullptr) return false;
      if (src) {
        memcpy(e->data + o, src, n);
        src += n;
      } else {
        memset(e->data + o, value, n);
      }
      e->dirty = true;
      off += n;
      len -= n;
    }
    return true;
  }
};

}  // namespace tinyext4

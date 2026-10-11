// SPDX-License-Identifier: MIT
#pragma once
#include <string.h>
#ifndef ARDUINO
#include <time.h>
#endif

#include "Config.h"
#include "Ext4Defs.h"
#include "File.h"
#include "SectorCache.h"
#include "Util.h"
#include "driver/IO.h"

namespace tinyext4 {

/**
 * @brief ext2/ext3/ext4 file system without journaling.
 *
 * Reading supports extents and the old block maps, htree directories (read
 * linearly), 64bit, flex_bg, meta_bg and inline data. Writing needs the
 * extents and filetype features; it keeps the metadata_csum / gdt_csum
 * checksums up to date but does not write a journal: the file system must be
 * clean (no journal recovery pending).
 *
 * The RAM use is the sector cache (TINYEXT4_CACHE_SECTORS * 512 bytes) plus
 * about 300 bytes per open File: no buffer of a full file system block is
 * needed.
 */
class Ext4FS {
 public:
  Ext4FS() = default;
  ~Ext4FS() { end(); }

  /**
   * @brief Mounts the file system.
   * @param io block device driver
   * @param readOnly mount read only; we also mount read only if the file
   * system uses features that we can not write
   * @param partition -1: whole device or first ext partition (MBR / GPT),
   * n: n-th ext partition (0 based)
   */
  bool begin(IO& io, bool readOnly = false, int partition = -1) {
    end();
    p_io = &io;
    error = Error::OK;
    DSTATUS st = io.disk_initialize(0);
    if (st & (STA_NOINIT | STA_NODISK)) return fail(Error::IO);
    if (st & STA_PROTECT) readOnly = true;
    uint32_t start = 0;
    if (!cache.begin(p_io, 0)) return fail(Error::NoMemory);
    if (!findPartition(partition, start)) return false;
    cache.begin(p_io, start);
    if (!readSuper(readOnly)) return false;
    mounted = true;
    return true;
  }

  /// writes all changes, marks the file system as cleanly unmounted and
  /// releases the cache buffers
  void end() {
    if (!mounted) {
      cache.release();
      return;
    }
#if TINYEXT4_WRITE
    if (!read_only && fs_dirty) {
      sync();
      state |= ext4::STATE_VALID;
      writeSuper();
      cache.flush();
      p_io->disk_ioctl(0, CTRL_SYNC, nullptr);
      fs_dirty = false;
    }
#endif
    mounted = false;
    cache.release();
  }

  /// writes all cached data and metadata to the device
  bool sync() {
    if (!mounted) return fail(Error::NotMounted);
#if TINYEXT4_WRITE
    if (read_only) return true;
    bool ok = flushExtentCsums();
    ok = syncGroups() && ok;
    if (sb_dirty) ok = writeSuper() && ok;
    ok = cache.flush() && ok;
    p_io->disk_ioctl(0, CTRL_SYNC, nullptr);
    if (!ok) return fail(Error::IO);
#endif
    return true;
  }

  /// opens a file: mode is "r", "r+", "w", "w+", "a" or "a+"
  File open(const char* path, const char* mode = "r") {
    return open(path, parseMode(mode));
  }

  /// opens a file with OpenFlags
  File open(const char* path, uint8_t flags);

  bool exists(const char* path) {
    Inode in;
    return resolve(path, strlen(path), in);
  }

  bool stat(const char* path, FileInfo& info) {
    Inode in;
    if (!resolve(path, strlen(path), in)) return false;
    info.inode = in.ino;
    info.mode = in.mode;
    info.links = in.links;
    info.uid = in.uid;
    info.gid = in.gid;
    info.size = in.size;
    info.atime = in.atime;
    info.mtime = in.mtime;
    info.ctime = in.ctime;
    return true;
  }

  /// reads the target of a symbolic link
  bool readlink(const char* path, char* buf, size_t len);

#if TINYEXT4_WRITE
  bool mkdir(const char* path);
  bool rmdir(const char* path);
  /// deletes a file
  bool remove(const char* path);
  /// renames or moves a file or directory; an existing target file is replaced
  bool rename(const char* from, const char* to);
#endif

  Error lastError() const { return error; }
  const char* lastErrorText() const { return errorText(error); }
  bool isMounted() const { return mounted; }
  bool isReadOnly() const { return read_only; }
  uint32_t blockSize() const { return block_size; }
  uint64_t totalBytes() const { return blocks_count * block_size; }
  uint64_t freeBytes() const { return free_blocks * block_size; }
  uint64_t usedBytes() const { return totalBytes() - freeBytes(); }
  uint32_t freeInodes() const { return free_inodes; }
  const char* volumeName() const { return volume_name; }

  /// number of 512 byte sectors in the metadata cache: call before begin()
  void setCacheSectors(uint16_t sectors) { cache.setSize(sectors); }
  /// allocator for the cache buffers (default: PSRAM if available)
  void setAllocator(Allocator& alloc) { cache.setAllocator(alloc); }

  /// callback which provides the current time (seconds since 1970)
  void setTimeCallback(uint32_t (*cb)()) { time_cb = cb; }
  /// owner of new files and directories
  void setOwner(uint32_t uid, uint32_t gid) {
    new_uid = uid;
    new_gid = gid;
  }
  /// permission bits of new files and directories
  void setPermissions(uint16_t fileMode, uint16_t dirMode) {
    file_mode = fileMode & 07777;
    dir_mode = dirMode & 07777;
  }

 protected:
  friend class File;
  IO* p_io = nullptr;
  SectorCache cache;
  Error error = Error::OK;
  bool mounted = false;
  bool read_only = true;
  bool sb_dirty = false;
  bool fs_dirty = false;

  // superblock
  uint64_t blocks_count = 0;
  uint64_t free_blocks = 0;
  uint32_t inodes_count = 0;
  uint32_t free_inodes = 0;
  uint32_t first_data_block = 0;
  uint32_t block_size = 1024;
  uint8_t log_block = 10;
  uint32_t blocks_per_group = 0;
  uint32_t clusters_per_group = 0;
  uint32_t inodes_per_group = 0;
  uint32_t group_count = 0;
  uint32_t first_ino = 11;
  uint16_t inode_size = 128;
  uint16_t desc_size = 32;
  uint16_t state = 0;
  uint16_t reserved_gdt = 0;
  uint32_t first_meta_bg = 0;
  uint32_t backup_bgs[2] = {0, 0};
  uint32_t compat = 0, incompat = 0, ro_compat = 0;
  uint32_t csum_seed = 0;
  uint8_t uuid[16] = {0};
  char volume_name[17] = {0};

  // settings
  uint32_t (*time_cb)() = nullptr;
  uint32_t new_uid = 0, new_gid = 0;
  uint16_t file_mode = 0644, dir_mode = 0755;
  uint32_t gen_counter = 0x5EED;

  // lazily updated checksums
  struct DirtyGroup {
    uint32_t group;
    uint8_t mask;
  };
  DirtyGroup dirty_groups[TINYEXT4_DIRTY_GROUPS];
  uint8_t dirty_group_count = 0;
  struct PendingCsum {
    uint64_t blk;
    uint32_t seed;
  };
  PendingCsum pending_ext[4];
  uint8_t pending_ext_count = 0;

  /// extent tree node: in the inode (mem) or in a block
  struct Node {
    uint64_t blk;
    uint8_t* mem;
  };

  /// raw directory entry
  struct RawDirent {
    uint64_t dev;  // device byte offset of the entry
    uint32_t inode;
    uint32_t recLen;
    uint8_t nameLen;
    uint8_t type;
  };

  /// directory iterator state
  struct DirIter {
    uint64_t pos = 0;
    MapCache map;
  };

  bool fail(Error e) {
    error = e;
    TINYEXT4_LOGF("tinyext4: %s\n", errorText(e));
    return false;
  }

  bool hasCsum() const { return ro_compat & ext4::RO_COMPAT_METADATA_CSUM; }
  bool hasGroupCsum() const {
    return ro_compat & (ext4::RO_COMPAT_METADATA_CSUM | ext4::RO_COMPAT_GDT_CSUM);
  }
  bool hasFiletype() const { return incompat & ext4::INCOMPAT_FILETYPE; }
  uint64_t blockOff(uint64_t blk) const { return blk << log_block; }
  uint32_t sectorsPerBlock() const { return block_size / 512; }
  uint64_t groupFirstBlock(uint32_t g) const {
    return first_data_block + (uint64_t)g * blocks_per_group;
  }
  uint32_t blocksInGroup(uint32_t g) const {
    uint64_t rest = blocks_count - groupFirstBlock(g);
    return rest < blocks_per_group ? (uint32_t)rest : blocks_per_group;
  }
  uint32_t now() {
#ifdef ARDUINO
    return time_cb ? time_cb() : 0;
#else
    return time_cb ? time_cb() : (uint32_t)::time(nullptr);
#endif
  }

  static uint8_t parseMode(const char* mode) {
    if (mode == nullptr || *mode == 0) return OpenFlags::Read;
    bool plus = strchr(mode, '+') != nullptr;
    switch (mode[0]) {
      case 'w':
        return OpenFlags::Write | OpenFlags::Create | OpenFlags::Truncate |
               (plus ? OpenFlags::Read : 0);
      case 'a':
        return OpenFlags::Write | OpenFlags::Create | OpenFlags::Append |
               (plus ? OpenFlags::Read : 0);
      default:
        return OpenFlags::Read | (plus ? OpenFlags::Write : 0);
    }
  }

  // ------------------------------------------------------------------
  // mount
  // ------------------------------------------------------------------

  bool probe(uint32_t lba) {
    uint8_t m[2];
    if (!cache.read((uint64_t)lba * 512 + ext4::SB_OFFSET + 0x38, m, 2))
      return false;
    return get16(m) == ext4::MAGIC;
  }

  /// finds the start sector of the file system (whole disk, MBR or GPT)
  bool findPartition(int partition, uint32_t& start) {
    if (partition < 0 && probe(0)) {
      start = 0;
      return true;
    }
    uint8_t mbr[66];
    if (!cache.read(0x1BE, mbr, 66)) return fail(Error::IO);
    int found = 0;
    if (mbr[64] == 0x55 && mbr[65] == 0xAA) {
      for (int j = 0; j < 4; j++) {
        uint8_t* e = mbr + 16 * j;
        uint8_t type = e[4];
        if (type == 0xEE) return findGptPartition(partition, start);
        uint32_t lba = get32(e + 8);
        if (type == 0 || lba == 0) continue;
        if (probe(lba) && (partition < 0 || found++ == partition)) {
          start = lba;
          return true;
        }
      }
    }
    return fail(Error::NotExt4);
  }

  bool findGptPartition(int partition, uint32_t& start) {
    uint8_t hdr[0x58];
    if (!cache.read(512, hdr, sizeof(hdr))) return fail(Error::IO);
    if (memcmp(hdr, "EFI PART", 8) != 0) return fail(Error::NotExt4);
    uint32_t entries = get32(hdr + 0x48);
    uint32_t count = get32(hdr + 0x50);
    uint32_t esize = get32(hdr + 0x54);
    if (count > 128) count = 128;
    int found = 0;
    for (uint32_t j = 0; j < count; j++) {
      uint8_t e[0x28];
      if (!cache.read((uint64_t)entries * 512 + (uint64_t)j * esize, e, sizeof(e)))
        return fail(Error::IO);
      bool used = false;
      for (int k = 0; k < 16; k++) used |= e[k] != 0;
      uint32_t lba = get32(e + 0x20);
      if (!used || lba == 0) continue;
      if (probe(lba) && (partition < 0 || found++ == partition)) {
        start = lba;
        return true;
      }
    }
    return fail(Error::NotExt4);
  }

  bool readSuper(bool readOnly) {
    uint8_t sb[0x274];
    if (!cache.read(ext4::SB_OFFSET, sb, sizeof(sb))) return fail(Error::IO);
    if (get16(sb + 0x38) != ext4::MAGIC) return fail(Error::NotExt4);
    incompat = get32(sb + 0x60);
    ro_compat = get32(sb + 0x64);
    compat = get32(sb + 0x5C);
    bool is64 = incompat & ext4::INCOMPAT_64BIT;
    inodes_count = get32(sb + 0x00);
    blocks_count = get32(sb + 0x04) | (is64 ? (uint64_t)get32(sb + 0x150) << 32 : 0);
    free_blocks = get32(sb + 0x0C) | (is64 ? (uint64_t)get32(sb + 0x158) << 32 : 0);
    free_inodes = get32(sb + 0x10);
    first_data_block = get32(sb + 0x14);
    uint32_t log = get32(sb + 0x18);
    if (log > 6) return fail(Error::Unsupported);
    log_block = (uint8_t)(10 + log);
    block_size = 1024u << log;
    blocks_per_group = get32(sb + 0x20);
    clusters_per_group = get32(sb + 0x24);
    inodes_per_group = get32(sb + 0x28);
    state = get16(sb + 0x3A);
    uint32_t rev = get32(sb + 0x4C);
    first_ino = rev ? get32(sb + 0x54) : 11;
    inode_size = rev ? get16(sb + 0x58) : 128;
    memcpy(uuid, sb + 0x68, 16);
    memcpy(volume_name, sb + 0x78, 16);
    volume_name[16] = 0;
    reserved_gdt = get16(sb + 0xCE);
    desc_size = is64 ? get16(sb + 0xFE) : 32;
    if (desc_size < 32) desc_size = 32;
    first_meta_bg = get32(sb + 0x104);
    backup_bgs[0] = get32(sb + 0x24C);
    backup_bgs[1] = get32(sb + 0x250);
    if (incompat & ext4::INCOMPAT_CSUM_SEED)
      csum_seed = get32(sb + 0x270);
    else
      csum_seed = crc32c(0xFFFFFFFF, uuid, 16);

    if (blocks_per_group == 0 || inodes_per_group == 0 ||
        blocks_per_group > block_size * 8 || inodes_per_group > block_size * 8 ||
        inode_size < 128 || inode_size > block_size || desc_size > 64 ||
        first_data_block >= blocks_count)
      return fail(Error::Corrupt);
    if (incompat & ~ext4::INCOMPAT_READ) return fail(Error::Unsupported);
    group_count = (uint32_t)((blocks_count - first_data_block + blocks_per_group - 1) /
                             blocks_per_group);

    bool canWrite = TINYEXT4_WRITE &&
                    (incompat & ~ext4::INCOMPAT_WRITE) == 0 &&
                    (ro_compat & ~ext4::RO_COMPAT_WRITE) == 0 &&
                    (incompat & ext4::INCOMPAT_EXTENTS) &&
                    (incompat & ext4::INCOMPAT_FILETYPE) &&
                    (state & ext4::STATE_VALID) && !(state & ext4::STATE_ERROR);
    read_only = readOnly || !canWrite;
    if (!readOnly && read_only) {
      TINYEXT4_LOGF("tinyext4: mounted read only (features 0x%x/0x%x, state %d)\n",
                    (unsigned)incompat, (unsigned)ro_compat, state);
    }
    return true;
  }

  bool hasSuper(uint32_t g) const {
    if (g == 0) return true;
    if (compat & ext4::COMPAT_SPARSE_SUPER2)
      return g == backup_bgs[0] || g == backup_bgs[1];
    if (g == 1 || !(ro_compat & ext4::RO_COMPAT_SPARSE_SUPER)) return true;
    if (!(g & 1)) return false;
    static const uint8_t bases[3] = {3, 5, 7};
    for (uint32_t b : bases) {
      uint32_t n = g;
      while (n % b == 0) n /= b;
      if (n == 1) return true;
    }
    return false;
  }

  // ------------------------------------------------------------------
  // group descriptors
  // ------------------------------------------------------------------

  uint64_t descOffset(uint32_t g) const {
    uint32_t per = block_size / desc_size;
    uint32_t db = g / per;
    uint64_t blk;
    if (!(incompat & ext4::INCOMPAT_META_BG) || db < first_meta_bg) {
      blk = first_data_block + 1 + db;
    } else {
      uint32_t first = db * per;
      blk = groupFirstBlock(first) + (hasSuper(first) ? 1 : 0);
    }
    return blockOff(blk) + (uint64_t)(g % per) * desc_size;
  }

  uint32_t descHi(const uint8_t* raw, uint32_t lo, uint32_t hiOff, int bits) const {
    if (desc_size < hiOff + bits / 8) return lo;
    uint32_t hi = bits == 16 ? get16(raw + hiOff) : get32(raw + hiOff);
    return lo | (hi << 16);
  }

  bool readDesc(uint32_t g, GroupDesc& d) {
    uint8_t raw[64] = {0};
    if (g >= group_count) return fail(Error::Corrupt);
    if (!cache.read(descOffset(g), raw, desc_size)) return fail(Error::IO);
    bool big = desc_size >= 64;
    d.blockBitmap = get32(raw + 0x00) | (big ? (uint64_t)get32(raw + 0x20) << 32 : 0);
    d.inodeBitmap = get32(raw + 0x04) | (big ? (uint64_t)get32(raw + 0x24) << 32 : 0);
    d.inodeTable = get32(raw + 0x08) | (big ? (uint64_t)get32(raw + 0x28) << 32 : 0);
    d.freeBlocks = get16(raw + 0x0C) | (big ? (uint32_t)get16(raw + 0x2C) << 16 : 0);
    d.freeInodes = get16(raw + 0x0E) | (big ? (uint32_t)get16(raw + 0x2E) << 16 : 0);
    d.usedDirs = get16(raw + 0x10) | (big ? (uint32_t)get16(raw + 0x30) << 16 : 0);
    d.flags = hasGroupCsum() ? get16(raw + 0x12) : 0;
    d.itableUnused = get16(raw + 0x1C) | (big ? (uint32_t)get16(raw + 0x32) << 16 : 0);
    return true;
  }

#if TINYEXT4_WRITE
  void descChecksum(uint32_t g, uint8_t* raw) {
    if (hasCsum()) {
      put16(raw + 0x1E, 0);
      uint32_t crc = crc32c_u32(csum_seed, g);
      crc = crc32c(crc, raw, desc_size);
      put16(raw + 0x1E, (uint16_t)crc);
    } else if (ro_compat & ext4::RO_COMPAT_GDT_CSUM) {
      uint8_t gb[4];
      put32(gb, g);
      uint16_t crc = crc16(0xFFFF, uuid, 16);
      crc = crc16(crc, gb, 4);
      crc = crc16(crc, raw, 0x1E);
      if ((incompat & ext4::INCOMPAT_64BIT) && desc_size > 0x20)
        crc = crc16(crc, raw + 0x20, desc_size - 0x20);
      put16(raw + 0x1E, crc);
    }
  }

  /// writes the counters and flags of a group descriptor
  bool writeDesc(uint32_t g, const GroupDesc& d) {
    uint8_t raw[64] = {0};
    uint64_t off = descOffset(g);
    if (!cache.read(off, raw, desc_size)) return fail(Error::IO);
    bool big = desc_size >= 64;
    put16(raw + 0x0C, (uint16_t)d.freeBlocks);
    put16(raw + 0x0E, (uint16_t)d.freeInodes);
    put16(raw + 0x10, (uint16_t)d.usedDirs);
    put16(raw + 0x12, d.flags);
    put16(raw + 0x1C, (uint16_t)d.itableUnused);
    if (big) {
      put16(raw + 0x2C, (uint16_t)(d.freeBlocks >> 16));
      put16(raw + 0x2E, (uint16_t)(d.freeInodes >> 16));
      put16(raw + 0x30, (uint16_t)(d.usedDirs >> 16));
      put16(raw + 0x32, (uint16_t)(d.itableUnused >> 16));
    }
    descChecksum(g, raw);
    if (!cache.write(off, raw, desc_size)) return fail(Error::IO);
    return true;
  }

  static constexpr uint8_t DIRTY_BLOCKS = 1;
  static constexpr uint8_t DIRTY_INODES = 2;

  /// remembers that the bitmap checksums of a group need to be updated
  bool markGroup(uint32_t g, uint8_t mask) {
    sb_dirty = true;
    if (!hasCsum()) return true;
    for (int j = 0; j < dirty_group_count; j++) {
      if (dirty_groups[j].group == g) {
        dirty_groups[j].mask |= mask;
        return true;
      }
    }
    if (dirty_group_count == TINYEXT4_DIRTY_GROUPS && !syncGroups()) return false;
    dirty_groups[dirty_group_count++] = {g, mask};
    return true;
  }

  /// updates the bitmap checksums of the modified groups
  bool syncGroups() {
    for (int j = 0; j < dirty_group_count; j++) {
      uint32_t g = dirty_groups[j].group;
      GroupDesc d;
      uint8_t raw[64] = {0};
      if (!readDesc(g, d)) return false;
      uint64_t off = descOffset(g);
      if (!cache.read(off, raw, desc_size)) return fail(Error::IO);
      if ((dirty_groups[j].mask & DIRTY_BLOCKS) && !(d.flags & ext4::BG_BLOCK_UNINIT)) {
        uint32_t crc = csum_seed;
        if (!cache.crc32c(blockOff(d.blockBitmap), clusters_per_group / 8, crc))
          return fail(Error::IO);
        put16(raw + 0x18, (uint16_t)crc);
        if (desc_size >= 0x3C) put16(raw + 0x38, (uint16_t)(crc >> 16));
      }
      if ((dirty_groups[j].mask & DIRTY_INODES) && !(d.flags & ext4::BG_INODE_UNINIT)) {
        uint32_t crc = csum_seed;
        if (!cache.crc32c(blockOff(d.inodeBitmap), inodes_per_group / 8, crc))
          return fail(Error::IO);
        put16(raw + 0x1A, (uint16_t)crc);
        if (desc_size >= 0x3C) put16(raw + 0x3A, (uint16_t)(crc >> 16));
      }
      descChecksum(g, raw);
      if (!cache.write(off, raw, desc_size)) return fail(Error::IO);
    }
    dirty_group_count = 0;
    return true;
  }

  bool writeSuper() {
    uint64_t sb = ext4::SB_OFFSET;
    bool ok = cache.write32(sb + 0x0C, (uint32_t)free_blocks) &&
              cache.write32(sb + 0x10, free_inodes) &&
              cache.write32(sb + 0x30, now()) && cache.write16(sb + 0x3A, state);
    if (ok && (incompat & ext4::INCOMPAT_64BIT))
      ok = cache.write32(sb + 0x158, (uint32_t)(free_blocks >> 32));
    if (ok && hasCsum()) {
      uint32_t crc = 0xFFFFFFFF;
      ok = cache.crc32c(sb, ext4::SB_CSUM_OFFSET, crc) &&
           cache.write32(sb + ext4::SB_CSUM_OFFSET, crc);
    }
    if (!ok) return fail(Error::IO);
    sb_dirty = false;
    return true;
  }

  /// called before the first change: marks the file system as not clean
  bool beginWrite() {
    if (!mounted) return fail(Error::NotMounted);
    if (read_only) return fail(Error::ReadOnly);
    if (fs_dirty) return true;
    fs_dirty = true;
    state &= ~ext4::STATE_VALID;
    if (!writeSuper() || !cache.flush()) return fail(Error::IO);
    p_io->disk_ioctl(0, CTRL_SYNC, nullptr);
    return true;
  }

  // ------------------------------------------------------------------
  // bitmaps and allocation
  // ------------------------------------------------------------------

  /// sets (value=true) or clears bits; returns the number of changed bits
  bool changeBits(uint64_t bmOff, uint32_t bit, uint32_t count, bool value,
                  uint32_t& changed) {
    changed = 0;
    while (count > 0) {
      uint64_t off = bmOff + bit / 8;
      uint8_t b;
      if ((bit & 7) == 0 && count >= 8) {
        // whole bytes
        uint8_t buf[32];
        uint32_t n = count / 8 > sizeof(buf) ? sizeof(buf) : count / 8;
        if (!cache.read(off, buf, n)) return fail(Error::IO);
        for (uint32_t j = 0; j < n; j++) {
          uint8_t nb = value ? 0xFF : 0;
          for (int k = 0; k < 8; k++) changed += ((buf[j] >> k) & 1) != value;
          buf[j] = nb;
        }
        if (!cache.write(off, buf, n)) return fail(Error::IO);
        bit += n * 8;
        count -= n * 8;
        continue;
      }
      if (!cache.read(off, &b, 1)) return fail(Error::IO);
      uint8_t mask = (uint8_t)(1 << (bit & 7));
      if (((b & mask) != 0) != value) {
        changed++;
        b = value ? (b | mask) : (b & ~mask);
        if (!cache.write(off, &b, 1)) return fail(Error::IO);
      }
      bit++;
      count--;
    }
    return true;
  }

  bool setBits(uint64_t bmOff, uint32_t bit, uint32_t count) {
    uint32_t changed;
    return changeBits(bmOff, bit, count, true, changed);
  }

  /// finds the first zero bit in [from, to)
  bool findZero(uint64_t bmOff, uint32_t from, uint32_t to, uint32_t& bit) {
    uint8_t buf[32];
    uint32_t j = from;
    while (j < to) {
      uint32_t byte = j / 8;
      uint32_t n = (to + 7) / 8 - byte;
      if (n > sizeof(buf)) n = sizeof(buf);
      if (!cache.read(bmOff + byte, buf, n)) return fail(Error::IO);
      for (uint32_t k = 0; k < n && j < to; k++) {
        if (buf[k] == 0xFF) {
          j = (byte + k + 1) * 8;
          continue;
        }
        for (; j < (byte + k + 1) * 8 && j < to; j++) {
          if (!(buf[k] & (1 << (j & 7)))) {
            bit = j;
            return true;
          }
        }
      }
    }
    return false;
  }

  /// creates the block bitmap of a BLOCK_UNINIT group
  bool initBlockBitmap(uint32_t g, const GroupDesc& d) {
    if (incompat & ext4::INCOMPAT_META_BG) return fail(Error::Unsupported);
    uint64_t bm = blockOff(d.blockBitmap);
    uint32_t n = blocksInGroup(g);
    if (!cache.fill(bm, 0, block_size)) return fail(Error::IO);
    if (hasSuper(g)) {
      uint32_t per = block_size / desc_size;
      uint32_t gdt = (group_count + per - 1) / per;
      if (!setBits(bm, 0, 1 + gdt + reserved_gdt)) return false;
    }
    // bitmaps and inode tables (flex_bg: of any group) located in this group
    uint64_t first = groupFirstBlock(g);
    uint64_t last = first + n;
    uint32_t itBlocks =
        (uint32_t)(((uint64_t)inodes_per_group * inode_size + block_size - 1) / block_size);
    for (uint32_t h = 0; h < group_count; h++) {
      GroupDesc o;
      if (!readDesc(h, o)) return false;
      uint64_t starts[3] = {o.blockBitmap, o.inodeBitmap, o.inodeTable};
      uint32_t lens[3] = {1, 1, itBlocks};
      for (int k = 0; k < 3; k++) {
        uint64_t s = starts[k], e = starts[k] + lens[k];
        if (e <= first || s >= last) continue;
        if (s < first) s = first;
        if (e > last) e = last;
        if (!setBits(bm, (uint32_t)(s - first), (uint32_t)(e - s))) return false;
      }
    }
    return setBits(bm, n, block_size * 8 - n);
  }

  bool initInodeBitmap(const GroupDesc& d) {
    uint64_t bm = blockOff(d.inodeBitmap);
    if (!cache.fill(bm, 0, block_size)) return fail(Error::IO);
    return setBits(bm, inodes_per_group, block_size * 8 - inodes_per_group);
  }

  /// allocates one block, preferably at goal
  bool allocBlock(uint64_t goal, uint64_t& out) {
    if (free_blocks == 0) return fail(Error::NoSpace);
    uint32_t g0 = 0, startBit = 0;
    if (goal >= first_data_block && goal < blocks_count) {
      g0 = (uint32_t)((goal - first_data_block) / blocks_per_group);
      startBit = (uint32_t)((goal - first_data_block) % blocks_per_group);
    }
    for (uint32_t i = 0; i <= group_count; i++) {
      uint32_t g = (g0 + i) % group_count;
      GroupDesc d;
      if (!readDesc(g, d)) return false;
      if (d.freeBlocks == 0) continue;
      if (d.flags & ext4::BG_BLOCK_UNINIT) {
        if (!initBlockBitmap(g, d)) {
          if (error == Error::Unsupported) continue;
          return false;
        }
        d.flags &= ~ext4::BG_BLOCK_UNINIT;
      }
      uint32_t bit;
      uint64_t bm = blockOff(d.blockBitmap);
      if (!findZero(bm, i == 0 ? startBit : 0, blocksInGroup(g), bit)) {
        if (error == Error::IO) return false;
        continue;
      }
      if (!setBits(bm, bit, 1)) return false;
      d.freeBlocks--;
      if (!writeDesc(g, d) || !markGroup(g, DIRTY_BLOCKS)) return false;
      free_blocks--;
      out = groupFirstBlock(g) + bit;
      return true;
    }
    return fail(Error::NoSpace);
  }

  /// releases a range of blocks
  bool freeBlockRange(uint64_t start, uint64_t count) {
    while (count > 0) {
      if (start < first_data_block || start + count > blocks_count)
        return fail(Error::Corrupt);
      uint32_t g = (uint32_t)((start - first_data_block) / blocks_per_group);
      uint32_t bit = (uint32_t)((start - first_data_block) % blocks_per_group);
      uint32_t n = blocks_per_group - bit;
      if (n > count) n = (uint32_t)count;
      GroupDesc d;
      if (!readDesc(g, d)) return false;
      if (d.flags & ext4::BG_BLOCK_UNINIT) return fail(Error::Corrupt);
      uint32_t changed;
      if (!changeBits(blockOff(d.blockBitmap), bit, n, false, changed)) return false;
      d.freeBlocks += changed;
      if (!writeDesc(g, d) || !markGroup(g, DIRTY_BLOCKS)) return false;
      free_blocks += changed;
      start += n;
      count -= n;
    }
    return true;
  }

  /// frees blocks owned by an inode and updates i_blocks
  bool freeInodeBlocks(Inode& in, uint64_t start, uint64_t count) {
    for (int j = 0; j < pending_ext_count; j++) {
      // never write a checksum into a released extent block
      if (pending_ext[j].blk >= start && pending_ext[j].blk < start + count) {
        pending_ext[j] = pending_ext[--pending_ext_count];
        j--;
      }
    }
    uint64_t sectors = count * sectorsPerBlock();
    in.blocks = in.blocks > sectors ? in.blocks - sectors : 0;
    return freeBlockRange(start, count);
  }

  bool allocInode(uint32_t hint, bool isDir, uint32_t& ino) {
    if (free_inodes == 0) return fail(Error::NoSpace);
    for (uint32_t i = 0; i < group_count; i++) {
      uint32_t g = (hint + i) % group_count;
      GroupDesc d;
      if (!readDesc(g, d)) return false;
      if (d.freeInodes == 0) continue;
      if (d.flags & ext4::BG_BLOCK_UNINIT) {
        // like the kernel: initialize the block bitmap of the group as well
        if (!initBlockBitmap(g, d)) {
          if (error == Error::Unsupported) continue;
          return false;
        }
        d.flags &= ~ext4::BG_BLOCK_UNINIT;
        if (!markGroup(g, DIRTY_BLOCKS)) return false;
      }
      if (d.flags & ext4::BG_INODE_UNINIT) {
        if (!initInodeBitmap(d)) return false;
        d.flags &= ~ext4::BG_INODE_UNINIT;
      }
      uint32_t bit;
      uint64_t bm = blockOff(d.inodeBitmap);
      uint32_t from = g == 0 ? first_ino - 1 : 0;
      if (!findZero(bm, from, inodes_per_group, bit)) {
        if (error == Error::IO) return false;
        continue;
      }
      if (!setBits(bm, bit, 1)) return false;
      d.freeInodes--;
      if (isDir) d.usedDirs++;
      if (hasGroupCsum() && bit >= inodes_per_group - d.itableUnused)
        d.itableUnused = inodes_per_group - bit - 1;
      if (!writeDesc(g, d) || !markGroup(g, DIRTY_INODES)) return false;
      free_inodes--;
      ino = g * inodes_per_group + bit + 1;
      return true;
    }
    return fail(Error::NoSpace);
  }

  bool freeInode(uint32_t ino, bool isDir) {
    uint32_t g = (ino - 1) / inodes_per_group;
    uint32_t bit = (ino - 1) % inodes_per_group;
    GroupDesc d;
    if (!readDesc(g, d)) return false;
    uint32_t changed;
    if (!changeBits(blockOff(d.inodeBitmap), bit, 1, false, changed)) return false;
    d.freeInodes += changed;
    if (isDir && changed && d.usedDirs > 0) d.usedDirs--;
    if (!writeDesc(g, d) || !markGroup(g, DIRTY_INODES)) return false;
    free_inodes += changed;
    return true;
  }
#endif  // TINYEXT4_WRITE

  // ------------------------------------------------------------------
  // inodes
  // ------------------------------------------------------------------

  bool inodeOffset(uint32_t ino, uint64_t& off) {
    if (ino == 0 || ino > inodes_count) return fail(Error::Corrupt);
    GroupDesc d;
    if (!readDesc((ino - 1) / inodes_per_group, d)) return false;
    off = blockOff(d.inodeTable) + (uint64_t)((ino - 1) % inodes_per_group) * inode_size;
    return true;
  }

  bool readInode(uint32_t ino, Inode& in) {
    uint8_t raw[0xA0] = {0};
    uint64_t off;
    if (!inodeOffset(ino, off)) return false;
    if (!cache.read(off, raw, inode_size < sizeof(raw) ? inode_size : sizeof(raw)))
      return fail(Error::IO);
    in.ino = ino;
    in.mode = get16(raw + 0x00);
    in.uid = get16(raw + 0x02) | ((uint32_t)get16(raw + 0x78) << 16);
    in.size = get32(raw + 0x04) | ((uint64_t)get32(raw + 0x6C) << 32);
    in.atime = get32(raw + 0x08);
    in.ctime = get32(raw + 0x0C);
    in.mtime = get32(raw + 0x10);
    in.dtime = get32(raw + 0x14);
    in.gid = get16(raw + 0x18) | ((uint32_t)get16(raw + 0x7A) << 16);
    in.links = get16(raw + 0x1A);
    in.blocks = get32(raw + 0x1C);
    if (ro_compat & ext4::RO_COMPAT_HUGE_FILE) in.blocks |= (uint64_t)get16(raw + 0x74) << 32;
    in.flags = get32(raw + 0x20);
    if (in.flags & ext4::FL_HUGE_FILE) in.blocks <<= (log_block - 9);
    memcpy(in.block, raw + 0x28, 60);
    in.generation = get32(raw + 0x64);
    in.fileAcl = get32(raw + 0x68) | ((uint64_t)get16(raw + 0x76) << 32);
    return true;
  }

#if TINYEXT4_WRITE
  bool writeInode(Inode& in) {
    uint8_t raw[0xA0] = {0};
    uint64_t off;
    size_t n = inode_size < sizeof(raw) ? inode_size : sizeof(raw);
    if (!inodeOffset(in.ino, off)) return false;
    if (!cache.read(off, raw, n)) return fail(Error::IO);
    put16(raw + 0x00, in.mode);
    put16(raw + 0x02, (uint16_t)in.uid);
    put32(raw + 0x04, (uint32_t)in.size);
    put32(raw + 0x08, in.atime);
    put32(raw + 0x0C, in.ctime);
    put32(raw + 0x10, in.mtime);
    put32(raw + 0x14, in.dtime);
    put16(raw + 0x18, (uint16_t)in.gid);
    put16(raw + 0x1A, in.links);
    uint64_t blocks = in.blocks;
    if (in.flags & ext4::FL_HUGE_FILE) blocks >>= (log_block - 9);
    put32(raw + 0x1C, (uint32_t)blocks);
    if (ro_compat & ext4::RO_COMPAT_HUGE_FILE) put16(raw + 0x74, (uint16_t)(blocks >> 32));
    put32(raw + 0x20, in.flags);
    memcpy(raw + 0x28, in.block, 60);
    put32(raw + 0x64, in.generation);
    put32(raw + 0x68, (uint32_t)in.fileAcl);
    put32(raw + 0x6C, (uint32_t)(in.size >> 32));
    put16(raw + 0x76, (uint16_t)(in.fileAcl >> 32));
    put16(raw + 0x78, (uint16_t)(in.uid >> 16));
    put16(raw + 0x7A, (uint16_t)(in.gid >> 16));
    if (!cache.write(off, raw, n)) return fail(Error::IO);
    return inodeChecksum(in.ino, in.generation, off);
  }

  bool inodeChecksum(uint32_t ino, uint32_t gen, uint64_t off) {
    if (!hasCsum()) return true;
    bool hasHi = false;
    if (inode_size > 128) {
      uint8_t e[2];
      if (!cache.read(off + 0x80, e, 2)) return fail(Error::IO);
      hasHi = get16(e) >= 4;
    }
    uint32_t crc = crc32c_u32(crc32c_u32(csum_seed, ino), gen);
    uint8_t buf[32];
    for (uint32_t pos = 0; pos < inode_size; pos += sizeof(buf)) {
      if (!cache.read(off + pos, buf, sizeof(buf))) return fail(Error::IO);
      for (uint32_t k = 0; k < sizeof(buf); k++) {
        uint32_t a = pos + k;
        if (a == 0x7C || a == 0x7D || (hasHi && (a == 0x82 || a == 0x83))) buf[k] = 0;
      }
      crc = crc32c(crc, buf, sizeof(buf));
    }
    if (!hasHi) crc &= 0xFFFF;
    if (!cache.write16(off + 0x7C, (uint16_t)crc)) return fail(Error::IO);
    if (hasHi && !cache.write16(off + 0x82, (uint16_t)(crc >> 16))) return fail(Error::IO);
    return true;
  }

  /// initializes a newly allocated inode with an empty extent tree
  bool initInode(uint32_t ino, uint16_t mode, Inode& in) {
    uint64_t off;
    if (!inodeOffset(ino, off)) return false;
    if (!cache.fill(off, 0, inode_size)) return fail(Error::IO);
    uint32_t t = now();
    if (inode_size >= 0x98) {
      // i_extra_isize and creation time
      if (!cache.write16(off + 0x80, 32) || !cache.write32(off + 0x90, t))
        return fail(Error::IO);
    }
    in = Inode();
    in.ino = ino;
    in.mode = mode;
    in.uid = new_uid;
    in.gid = new_gid;
    in.atime = in.ctime = in.mtime = t;
    gen_counter = gen_counter * 1103515245u + 12345u + t;
    in.generation = gen_counter ^ ino;
    in.flags = ext4::FL_EXTENTS;
    put16(in.block + 0, ext4::EXT_MAGIC);
    put16(in.block + 2, 0);
    put16(in.block + 4, 4);
    put16(in.block + 6, 0);
    return writeInode(in);
  }

  uint32_t inodeSeed(const Inode& in) const {
    return crc32c_u32(crc32c_u32(csum_seed, in.ino), in.generation);
  }
#endif

  // ------------------------------------------------------------------
  // block mapping
  // ------------------------------------------------------------------

  bool nodeGet(const Node& n, uint32_t idx, uint8_t* e) {
    if (n.mem) {
      memcpy(e, n.mem + 12 * idx, 12);
      return true;
    }
    if (!cache.read(blockOff(n.blk) + 12 * idx, e, 12)) return fail(Error::IO);
    return true;
  }

  static uint64_t extStart(const uint8_t* e) {
    return ((uint64_t)get16(e + 6) << 32) | get32(e + 8);
  }
  static uint64_t idxLeaf(const uint8_t* e) {
    return get32(e + 4) | ((uint64_t)get16(e + 8) << 32);
  }

  /**
   * @brief Maps a logical block: pblk = 0 for holes. run is the number of
   * following blocks with the same state.
   */
  bool mapBlock(Inode& in, uint32_t lblk, uint64_t& pblk, uint32_t& run,
                bool* uninit = nullptr) {
    if (uninit) *uninit = false;
    pblk = 0;
    run = 1;
    if (!in.hasExtents()) return mapIndirect(in, lblk, pblk);
    Node n{0, in.block};
    uint8_t h[12], e[12];
    if (!nodeGet(n, 0, h)) return false;
    for (int level = 0; level < 8; level++) {
      if (get16(h) != ext4::EXT_MAGIC) return fail(Error::Corrupt);
      uint16_t entries = get16(h + 2);
      if (get16(h + 6) == 0) {
        for (uint32_t i = 1; i <= entries; i++) {
          if (!nodeGet(n, i, e)) return false;
          uint32_t start = get32(e);
          uint32_t len = get16(e + 4);
          bool un = len > ext4::EXT_INIT_MAX_LEN;
          if (un) len -= ext4::EXT_INIT_MAX_LEN;
          if (lblk < start) {
            run = start - lblk;
            return true;
          }
          if (lblk < start + len) {
            run = start + len - lblk;
            if (un) {
              if (uninit) *uninit = true;
            } else {
              pblk = extStart(e) + (lblk - start);
            }
            return true;
          }
        }
        run = 0xFFFFFFFF - lblk;
        return true;
      }
      uint64_t child = 0;
      uint32_t next = 0xFFFFFFFF;
      for (uint32_t i = 1; i <= entries; i++) {
        if (!nodeGet(n, i, e)) return false;
        if (get32(e) > lblk) {
          next = get32(e);
          break;
        }
        child = idxLeaf(e);
      }
      if (child == 0) {
        run = next - lblk;
        return true;
      }
      n = Node{child, nullptr};
      if (!nodeGet(n, 0, h)) return false;
    }
    return fail(Error::Corrupt);
  }

  bool mapIndirect(Inode& in, uint32_t lblk, uint64_t& pblk) {
    uint32_t apb = block_size / 4;
    if (lblk < 12) {
      pblk = get32(in.block + 4 * lblk);
      return true;
    }
    lblk -= 12;
    int level, slot;
    if (lblk < apb) {
      level = 1;
      slot = 12;
    } else if ((lblk -= apb) < (uint64_t)apb * apb) {
      level = 2;
      slot = 13;
    } else {
      lblk -= apb * apb;
      level = 3;
      slot = 14;
    }
    uint32_t ptr = get32(in.block + 4 * slot);
    while (level > 0 && ptr != 0) {
      uint32_t div = 1;
      for (int j = 1; j < level; j++) div *= apb;
      if (!cache.read32(blockOff(ptr) + 4 * (lblk / div), ptr)) return fail(Error::IO);
      lblk %= div;
      level--;
    }
    pblk = ptr;
    return true;
  }

  bool hasDataBlocks(const Inode& in) const {
    if (in.isInline()) return false;
    if (in.isReg() || in.isDir()) return true;
    // fast symlinks store the target in i_block
    if (in.isSymlink()) return in.hasExtents() || in.size >= 60;
    return false;
  }

  /// mapBlock with a cache of the last result
  bool cachedMap(Inode& in, MapCache& m, uint32_t lblk, uint64_t& pblk, uint32_t& run) {
    if (m.len > 0 && lblk >= m.lblk && lblk - m.lblk < m.len) {
      uint32_t d = lblk - m.lblk;
      pblk = m.pblk ? m.pblk + d : 0;
      run = m.len - d;
      return true;
    }
    if (!mapBlock(in, lblk, pblk, run)) return false;
    m.lblk = lblk;
    m.len = run;
    m.pblk = pblk;
    return true;
  }

  // ------------------------------------------------------------------
  // extent tree changes
  // ------------------------------------------------------------------

#if TINYEXT4_WRITE
  uint16_t blockMaxEntries() const { return (uint16_t)((block_size - 12) / 12); }

  bool computeExtentCsum(uint64_t blk, uint32_t seed) {
    uint8_t h[12];
    uint64_t off = blockOff(blk);
    if (!cache.read(off, h, 12)) return fail(Error::IO);
    uint32_t tail = 12 + 12 * (uint32_t)get16(h + 4);
    if (tail + 4 > block_size) return fail(Error::Corrupt);
    uint32_t crc = seed;
    if (!cache.crc32c(off, tail, crc) || !cache.write32(off + tail, crc))
      return fail(Error::IO);
    return true;
  }

  /// extent block checksums are calculated lazily
  bool markExtentBlock(const Inode& in, uint64_t blk) {
    if (!hasCsum()) return true;
    uint32_t seed = inodeSeed(in);
    for (int j = 0; j < pending_ext_count; j++) {
      if (pending_ext[j].blk == blk) return true;
    }
    if (pending_ext_count == 4) {
      if (!computeExtentCsum(pending_ext[0].blk, pending_ext[0].seed)) return false;
      memmove(pending_ext, pending_ext + 1, 3 * sizeof(PendingCsum));
      pending_ext_count--;
    }
    pending_ext[pending_ext_count++] = {blk, seed};
    return true;
  }

  bool flushExtentCsums() {
    bool ok = true;
    for (int j = 0; j < pending_ext_count; j++)
      ok = computeExtentCsum(pending_ext[j].blk, pending_ext[j].seed) && ok;
    pending_ext_count = 0;
    return ok;
  }

  bool nodePut(const Inode& in, const Node& n, uint32_t idx, const uint8_t* e) {
    if (n.mem) {
      memcpy(n.mem + 12 * idx, e, 12);
      return true;
    }
    if (!cache.write(blockOff(n.blk) + 12 * idx, e, 12)) return fail(Error::IO);
    return markExtentBlock(in, n.blk);
  }

  static void makeHeader(uint8_t* h, uint16_t entries, uint16_t max, uint16_t depth) {
    put16(h, ext4::EXT_MAGIC);
    put16(h + 2, entries);
    put16(h + 4, max);
    put16(h + 6, depth);
    put32(h + 8, 0);
  }
  static void makeExtent(uint8_t* e, uint32_t lblk, uint16_t len, uint64_t pblk) {
    put32(e, lblk);
    put16(e + 4, len);
    put16(e + 6, (uint16_t)(pblk >> 32));
    put32(e + 8, (uint32_t)pblk);
  }
  static void makeIndex(uint8_t* e, uint32_t lblk, uint64_t leaf) {
    put32(e, lblk);
    put32(e + 4, (uint32_t)leaf);
    put16(e + 8, (uint16_t)(leaf >> 32));
    put16(e + 10, 0);
  }

  /// allocates and initializes an empty extent tree block
  bool newTreeBlock(Inode& in, uint64_t goal, uint16_t depth, uint64_t& blk) {
    if (!allocBlock(goal, blk)) return false;
    in.blocks += sectorsPerBlock();
    uint8_t h[12];
    makeHeader(h, 0, blockMaxEntries(), depth);
    if (!cache.fill(blockOff(blk), 0, block_size) || !cache.write(blockOff(blk), h, 12))
      return fail(Error::IO);
    return markExtentBlock(in, blk);
  }

  /// adds the mapping lblk -> pblk after the last extent of the file
  bool appendExtent(Inode& in, uint32_t lblk, uint64_t pblk) {
    for (int attempt = 0; attempt < 2; attempt++) {
      Node path[8];
      uint8_t hdr[8][12];
      int lvl = 0;
      path[0] = Node{0, in.block};
      if (!nodeGet(path[0], 0, hdr[0])) return false;
      while (get16(hdr[lvl] + 6) > 0) {
        if (get16(hdr[lvl]) != ext4::EXT_MAGIC || lvl == 7) return fail(Error::Corrupt);
        uint8_t e[12];
        uint16_t entries = get16(hdr[lvl] + 2);
        if (entries == 0) return fail(Error::Corrupt);
        if (!nodeGet(path[lvl], entries, e)) return false;
        path[lvl + 1] = Node{idxLeaf(e), nullptr};
        lvl++;
        if (!nodeGet(path[lvl], 0, hdr[lvl])) return false;
      }
      if (get16(hdr[lvl]) != ext4::EXT_MAGIC) return fail(Error::Corrupt);
      Node& leaf = path[lvl];
      uint16_t entries = get16(hdr[lvl] + 2);
      uint16_t max = get16(hdr[lvl] + 4);
      uint8_t e[12];
      if (entries > 0) {
        if (!nodeGet(leaf, entries, e)) return false;
        uint32_t start = get32(e);
        uint16_t len = get16(e + 4);
        if (len > ext4::EXT_INIT_MAX_LEN) {
          if (lblk < start + len - ext4::EXT_INIT_MAX_LEN) return fail(Error::Unsupported);
        } else {
          if (lblk < start + len) return fail(Error::Unsupported);
          if (start + len == lblk && extStart(e) + len == pblk &&
              len < ext4::EXT_INIT_MAX_LEN) {
            put16(e + 4, len + 1);
            return nodePut(in, leaf, entries, e);
          }
        }
      }
      makeExtent(e, lblk, 1, pblk);
      if (entries < max) {
        put16(hdr[lvl] + 2, entries + 1);
        return nodePut(in, leaf, entries + 1, e) && nodePut(in, leaf, 0, hdr[lvl]);
      }
      // leaf is full: find the deepest index node with a free slot
      int k = lvl - 1;
      while (k >= 0 && get16(hdr[k] + 2) >= get16(hdr[k] + 4)) k--;
      if (k < 0) {
        if (!growTree(in, pblk)) return false;
        continue;  // retry with the new root
      }
      // new chain of nodes below level k, bottom up
      uint64_t child;
      if (!newTreeBlock(in, pblk + 1, 0, child)) return false;
      Node n{child, nullptr};
      uint8_t h[12];
      makeHeader(h, 1, blockMaxEntries(), 0);
      if (!nodePut(in, n, 1, e) || !nodePut(in, n, 0, h)) return false;
      for (int level = lvl - 1; level > k; level--) {
        uint64_t parent;
        if (!newTreeBlock(in, pblk + 1, (uint16_t)(lvl - level), parent)) return false;
        Node p{parent, nullptr};
        uint8_t ie[12];
        makeIndex(ie, lblk, child);
        makeHeader(h, 1, blockMaxEntries(), (uint16_t)(lvl - level));
        if (!nodePut(in, p, 1, ie) || !nodePut(in, p, 0, h)) return false;
        child = parent;
      }
      uint8_t ie[12];
      makeIndex(ie, lblk, child);
      uint16_t ke = get16(hdr[k] + 2) + 1;
      put16(hdr[k] + 2, ke);
      return nodePut(in, path[k], ke, ie) && nodePut(in, path[k], 0, hdr[k]);
    }
    return fail(Error::Corrupt);
  }

  /// moves the root of the extent tree into a new block (depth + 1)
  bool growTree(Inode& in, uint64_t goal) {
    uint8_t h[12];
    memcpy(h, in.block, 12);
    uint16_t depth = get16(h + 6);
    uint16_t entries = get16(h + 2);
    if (depth >= 4) return fail(Error::Unsupported);
    uint64_t blk;
    if (!newTreeBlock(in, goal, depth, blk)) return false;
    uint64_t off = blockOff(blk);
    makeHeader(h, entries, blockMaxEntries(), depth);
    if (!cache.write(off, h, 12) || !cache.write(off + 12, in.block + 12, 12 * entries))
      return fail(Error::IO);
    uint8_t ie[12];
    makeIndex(ie, entries > 0 ? get32(in.block + 12) : 0, blk);
    makeHeader(h, 1, 4, depth + 1);
    memset(in.block, 0, 60);
    memcpy(in.block, h, 12);
    memcpy(in.block + 12, ie, 12);
    return true;
  }

  /// removes all blocks >= cutoff below node n
  bool truncNode(Inode& in, const Node& n, uint32_t cutoff, bool& empty) {
    uint8_t h[12], e[12];
    if (!nodeGet(n, 0, h)) return false;
    if (get16(h) != ext4::EXT_MAGIC) return fail(Error::Corrupt);
    uint16_t entries = get16(h + 2);
    uint16_t old = entries;
    if (get16(h + 6) == 0) {
      while (entries > 0) {
        if (!nodeGet(n, entries, e)) return false;
        uint32_t start = get32(e);
        uint16_t raw = get16(e + 4);
        bool un = raw > ext4::EXT_INIT_MAX_LEN;
        uint32_t len = un ? raw - ext4::EXT_INIT_MAX_LEN : raw;
        if (start >= cutoff) {
          if (!freeInodeBlocks(in, extStart(e), len)) return false;
          entries--;
          continue;
        }
        if (start + len > cutoff) {
          uint32_t keep = cutoff - start;
          if (!freeInodeBlocks(in, extStart(e) + keep, len - keep)) return false;
          put16(e + 4, (uint16_t)(un ? keep + ext4::EXT_INIT_MAX_LEN : keep));
          if (!nodePut(in, n, entries, e)) return false;
        }
        break;
      }
    } else {
      while (entries > 0) {
        if (!nodeGet(n, entries, e)) return false;
        uint32_t start = get32(e);
        uint64_t child = idxLeaf(e);
        bool childEmpty = false;
        if (!truncNode(in, Node{child, nullptr}, cutoff, childEmpty)) return false;
        if (childEmpty) {
          if (!freeInodeBlocks(in, child, 1)) return false;
          entries--;
          if (start >= cutoff) continue;
        }
        break;
      }
    }
    if (entries != old) {
      put16(h + 2, entries);
      if (!nodePut(in, n, 0, h)) return false;
    }
    empty = entries == 0;
    return true;
  }

  bool freeIndirect(Inode& in, uint32_t blk, int level) {
    if (blk == 0) return true;
    if (level > 0) {
      uint32_t apb = block_size / 4;
      for (uint32_t j = 0; j < apb; j++) {
        uint32_t child;
        if (!cache.read32(blockOff(blk) + 4 * j, child)) return fail(Error::IO);
        if (!freeIndirect(in, child, level - 1)) return false;
      }
    }
    return freeInodeBlocks(in, blk, 1);
  }

  /// changes the size of an inode (does not write the inode)
  bool truncateInode(Inode& in, uint64_t size) {
    if (in.isInline()) {
      if (size > in.size || size > 60) return fail(Error::Unsupported);
      memset(in.block + size, 0, 60 - size);
      in.size = size;
      return true;
    }
    if (hasDataBlocks(in)) {
      uint32_t cutoff = (uint32_t)((size + block_size - 1) >> log_block);
      if (in.hasExtents()) {
        bool empty;
        if (!truncNode(in, Node{0, in.block}, cutoff, empty)) return false;
        if (empty) makeHeader(in.block, 0, 4, 0);
      } else {
        if (size != 0) return fail(Error::Unsupported);
        for (int j = 0; j < 12; j++)
          if (!freeIndirect(in, get32(in.block + 4 * j), 0)) return false;
        for (int j = 12; j < 15; j++)
          if (!freeIndirect(in, get32(in.block + 4 * j), j - 11)) return false;
        memset(in.block, 0, 60);
      }
      // zero the rest of the last block, so that growing the file reads zeros
      uint32_t rest = (uint32_t)(size & (block_size - 1));
      if (rest > 0 && size < in.size) {
        uint64_t pblk;
        uint32_t run;
        if (!mapBlock(in, (uint32_t)(size >> log_block), pblk, run)) return false;
        if (pblk && !cache.fill(blockOff(pblk) + rest, 0, block_size - rest))
          return fail(Error::IO);
      }
    }
    in.size = size;
    return true;
  }

  /// releases the extended attribute block of a deleted inode
  bool releaseXattrBlock(Inode& in) {
    if (in.fileAcl == 0) return true;
    uint64_t off = blockOff(in.fileAcl);
    uint8_t h[8];
    if (!cache.read(off, h, 8)) return fail(Error::IO);
    uint32_t refs = get32(h + 4);
    if (get32(h) == 0xEA020000 && refs > 1) {
      if (!cache.write32(off + 4, refs - 1)) return fail(Error::IO);
      if (hasCsum()) {
        uint8_t nr[8];
        put32(nr, (uint32_t)in.fileAcl);
        put32(nr + 4, (uint32_t)(in.fileAcl >> 32));
        uint32_t crc = crc32c(csum_seed, nr, 8);
        uint8_t zero[4] = {0, 0, 0, 0};
        if (!cache.crc32c(off, 0x10, crc)) return fail(Error::IO);
        crc = crc32c(crc, zero, 4);
        if (!cache.crc32c(off + 0x14, block_size - 0x14, crc) ||
            !cache.write32(off + 0x10, crc))
          return fail(Error::IO);
      }
    } else if (!freeInodeBlocks(in, in.fileAcl, 1)) {
      return false;
    }
    in.fileAcl = 0;
    return true;
  }

  /// frees the blocks and the inode of a file or directory without links
  bool deleteInode(Inode& in) {
    if (!truncateInode(in, 0) || !releaseXattrBlock(in)) return false;
    in.links = 0;
    in.dtime = now();
    if (in.dtime == 0) in.dtime = 1;  // e2fsck: deleted inodes need a dtime
    return writeInode(in) && freeInode(in.ino, in.isDir());
  }
#endif  // TINYEXT4_WRITE

  // ------------------------------------------------------------------
  // directories
  // ------------------------------------------------------------------

  static uint32_t dirRecLen(uint32_t nameLen) { return (8 + nameLen + 3) & ~3u; }
  uint32_t decodeRecLen(uint16_t v) const {
    if (block_size < 65536) return v;
    if (v == 65535 || v == 0) return block_size;
    return (v & 65532) | ((v & 3) << 16);
  }
  uint16_t encodeRecLen(uint32_t len) const {
    if (block_size < 65536) return (uint16_t)len;
    if (len == 65536) return 65535;
    return (uint16_t)((len & 65532) | ((len >> 16) & 3));
  }

  /// next raw entry of a directory (including unused ones)
  bool dirNextRaw(Inode& dir, DirIter& it, RawDirent& d) {
    while (it.pos < dir.size) {
      uint32_t lblk = (uint32_t)(it.pos >> log_block);
      uint32_t off = (uint32_t)(it.pos & (block_size - 1));
      uint64_t pblk;
      uint32_t run;
      if (!cachedMap(dir, it.map, lblk, pblk, run)) return false;
      if (pblk == 0) {
        it.pos = (uint64_t)(lblk + 1) << log_block;
        continue;
      }
      uint8_t h[8];
      d.dev = blockOff(pblk) + off;
      if (!cache.read(d.dev, h, 8)) return fail(Error::IO);
      d.inode = get32(h);
      d.recLen = decodeRecLen(get16(h + 4));
      d.nameLen = h[6];
      d.type = hasFiletype() ? h[7] : 0;
      if (d.recLen < 8 || (d.recLen & 3) || off + d.recLen > block_size ||
          d.nameLen + 8u > d.recLen) {
        // damaged block: skip it
        it.pos = (uint64_t)(lblk + 1) << log_block;
        continue;
      }
      it.pos += d.recLen;
      return true;
    }
    return false;
  }

  bool nameEquals(const RawDirent& d, const char* name, size_t len) {
    if (d.inode == 0 || d.nameLen != len) return false;
    uint8_t buf[32];
    for (size_t pos = 0; pos < len; pos += sizeof(buf)) {
      size_t n = len - pos < sizeof(buf) ? len - pos : sizeof(buf);
      if (!cache.read(d.dev + 8 + pos, buf, n)) return false;
      if (memcmp(buf, name + pos, n) != 0) return false;
    }
    return true;
  }

  bool lookup(Inode& dir, const char* name, size_t len, uint32_t& ino,
              uint8_t* type = nullptr) {
    if (!dir.isDir()) return fail(Error::NotDir);
    if (dir.isInline()) return fail(Error::Unsupported);
    DirIter it;
    RawDirent d;
    while (dirNextRaw(dir, it, d)) {
      if (nameEquals(d, name, len)) {
        ino = d.inode;
        if (type) *type = d.type;
        return true;
      }
    }
    if (error == Error::IO) return false;
    return fail(Error::NotFound);
  }

  /// resolves the first len characters of path
  bool resolve(const char* path, size_t len, Inode& out) {
    if (!mounted) return fail(Error::NotMounted);
    if (!readInode(ext4::ROOT_INO, out)) return false;
    size_t pos = 0;
    while (pos < len) {
      while (pos < len && path[pos] == '/') pos++;
      size_t start = pos;
      while (pos < len && path[pos] != '/') pos++;
      size_t n = pos - start;
      if (n == 0) break;
      if (n > 255) return fail(Error::Invalid);
      uint32_t ino;
      if (!lookup(out, path + start, n, ino)) return false;
      if (!readInode(ino, out)) return false;
    }
    return true;
  }

  /// resolves the parent directory and returns the last path component
  bool resolveParent(const char* path, Inode& parent, const char*& name,
                     uint8_t& nameLen) {
    size_t len = strlen(path);
    while (len > 0 && path[len - 1] == '/') len--;
    size_t start = len;
    while (start > 0 && path[start - 1] != '/') start--;
    size_t n = len - start;
    if (n == 0 || n > 255 || (n == 1 && path[start] == '.') ||
        (n == 2 && path[start] == '.' && path[start + 1] == '.'))
      return fail(Error::Invalid);
    name = path + start;
    nameLen = (uint8_t)n;
    if (!resolve(path, start, parent)) return false;
    if (!parent.isDir()) return fail(Error::NotDir);
    return true;
  }

  static FileType modeToType(uint16_t mode) {
    switch (mode & ext4::MODE_FMT) {
      case 0x8000: return FileType::Regular;
      case 0x4000: return FileType::Directory;
      case 0x2000: return FileType::CharDevice;
      case 0x6000: return FileType::BlockDevice;
      case 0x1000: return FileType::Fifo;
      case 0xC000: return FileType::Socket;
      case 0xA000: return FileType::Symlink;
    }
    return FileType::Unknown;
  }

  /// next entry for File::readDir(): skips "." and ".."
  bool dirRead(File& f, DirEntry& entry) {
    RawDirent d;
    DirIter it;
    it.pos = f.pos;
    it.map = f.map;
    bool found = false;
    while (dirNextRaw(f.inode, it, d)) {
      if (d.inode == 0 || d.nameLen == 0) continue;
      char name[3] = {0};
      if (d.nameLen <= 2 && !cache.read(d.dev + 8, name, d.nameLen)) break;
      if ((d.nameLen == 1 && name[0] == '.') ||
          (d.nameLen == 2 && name[0] == '.' && name[1] == '.'))
        continue;
      size_t n = d.nameLen;
      if (n > TINYEXT4_NAME_MAX) n = TINYEXT4_NAME_MAX;
      if (!cache.read(d.dev + 8, entry.name, n)) break;
      entry.name[n] = 0;
      entry.inode = d.inode;
      entry.type = (FileType)d.type;
      if (entry.type == FileType::Unknown) {
        Inode in;
        if (readInode(d.inode, in)) entry.type = modeToType(in.mode);
      }
      found = true;
      break;
    }
    f.pos = it.pos;
    f.map = it.map;
    return found;
  }

#if TINYEXT4_WRITE
  bool dirBlockCsum(const Inode& dir, uint64_t pblk) {
    if (!hasCsum()) return true;
    uint64_t off = blockOff(pblk);
    uint8_t t[12];
    if (!cache.read(off + block_size - 12, t, 12)) return fail(Error::IO);
    if (get32(t) != 0 || get16(t + 4) != 12 || t[7] != ext4::DIR_TAIL_FT) return true;
    uint32_t crc = inodeSeed(dir);
    if (!cache.crc32c(off, block_size - 12, crc) ||
        !cache.write32(off + block_size - 4, crc))
      return fail(Error::IO);
    return true;
  }

  bool writeDirent(uint64_t off, uint32_t ino, uint32_t recLen, const char* name,
                   uint8_t len, FileType type) {
    uint8_t buf[8 + 256 + 4] = {0};
    put32(buf, ino);
    put16(buf + 4, encodeRecLen(recLen));
    buf[6] = len;
    buf[7] = hasFiletype() ? (uint8_t)type : 0;
    memcpy(buf + 8, name, len);
    if (!cache.write(off, buf, dirRecLen(len))) return fail(Error::IO);
    return true;
  }

  bool writeDirTail(uint64_t blockStart) {
    uint8_t t[12] = {0};
    put16(t + 4, 12);
    t[7] = ext4::DIR_TAIL_FT;
    if (!cache.write(blockStart + block_size - 12, t, 12)) return fail(Error::IO);
    return true;
  }

  /// removes the htree index so that the directory can be changed linearly
  bool dropHtree(Inode& dir) {
    uint64_t pblk;
    uint32_t run;
    if (!mapBlock(dir, 0, pblk, run)) return false;
    if (pblk == 0) return fail(Error::Corrupt);
    uint64_t off = blockOff(pblk);
    uint8_t levels;
    if (!cache.read(off + 0x1E, &levels, 1)) return fail(Error::IO);
    if (levels > 0) return fail(Error::Unsupported);
    uint32_t tail = hasCsum() ? ext4::DIR_TAIL_SIZE : 0;
    if (!cache.write16(off + 12 + 4, encodeRecLen(block_size - 12 - tail)))
      return fail(Error::IO);
    if (tail && !writeDirTail(off)) return false;
    dir.flags &= ~ext4::FL_INDEX;
    return writeInode(dir) && dirBlockCsum(dir, pblk);
  }

  bool dirAdd(Inode& dir, const char* name, uint8_t len, uint32_t ino, FileType type) {
    if (dir.isInline()) return fail(Error::Unsupported);
    if ((dir.flags & ext4::FL_INDEX) && !dropHtree(dir)) return false;
    uint32_t need = dirRecLen(len);
    uint32_t usable = block_size - (hasCsum() ? ext4::DIR_TAIL_SIZE : 0);
    uint32_t blocks = (uint32_t)(dir.size >> log_block);
    uint64_t last = 0;
    for (uint32_t b = 0; b < blocks; b++) {
      uint64_t pblk;
      uint32_t run;
      if (!mapBlock(dir, b, pblk, run)) return false;
      if (pblk == 0) continue;
      last = pblk;
      uint64_t base = blockOff(pblk);
      uint32_t off = 0;
      while (off < usable) {
        uint8_t h[8];
        if (!cache.read(base + off, h, 8)) return fail(Error::IO);
        uint32_t entIno = get32(h);
        uint32_t rec = decodeRecLen(get16(h + 4));
        if (rec < 8 || off + rec > block_size) return fail(Error::Corrupt);
        uint32_t used = entIno ? dirRecLen(h[6]) : 0;
        uint32_t limit = off + rec > usable ? usable - off : rec;
        if (limit >= used + need) {
          if (entIno == 0) {
            if (!writeDirent(base + off, ino, rec, name, len, type)) return false;
          } else {
            if (!cache.write16(base + off + 4, encodeRecLen(used)) ||
                !writeDirent(base + off + used, ino, rec - used, name, len, type))
              return fail(Error::IO);
          }
          return dirBlockCsum(dir, pblk);
        }
        off += rec;
      }
    }
    // no space: append a new block
    uint64_t pblk;
    if (!allocBlock(last ? last + 1 : groupFirstBlock((dir.ino - 1) / inodes_per_group), pblk))
      return false;
    if (!appendExtent(dir, blocks, pblk)) {
      freeBlockRange(pblk, 1);
      return false;
    }
    dir.blocks += sectorsPerBlock();
    dir.size += block_size;
    uint64_t base = blockOff(pblk);
    if (!cache.fill(base, 0, block_size)) return fail(Error::IO);
    if (!writeDirent(base, ino, usable, name, len, type)) return false;
    if (hasCsum() && !writeDirTail(base)) return false;
    return dirBlockCsum(dir, pblk) && writeInode(dir);
  }

  bool dirRemove(Inode& dir, const char* name, uint8_t len) {
    if (dir.isInline()) return fail(Error::Unsupported);
    uint32_t blocks = (uint32_t)(dir.size >> log_block);
    for (uint32_t b = 0; b < blocks; b++) {
      uint64_t pblk;
      uint32_t run;
      if (!mapBlock(dir, b, pblk, run)) return false;
      if (pblk == 0) continue;
      uint64_t base = blockOff(pblk);
      uint32_t off = 0, prev = 0xFFFFFFFF, prevRec = 0;
      while (off < block_size) {
        uint8_t h[8];
        if (!cache.read(base + off, h, 8)) return fail(Error::IO);
        RawDirent d{base + off, get32(h), decodeRecLen(get16(h + 4)), h[6], h[7]};
        if (d.recLen < 8 || off + d.recLen > block_size) return fail(Error::Corrupt);
        if (nameEquals(d, name, len)) {
          bool ok = prev == 0xFFFFFFFF
                        ? cache.write32(base + off, 0)
                        : cache.write16(base + prev + 4, encodeRecLen(prevRec + d.recLen));
          if (!ok) return fail(Error::IO);
          return dirBlockCsum(dir, pblk);
        }
        prev = off;
        prevRec = d.recLen;
        off += d.recLen;
      }
    }
    return fail(Error::NotFound);
  }

  bool dirIsEmpty(Inode& dir, bool& empty) {
    DirIter it;
    RawDirent d;
    empty = true;
    while (dirNextRaw(dir, it, d)) {
      if (d.inode == 0) continue;
      char n[2];
      if (d.nameLen <= 2 && !cache.read(d.dev + 8, n, d.nameLen)) return fail(Error::IO);
      if ((d.nameLen == 1 && n[0] == '.') || (d.nameLen == 2 && n[0] == '.' && n[1] == '.'))
        continue;
      empty = false;
      break;
    }
    return error != Error::IO;
  }

  /// dir_nlink: link counts of directories with too many subdirectories are 1
  void incDirLinks(Inode& dir) {
    if (dir.links >= 64999 || dir.links == 1) {
      dir.links = 1;
    } else {
      dir.links++;
    }
  }
  void decDirLinks(Inode& dir) {
    if (dir.links > 2) dir.links--;
  }

  bool createInode(Inode& parent, const char* name, uint8_t len, uint16_t mode,
                   Inode& out) {
    bool isDir = (mode & ext4::MODE_FMT) == ext4::MODE_DIR;
    uint32_t ino;
    if (!beginWrite()) return false;
    if (!allocInode((parent.ino - 1) / inodes_per_group, isDir, ino)) return false;
    if (!initInode(ino, mode, out)) return false;
    if (isDir) {
      uint64_t pblk;
      if (!allocBlock(groupFirstBlock((ino - 1) / inodes_per_group), pblk)) return false;
      uint64_t base = blockOff(pblk);
      uint32_t usable = block_size - (hasCsum() ? ext4::DIR_TAIL_SIZE : 0);
      if (!cache.fill(base, 0, block_size) ||
          !writeDirent(base, ino, 12, ".", 1, FileType::Directory) ||
          !writeDirent(base + 12, parent.ino, usable - 12, "..", 2, FileType::Directory) ||
          (hasCsum() && !writeDirTail(base)))
        return false;
      if (!appendExtent(out, 0, pblk)) return false;
      out.links = 2;
      out.size = block_size;
      out.blocks = sectorsPerBlock();
      if (!writeInode(out) || !dirBlockCsum(out, pblk)) return false;
    } else {
      out.links = 1;
      if (!writeInode(out)) return false;
    }
    if (!dirAdd(parent, name, len, ino, modeToType(mode))) {
      Error e = error;
      deleteInode(out);
      return fail(e);
    }
    if (isDir) {
      incDirLinks(parent);
      if (!writeInode(parent)) return false;
    }
    return true;
  }
#endif  // TINYEXT4_WRITE

  // ------------------------------------------------------------------
  // file data
  // ------------------------------------------------------------------

  size_t fileRead(File& f, uint8_t* buf, size_t len) {
    Inode& in = f.inode;
    size_t done = 0;
    if (in.isDir()) {
      fail(Error::IsDir);
      return 0;
    }
    if (in.isInline() || (in.isSymlink() && !hasDataBlocks(in))) {
      // data in i_block (inline data beyond 60 bytes in xattrs is not supported)
      while (done < len && f.pos < in.size && f.pos < 60) buf[done++] = in.block[f.pos++];
      return done;
    }
    while (done < len && f.pos < in.size) {
      uint32_t lblk = (uint32_t)(f.pos >> log_block);
      uint32_t off = (uint32_t)(f.pos & (block_size - 1));
      uint64_t pblk;
      uint32_t run;
      if (!cachedMap(in, f.map, lblk, pblk, run)) break;
      uint64_t n = ((uint64_t)run << log_block) - off;
      if (n > len - done) n = len - done;
      if (n > in.size - f.pos) n = in.size - f.pos;
      if (pblk == 0) {
        memset(buf + done, 0, (size_t)n);
      } else if (!cache.readBulk(blockOff(pblk) + off, buf + done, (size_t)n)) {
        fail(Error::IO);
        break;
      }
      done += (size_t)n;
      f.pos += n;
    }
    return done;
  }

#if TINYEXT4_WRITE
  size_t fileWrite(File& f, const uint8_t* buf, size_t len) {
    Inode& in = f.inode;
    if (!(f.flags & OpenFlags::Write)) {
      fail(Error::ReadOnly);
      return 0;
    }
    if (!in.isReg()) {
      fail(Error::IsDir);
      return 0;
    }
    if (in.isInline() || !in.hasExtents()) {
      fail(Error::Unsupported);
      return 0;
    }
    if (!beginWrite()) return 0;
    if (f.flags & OpenFlags::Append) f.pos = in.size;
    size_t done = 0;
    while (done < len) {
      uint32_t lblk = (uint32_t)(f.pos >> log_block);
      uint32_t off = (uint32_t)(f.pos & (block_size - 1));
      size_t n = block_size - off;
      if (n > len - done) n = len - done;
      uint64_t pblk;
      uint32_t run;
      if (!cachedMap(in, f.map, lblk, pblk, run)) break;
      if (pblk == 0) {
        bool uninit;
        if (!mapBlock(in, lblk, pblk, run, &uninit)) break;
        if (uninit) {
          fail(Error::Unsupported);
          break;
        }
        uint64_t goal = f.goalLblk + 1 == lblk
                            ? f.goalPblk + 1
                            : groupFirstBlock((in.ino - 1) / inodes_per_group);
        if (!allocBlock(goal, pblk)) break;
        if (!appendExtent(in, lblk, pblk)) {
          freeBlockRange(pblk, 1);
          break;
        }
        in.blocks += sectorsPerBlock();
        f.dirty = true;
        f.goalLblk = lblk;
        f.goalPblk = pblk;
        // the rest of the hole stays a hole
        f.map.lblk = lblk + 1;
        f.map.len = run > 1 ? run - 1 : 0;
        f.map.pblk = 0;
        uint64_t base = blockOff(pblk);
        if ((off > 0 && !cache.fill(base, 0, off)) ||
            (off + n < block_size && !cache.fill(base + off + n, 0, block_size - off - n))) {
          fail(Error::IO);
          break;
        }
      }
      if (!cache.writeBulk(blockOff(pblk) + off, buf + done, n)) {
        fail(Error::IO);
        break;
      }
      done += n;
      f.pos += n;
      if (f.pos > in.size) {
        in.size = f.pos;
        f.dirty = true;
      }
    }
    if (done > 0) {
      in.mtime = in.ctime = now();
      f.dirty = true;
    }
    return done;
  }

  bool fileFlush(File& f) {
    if (!f.dirty) return true;
    f.dirty = false;
    return writeInode(f.inode) && sync();
  }

  bool fileTruncate(File& f, uint64_t size) {
    if (!(f.flags & OpenFlags::Write)) return fail(Error::ReadOnly);
    if (!f.inode.isReg()) return fail(Error::IsDir);
    if (!beginWrite()) return false;
    if (size > f.inode.size) {
      // grow: the new range is a hole
      f.inode.size = size;
    } else if (!truncateInode(f.inode, size)) {
      return false;
    }
    f.map.clear();
    f.goalLblk = 0xFFFFFFFF;
    f.inode.mtime = f.inode.ctime = now();
    f.dirty = true;
    return fileFlush(f);
  }
#endif
};

// ----------------------------------------------------------------------
// Ext4FS: public operations
// ----------------------------------------------------------------------

inline File Ext4FS::open(const char* path, uint8_t flags) {
  File f;
  Inode in;
  error = Error::OK;
  if (!mounted) {
    fail(Error::NotMounted);
    return f;
  }
  bool write = flags & (OpenFlags::Write | OpenFlags::Truncate | OpenFlags::Append);
  if (write && read_only) {
    fail(Error::ReadOnly);
    return f;
  }
  if (!resolve(path, strlen(path), in)) {
#if TINYEXT4_WRITE
    if (error != Error::NotFound || !(flags & OpenFlags::Create)) return f;
    Inode parent;
    const char* name;
    uint8_t len;
    error = Error::OK;
    if (!resolveParent(path, parent, name, len)) return f;
    if (!createInode(parent, name, len, ext4::MODE_REG | file_mode, in)) return f;
    if (!sync()) return f;
#else
    return f;
#endif
  } else if (write && !in.isReg()) {
    fail(in.isDir() ? Error::IsDir : Error::Unsupported);
    return f;
  }
#if TINYEXT4_WRITE
  if ((flags & OpenFlags::Truncate) && in.size > 0) {
    if (!beginWrite() || !truncateInode(in, 0)) return f;
    in.mtime = in.ctime = now();
    if (!writeInode(in) || !sync()) return f;
  }
#endif
  if (write) flags |= OpenFlags::Write;
  f.fs = this;
  f.inode = in;
  f.flags = flags;
  f.pos = (flags & OpenFlags::Append) ? in.size : 0;
  size_t len = strlen(path);
  while (len > 1 && path[len - 1] == '/') len--;
  size_t start = len;
  while (start > 0 && path[start - 1] != '/') start--;
  if (start == len && len > 0) start--;  // root: "/"
  f.setName(path + start, len - start);
  return f;
}

inline bool Ext4FS::readlink(const char* path, char* buf, size_t len) {
  if (len == 0) return fail(Error::Invalid);
  File f = open(path, OpenFlags::Read);
  if (!f) return false;
  if (!f.inode.isSymlink()) return fail(Error::Invalid);
  size_t n = fileRead(f, (uint8_t*)buf, len - 1);
  buf[n] = 0;
  return true;
}

#if TINYEXT4_WRITE
inline bool Ext4FS::mkdir(const char* path) {
  Inode parent, in;
  const char* name;
  uint8_t len;
  uint32_t ino;
  error = Error::OK;
  if (!resolveParent(path, parent, name, len)) return false;
  if (lookup(parent, name, len, ino)) return fail(Error::Exists);
  if (error != Error::NotFound) return false;
  error = Error::OK;
  return createInode(parent, name, len, ext4::MODE_DIR | dir_mode, in) && sync();
}

inline bool Ext4FS::rmdir(const char* path) {
  Inode parent, in;
  const char* name;
  uint8_t len;
  uint32_t ino;
  bool empty;
  error = Error::OK;
  if (!resolveParent(path, parent, name, len)) return false;
  if (!lookup(parent, name, len, ino) || !readInode(ino, in)) return false;
  if (!in.isDir()) return fail(Error::NotDir);
  if (!dirIsEmpty(in, empty)) return false;
  if (!empty) return fail(Error::NotEmpty);
  if (!beginWrite() || !dirRemove(parent, name, len)) return false;
  if (!deleteInode(in)) return false;
  decDirLinks(parent);
  parent.mtime = parent.ctime = now();
  return writeInode(parent) && sync();
}

inline bool Ext4FS::remove(const char* path) {
  Inode parent, in;
  const char* name;
  uint8_t len;
  uint32_t ino;
  error = Error::OK;
  if (!resolveParent(path, parent, name, len)) return false;
  if (!lookup(parent, name, len, ino) || !readInode(ino, in)) return false;
  if (in.isDir()) return fail(Error::IsDir);
  if (!beginWrite() || !dirRemove(parent, name, len)) return false;
  parent.mtime = parent.ctime = now();
  if (!writeInode(parent)) return false;
  if (in.links > 1) {
    in.links--;
    in.ctime = now();
    return writeInode(in) && sync();
  }
  return deleteInode(in) && sync();
}

inline bool Ext4FS::rename(const char* from, const char* to) {
  Inode op, np, src;
  const char *oname, *nname;
  uint8_t olen, nlen, type;
  uint32_t ino, target;
  error = Error::OK;
  if (!resolveParent(from, op, oname, olen)) return false;
  if (!lookup(op, oname, olen, ino, &type) || !readInode(ino, src)) return false;
  if (!resolveParent(to, np, nname, nlen)) return false;
  if (!beginWrite()) return false;
  if (lookup(np, nname, nlen, target)) {
    if (target == ino) return true;
    Inode t;
    if (!readInode(target, t)) return false;
    if (t.isDir() || src.isDir()) return fail(Error::Exists);
    if (!dirRemove(np, nname, nlen)) return false;
    if (t.links > 1) {
      t.links--;
      if (!writeInode(t)) return false;
    } else if (!deleteInode(t)) {
      return false;
    }
  } else if (error != Error::NotFound) {
    return false;
  }
  error = Error::OK;
  bool same = op.ino == np.ino;
  if (src.isDir() && !same) {
    // a directory can not be moved into itself
    Inode cur = np;
    while (cur.ino != ext4::ROOT_INO) {
      uint32_t up;
      if (cur.ino == ino) return fail(Error::Invalid);
      if (!lookup(cur, "..", 2, up) || !readInode(up, cur)) return false;
    }
  }
  Inode& target_dir = same ? op : np;
  FileType ft = hasFiletype() ? (FileType)type : modeToType(src.mode);
  if (!dirAdd(target_dir, nname, nlen, ino, ft)) return false;
  if (!dirRemove(op, oname, olen)) return false;
  uint32_t t = now();
  op.mtime = op.ctime = t;
  if (src.isDir() && !same) {
    uint64_t pblk;
    uint32_t run;
    if (!mapBlock(src, 0, pblk, run)) return false;
    if (pblk == 0) return fail(Error::Corrupt);
    if (!cache.write32(blockOff(pblk) + 12, np.ino)) return fail(Error::IO);
    if (!dirBlockCsum(src, pblk)) return false;
    decDirLinks(op);
    incDirLinks(np);
  }
  if (!same) {
    np.mtime = np.ctime = t;
    if (!writeInode(np)) return false;
  }
  src.ctime = t;
  return writeInode(op) && writeInode(src) && sync();
}
#endif  // TINYEXT4_WRITE

// ----------------------------------------------------------------------
// File
// ----------------------------------------------------------------------

inline void File::setName(const char* n, size_t len) {
  if (len > TINYEXT4_NAME_MAX) len = TINYEXT4_NAME_MAX;
  memcpy(fileName, n, len);
  fileName[len] = 0;
}

inline size_t File::read(uint8_t* buf, size_t len) {
  if (!fs || !(flags & OpenFlags::Read)) return 0;
  return fs->fileRead(*this, buf, len);
}

inline size_t File::write(const uint8_t* buf, size_t len) {
#if TINYEXT4_WRITE
  if (!fs) return 0;
  return fs->fileWrite(*this, buf, len);
#else
  (void)buf;
  (void)len;
  return 0;
#endif
}

inline size_t File::write(const char* str) { return write((const uint8_t*)str, strlen(str)); }

inline void File::flush() {
#if TINYEXT4_WRITE
  if (fs) fs->fileFlush(*this);
#endif
}

inline bool File::seek(uint64_t p) {
  if (!fs) return false;
  if (p > inode.size && !(flags & OpenFlags::Write)) return false;
  pos = p;
  return true;
}

inline bool File::truncate(uint64_t size) {
#if TINYEXT4_WRITE
  if (!fs) return false;
  if (!fs->fileTruncate(*this, size)) return false;
  if (pos > size) pos = size;
  return true;
#else
  (void)size;
  return false;
#endif
}

inline void File::close() {
  if (!fs) return;
  flush();
  fs = nullptr;
  dirty = false;
}

inline bool File::readDir(DirEntry& entry) {
  if (!fs || !inode.isDir()) return false;
  return fs->dirRead(*this, entry);
}

inline File File::openNextFile() {
  File f;
  DirEntry e;
  if (!readDir(e)) return f;
  if (!fs->readInode(e.inode, f.inode)) return f;
  f.fs = fs;
  f.flags = OpenFlags::Read;
  f.setName(e.name, strlen(e.name));
  return f;
}

}  // namespace tinyext4

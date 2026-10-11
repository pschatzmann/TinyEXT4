// SPDX-License-Identifier: MIT
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "Ext4Defs.h"

#ifdef ARDUINO
#include <Stream.h>
#define TINYEXT4_FILE_BASE : public Stream
#define TINYEXT4_OVERRIDE override
#else
#define TINYEXT4_FILE_BASE
#define TINYEXT4_OVERRIDE
#endif

namespace tinyext4 {

class Ext4FS;

/**
 * @brief An open file or directory. On Arduino it is a Stream, so print(),
 * println(), readBytesUntil() etc. can be used.
 *
 * Changes to the inode (size, extents) are kept in the File object and are
 * written by flush() or close(), which is also called by the destructor.
 * Don't write to the same file through two File objects at the same time.
 */
class File TINYEXT4_FILE_BASE {
 public:
  File() = default;
  File(const File& other) = default;
  File& operator=(const File& other) = default;
  ~File() {
    if (dirty) flush();
  }

  explicit operator bool() const { return fs != nullptr; }

  /// reads up to len bytes, returns the number of bytes read
  size_t read(uint8_t* buf, size_t len);
  size_t read(char* buf, size_t len) { return read((uint8_t*)buf, len); }
  /// writes len bytes at the current position
  size_t write(const uint8_t* buf, size_t len) TINYEXT4_OVERRIDE;
  size_t write(uint8_t b) TINYEXT4_OVERRIDE { return write(&b, 1); }
  size_t write(const char* str);
#ifdef ARDUINO
  using Print::write;
#endif

  int available() TINYEXT4_OVERRIDE {
    if (!fs || inode.size <= pos) return 0;
    uint64_t n = inode.size - pos;
    return n > 0x7FFF ? 0x7FFF : (int)n;
  }
  int read() TINYEXT4_OVERRIDE {
    uint8_t b;
    return read(&b, 1) == 1 ? b : -1;
  }
  int peek() TINYEXT4_OVERRIDE {
    uint64_t p = pos;
    int r = read();
    pos = p;
    return r;
  }
  /// writes the inode and all cached data to the device
  void flush() TINYEXT4_OVERRIDE;

  bool seek(uint64_t position);
  uint64_t position() const { return pos; }
  uint64_t size() const { return inode.size; }
  /// changes the file size: frees the blocks after the new end
  bool truncate(uint64_t size);
  void close();

  /// file name (last path component)
  const char* name() const { return fileName; }
  bool isDirectory() const { return inode.isDir(); }
  uint32_t inodeNumber() const { return inode.ino; }
  uint16_t mode() const { return inode.mode; }
  uint32_t getLastWrite() const { return inode.mtime; }

  /// directory: next entry ("." and ".." are skipped)
  bool readDir(DirEntry& entry);
  /// directory: opens the next entry (read only)
  File openNextFile();
  void rewindDirectory() { pos = 0; }

 protected:
  friend class Ext4FS;
  Ext4FS* fs = nullptr;
  Inode inode;
  uint64_t pos = 0;
  uint8_t flags = 0;
  bool dirty = false;
  MapCache map;  // last logical -> physical block mapping
  uint32_t goalLblk = 0xFFFFFFFF;  // last allocated block: goal for the next
  uint64_t goalPblk = 0;
  char fileName[TINYEXT4_NAME_MAX + 1] = {0};

  void setName(const char* n, size_t len);
};

}  // namespace tinyext4

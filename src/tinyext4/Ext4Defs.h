// SPDX-License-Identifier: MIT
#pragma once
#include <stdint.h>

#include "Config.h"

namespace tinyext4 {

/// Result of the last file system operation
enum class Error : uint8_t {
  OK = 0,
  IO,           ///< driver reported an error
  NotMounted,   ///< begin() was not called or failed
  NotExt4,      ///< no ext2/3/4 superblock found
  Unsupported,  ///< feature or on-disk layout we can not handle
  NotFound,     ///< path does not exist
  Exists,       ///< target already exists
  NotDir,       ///< path component is not a directory
  IsDir,        ///< operation not allowed on a directory
  NotEmpty,     ///< directory is not empty
  NoSpace,      ///< no free blocks or inodes
  ReadOnly,     ///< mounted or opened read only
  Invalid,      ///< invalid argument (name, mode, ...)
  Corrupt,      ///< inconsistent on-disk data
  NoMemory,     ///< buffer allocation failed
};

inline const char* errorText(Error e) {
  switch (e) {
    case Error::OK: return "OK";
    case Error::IO: return "IO error";
    case Error::NotMounted: return "not mounted";
    case Error::NotExt4: return "not an ext4 file system";
    case Error::Unsupported: return "unsupported";
    case Error::NotFound: return "not found";
    case Error::Exists: return "exists";
    case Error::NotDir: return "not a directory";
    case Error::IsDir: return "is a directory";
    case Error::NotEmpty: return "directory not empty";
    case Error::NoSpace: return "no space";
    case Error::ReadOnly: return "read only";
    case Error::Invalid: return "invalid argument";
    case Error::Corrupt: return "corrupt file system";
    case Error::NoMemory: return "out of memory";
  }
  return "?";
}

/// Flags for Ext4FS::open()
struct OpenFlags {
  enum : uint8_t {
    Read = 1,
    Write = 2,
    Create = 4,
    Truncate = 8,
    Append = 16,
  };
};

/// Directory entry file types
enum class FileType : uint8_t {
  Unknown = 0,
  Regular = 1,
  Directory = 2,
  CharDevice = 3,
  BlockDevice = 4,
  Fifo = 5,
  Socket = 6,
  Symlink = 7
};

/// Entry returned by File::readDir()
struct DirEntry {
  uint32_t inode = 0;
  FileType type = FileType::Unknown;
  char name[TINYEXT4_NAME_MAX + 1] = {0};
  bool isDirectory() const { return type == FileType::Directory; }
};

/// Result of Ext4FS::stat()
struct FileInfo {
  uint32_t inode = 0;
  uint16_t mode = 0;  ///< type and permission bits (0x4000 directory, 0x8000 file, 0xA000 symlink)
  uint16_t links = 0;
  uint32_t uid = 0;
  uint32_t gid = 0;
  uint64_t size = 0;
  uint32_t atime = 0;
  uint32_t mtime = 0;
  uint32_t ctime = 0;
  bool isDirectory() const { return (mode & 0xF000) == 0x4000; }
  bool isFile() const { return (mode & 0xF000) == 0x8000; }
  bool isSymlink() const { return (mode & 0xF000) == 0xA000; }
};

namespace ext4 {

// superblock
constexpr uint32_t SB_OFFSET = 1024;
constexpr uint16_t MAGIC = 0xEF53;
constexpr uint32_t SB_CSUM_OFFSET = 0x3FC;

// s_state
constexpr uint16_t STATE_VALID = 1;
constexpr uint16_t STATE_ERROR = 2;

// compat
constexpr uint32_t COMPAT_HAS_JOURNAL = 0x4;
constexpr uint32_t COMPAT_SPARSE_SUPER2 = 0x200;

// incompat
constexpr uint32_t INCOMPAT_COMPRESSION = 0x1;
constexpr uint32_t INCOMPAT_FILETYPE = 0x2;
constexpr uint32_t INCOMPAT_RECOVER = 0x4;
constexpr uint32_t INCOMPAT_JOURNAL_DEV = 0x8;
constexpr uint32_t INCOMPAT_META_BG = 0x10;
constexpr uint32_t INCOMPAT_EXTENTS = 0x40;
constexpr uint32_t INCOMPAT_64BIT = 0x80;
constexpr uint32_t INCOMPAT_MMP = 0x100;
constexpr uint32_t INCOMPAT_FLEX_BG = 0x200;
constexpr uint32_t INCOMPAT_EA_INODE = 0x400;
constexpr uint32_t INCOMPAT_DIRDATA = 0x1000;
constexpr uint32_t INCOMPAT_CSUM_SEED = 0x2000;
constexpr uint32_t INCOMPAT_LARGEDIR = 0x4000;
constexpr uint32_t INCOMPAT_INLINE_DATA = 0x8000;
constexpr uint32_t INCOMPAT_ENCRYPT = 0x10000;
constexpr uint32_t INCOMPAT_CASEFOLD = 0x20000;

// we can read these
constexpr uint32_t INCOMPAT_READ =
    INCOMPAT_FILETYPE | INCOMPAT_RECOVER | INCOMPAT_META_BG | INCOMPAT_EXTENTS |
    INCOMPAT_64BIT | INCOMPAT_MMP | INCOMPAT_FLEX_BG | INCOMPAT_EA_INODE |
    INCOMPAT_CSUM_SEED | INCOMPAT_LARGEDIR | INCOMPAT_INLINE_DATA |
    INCOMPAT_ENCRYPT | INCOMPAT_CASEFOLD;
// we can write these
constexpr uint32_t INCOMPAT_WRITE =
    INCOMPAT_FILETYPE | INCOMPAT_EXTENTS | INCOMPAT_64BIT | INCOMPAT_FLEX_BG |
    INCOMPAT_EA_INODE | INCOMPAT_CSUM_SEED | INCOMPAT_LARGEDIR |
    INCOMPAT_INLINE_DATA | INCOMPAT_META_BG;

// ro compat
constexpr uint32_t RO_COMPAT_SPARSE_SUPER = 0x1;
constexpr uint32_t RO_COMPAT_LARGE_FILE = 0x2;
constexpr uint32_t RO_COMPAT_BTREE_DIR = 0x4;
constexpr uint32_t RO_COMPAT_HUGE_FILE = 0x8;
constexpr uint32_t RO_COMPAT_GDT_CSUM = 0x10;
constexpr uint32_t RO_COMPAT_DIR_NLINK = 0x20;
constexpr uint32_t RO_COMPAT_EXTRA_ISIZE = 0x40;
constexpr uint32_t RO_COMPAT_QUOTA = 0x100;
constexpr uint32_t RO_COMPAT_BIGALLOC = 0x200;
constexpr uint32_t RO_COMPAT_METADATA_CSUM = 0x400;
constexpr uint32_t RO_COMPAT_ORPHAN_PRESENT = 0x10000;

constexpr uint32_t RO_COMPAT_WRITE =
    RO_COMPAT_SPARSE_SUPER | RO_COMPAT_LARGE_FILE | RO_COMPAT_BTREE_DIR |
    RO_COMPAT_HUGE_FILE | RO_COMPAT_GDT_CSUM | RO_COMPAT_DIR_NLINK |
    RO_COMPAT_EXTRA_ISIZE | RO_COMPAT_METADATA_CSUM;

// group descriptor flags
constexpr uint16_t BG_INODE_UNINIT = 0x1;
constexpr uint16_t BG_BLOCK_UNINIT = 0x2;

// inode
constexpr uint32_t ROOT_INO = 2;
constexpr uint16_t MODE_FMT = 0xF000;
constexpr uint16_t MODE_DIR = 0x4000;
constexpr uint16_t MODE_REG = 0x8000;
constexpr uint16_t MODE_LNK = 0xA000;
constexpr uint32_t FL_INDEX = 0x1000;
constexpr uint32_t FL_HUGE_FILE = 0x40000;
constexpr uint32_t FL_EXTENTS = 0x80000;
constexpr uint32_t FL_INLINE_DATA = 0x10000000;

// extents
constexpr uint16_t EXT_MAGIC = 0xF30A;
constexpr uint16_t EXT_INIT_MAX_LEN = 32768;

// directory entry tail (metadata_csum)
constexpr uint8_t DIR_TAIL_FT = 0xDE;
constexpr uint32_t DIR_TAIL_SIZE = 12;

}  // namespace ext4

/// In-memory copy of the inode fields we use
struct Inode {
  uint32_t ino = 0;
  uint16_t mode = 0;
  uint16_t links = 0;
  uint32_t uid = 0;
  uint32_t gid = 0;
  uint32_t flags = 0;
  uint64_t size = 0;
  uint64_t blocks = 0;  ///< allocated space in 512 byte units
  uint32_t generation = 0;
  uint64_t fileAcl = 0;  ///< extended attribute block
  uint32_t atime = 0, ctime = 0, mtime = 0, dtime = 0;
  uint8_t block[60] = {0};  ///< i_block: extent root, block map or inline data

  bool isDir() const { return (mode & ext4::MODE_FMT) == ext4::MODE_DIR; }
  bool isReg() const { return (mode & ext4::MODE_FMT) == ext4::MODE_REG; }
  bool isSymlink() const { return (mode & ext4::MODE_FMT) == ext4::MODE_LNK; }
  bool hasExtents() const { return flags & ext4::FL_EXTENTS; }
  bool isInline() const { return flags & ext4::FL_INLINE_DATA; }
};

/// Cached result of a logical to physical block mapping (pblk 0 = hole)
struct MapCache {
  uint32_t lblk = 0;
  uint32_t len = 0;
  uint64_t pblk = 0;
  void clear() { len = 0; }
};

/// Decoded group descriptor
struct GroupDesc {
  uint64_t blockBitmap = 0;
  uint64_t inodeBitmap = 0;
  uint64_t inodeTable = 0;
  uint32_t freeBlocks = 0;
  uint32_t freeInodes = 0;
  uint32_t usedDirs = 0;
  uint32_t itableUnused = 0;
  uint16_t flags = 0;
};

}  // namespace tinyext4

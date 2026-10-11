// SPDX-License-Identifier: MIT
/**
 * @defgroup io IO
 * @ingroup main
 * @brief Block device drivers (ported from TinyFATFS)
 *
 * The drivers use the same diskio style interface as TinyFATFS, so a driver
 * written for one library can be copied to the other with minimal changes.
 * The ext4 file system always uses drive number 0 and 512 byte sectors.
 */

#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifndef FF_MIN_SS
#define FF_MIN_SS 512
#endif
#ifndef FF_MAX_SS
#define FF_MAX_SS 512
#endif
/// 1: drivers implement disk_write
#ifndef FF_IO_USE_WRITE
#define FF_IO_USE_WRITE 1
#endif
/// 1: drivers implement disk_ioctl
#ifndef FF_IO_USE_IOCTL
#define FF_IO_USE_IOCTL 1
#endif
/// SPI fast speed in Hz for SD card access
#ifndef FF_SPI_SPEED_FAST
#define FF_SPI_SPEED_FAST 20000000
#endif

namespace tinyext4 {

typedef unsigned int UINT;
typedef unsigned char BYTE;
typedef uint16_t WORD;
typedef uint32_t DWORD;
typedef uint64_t QWORD;
typedef DWORD LBA_t;

/// Status of Disk Functions
enum DSTATUS {
  STA_CLEAR = 0X00,
  STA_NOINIT = 0x01,  /*!<  Drive not initialized */
  STA_NODISK = 0x02,  /*!<  No medium in the drive */
  STA_PROTECT = 0x04  /*!<  Write protected */
};

/// Results of Disk Functions
enum DRESULT {
  RES_OK = 0, /*!< 0: Successful */
  RES_ERROR,  /*!< 1: R/W Error */
  RES_WRPRT,  /*!< 2: Write Protected */
  RES_NOTRDY, /*!< 3: Not Ready */
  RES_PARERR  /*!< 4: Invalid Parameter */
};

enum ioctl_cmd_t {
  CTRL_SYNC = 0,        /* Complete pending write process */
  GET_SECTOR_COUNT = 1, /* Get media size */
  GET_SECTOR_SIZE = 2,  /* Get sector size */
  GET_BLOCK_SIZE = 3,   /* Get erase block size */
  CTRL_TRIM = 4,        /* Inform device that sectors are no longer used */
  CTRL_POWER = 5,
  CTRL_LOCK = 6,
  CTRL_EJECT = 7,
  CTRL_FORMAT = 8,
  MMC_GET_TYPE = 10,
  MMC_GET_CSD = 11,
  MMC_GET_CID = 12,
  MMC_GET_OCR = 13,
  MMC_GET_SDSTAT = 14,
  ISDIO_READ = 55,
  ISDIO_WRITE = 56,
  ISDIO_MRITE = 57,
  ATA_GET_REV = 20,
  ATA_GET_MODEL = 21,
  ATA_GET_SN = 22
};

/**
 * @brief Abstract block device: 512 byte sectors, drive number 0.
 * @ingroup io
 */
class IO {
 public:
  virtual ~IO() = default;
  virtual DSTATUS disk_initialize(BYTE pdrv) = 0;
  virtual DSTATUS disk_status(BYTE pdrv) = 0;
  virtual DRESULT disk_read(BYTE pdrv, BYTE* buff, LBA_t sector,
                            UINT count) = 0;
  virtual DRESULT disk_write(BYTE pdrv, const BYTE* buff, LBA_t sector,
                             UINT count) = 0;
  virtual DRESULT disk_ioctl(BYTE pdrv, ioctl_cmd_t cmd, void* buff) = 0;
};

}  // namespace tinyext4

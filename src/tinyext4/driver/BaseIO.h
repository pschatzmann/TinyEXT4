// SPDX-License-Identifier: MIT
#pragma once
#include "IO.h"

namespace tinyext4 {

/**
 * @brief Empty IO implementation that we can use to test the compilation
 * @ingroup io
 */
class BaseIO : public IO {
 public:
   DSTATUS disk_initialize(BYTE) override { return STA_NOINIT; }
   DSTATUS disk_status(BYTE) override { return STA_NOINIT; }
   DRESULT disk_read(BYTE, BYTE*, LBA_t,
                            UINT) override {
    return RES_ERROR;
  }
   DRESULT disk_write(BYTE, const BYTE*, LBA_t,
                             UINT) override {
    return RES_ERROR;
  }
   DRESULT disk_ioctl(BYTE, ioctl_cmd_t, void*) override {
    return RES_ERROR;
  }
};

}  // namespace tinyext4
// SPDX-License-Identifier: MIT
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace tinyext4 {

/// @brief little endian access helpers: ext4 stores everything little endian
/// and we never cast packed structs, so unaligned access is no issue.
inline uint16_t get16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
inline uint32_t get32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}
inline void put16(uint8_t* p, uint16_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
}
inline void put32(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

/// @brief CRC32C (Castagnoli) as used by ext4 metadata_csum: raw update
/// without pre/post inversion. Uses a 16 entry table to save flash.
inline uint32_t crc32c(uint32_t crc, const void* data, size_t len) {
  static const uint32_t table[16] = {
      0x00000000, 0x105EC76F, 0x20BD8EDE, 0x30E349B1, 0x417B1DBC, 0x5125DAD3,
      0x61C69362, 0x7198540D, 0x82F63B78, 0x92A8FC17, 0xA24BB5A6, 0xB21572C9,
      0xC38D26C4, 0xD3D3E1AB, 0xE330A81A, 0xF36E6F75};
  const uint8_t* p = (const uint8_t*)data;
  while (len--) {
    crc ^= *p++;
    crc = (crc >> 4) ^ table[crc & 15];
    crc = (crc >> 4) ^ table[crc & 15];
  }
  return crc;
}

inline uint32_t crc32c_u32(uint32_t crc, uint32_t value) {
  uint8_t b[4];
  put32(b, value);
  return crc32c(crc, b, 4);
}

/// @brief CRC16 (ANSI, reflected 0xA001) used by the older gdt_csum feature
inline uint16_t crc16(uint16_t crc, const void* data, size_t len) {
  const uint8_t* p = (const uint8_t*)data;
  while (len--) {
    crc ^= *p++;
    for (int j = 0; j < 8; j++) crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : crc >> 1;
  }
  return crc;
}

}  // namespace tinyext4

// Unit tests for the helpers: checksums, sector cache, allocator, RamIO
#include <vector>

#include "TinyEXT4.h"
#include "test_common.h"

using namespace tinyext4;

int main() {
  // CRC32C check value (standard algorithm = inverted raw crc)
  const char* digits = "123456789";
  CHECK(~crc32c(0xFFFFFFFF, digits, 9) == 0xE3069283, "crc32c");
  CHECK(crc32c_u32(0xFFFFFFFF, 0x34333231) == crc32c(0xFFFFFFFF, "1234", 4), "crc32c_u32");
  // CRC16/ARC check value
  CHECK(crc16(0, digits, 9) == 0xBB3D, "crc16");

  // endian helpers
  uint8_t b[4];
  put32(b, 0x12345678);
  CHECK(b[0] == 0x78 && b[3] == 0x12 && get32(b) == 0x12345678, "put32/get32");
  put16(b, 0xABCD);
  CHECK(get16(b) == 0xABCD, "put16/get16");

  // sector cache on a RAM disk: small cache to force evictions
  RamIO ram(64);
  CHECK(ram.disk_initialize(0) == STA_CLEAR, "ram init");
  SectorCache cache;
  cache.setSize(2);
  CHECK(cache.begin(&ram, 4), "cache begin");
  std::vector<uint8_t> data(20000), back(20000);
  for (size_t j = 0; j < data.size(); j++) data[j] = pattern(j, 1);
  CHECK(cache.write(100, data.data(), 3000), "cache write");
  CHECK(cache.writeBulk(3100, data.data() + 3000, 17000), "cache bulk write");
  CHECK(cache.read(100, back.data(), 20000), "cache read");
  CHECK(memcmp(data.data(), back.data(), 20000) == 0, "cache content");
  CHECK(cache.flush(), "flush");
  // the data must be on the device with the base offset of 4 sectors
  std::vector<uint8_t> raw(512 * 50);
  CHECK(ram.disk_read(0, raw.data(), 4, 50) == RES_OK, "ram read");
  CHECK(memcmp(raw.data() + 100, data.data(), 20000) == 0, "device content");
  // bulk read sees dirty cached sectors
  uint8_t x = 0x5A;
  CHECK(cache.write(1024 + 7, &x, 1), "dirty byte");
  std::vector<uint8_t> bulk(4096);
  CHECK(cache.readBulk(1024, bulk.data(), 4096), "bulk read");
  CHECK(bulk[7] == 0x5A, "bulk read coherent");
  uint32_t crc = 0xFFFFFFFF;
  CHECK(cache.crc32c(2000, 1000, crc), "cache crc");
  CHECK(crc == crc32c(0xFFFFFFFF, data.data() + 1900, 1000), "cache crc value");

  // allocator
  void* p = defaultAllocator().allocate(1000);
  CHECK(p != nullptr && ((uint8_t*)p)[999] == 0, "allocator zeroed");
  defaultAllocator().release(p);
  std::vector<int, AllocatorExt<int>> v(100, 7);
  CHECK(v[99] == 7, "AllocatorExt");

  // mounting something that is not ext4 fails cleanly
  RamIO empty(128);
  Ext4FS fs;
  CHECK(!fs.begin(empty), "mount empty");
  CHECK(fs.lastError() == Error::NotExt4, "NotExt4");
  TEST_EXIT_OK();
}

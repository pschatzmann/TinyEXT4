// Loads an ext4 image into a RamIO and uses it with the Arduino style API
#include <fstream>
#include <vector>

#include "TinyEXT4.h"
#include "test_common.h"

using namespace tinyext4;

int main() {
  if (!haveTool("mke2fs") || !haveTool("e2fsck")) TEST_SKIP("e2fsprogs not installed");
  const char* img = "ramio.img";
  CHECK(run(tool("mke2fs") + " -q -F -t ext4 -b 1024 " + img + " 4M") == 0, "mke2fs");
  std::ifstream in(img, std::ios::binary | std::ios::ate);
  std::vector<uint8_t> data((size_t)in.tellg());
  in.seekg(0);
  in.read((char*)data.data(), data.size());
  RamIO ram(data.size() / 512);
  CHECK(ram.load(data.data(), data.size()), "load");

  Ext4FS fs;
  fs.setTimeCallback([]() -> uint32_t { return 1700000000; });
  fs.setOwner(1000, 1000);
  CHECK(fs.begin(ram), fs.lastErrorText());
  CHECK(fs.mkdir("/logs"), "mkdir");
  File f = fs.open("/logs/log.txt", "a");
  for (int j = 0; j < 1000; j++) {
    char line[40];
    snprintf(line, sizeof(line), "line %d\n", j);
    f.write(line);
  }
  f.close();
  FileInfo info;
  CHECK(fs.stat("/logs/log.txt", info), "stat");
  CHECK(info.uid == 1000 && info.mtime == 1700000000 && info.isFile(), "stat values");
  f = fs.open("/logs/log.txt");
  char buf[16] = {0};
  CHECK(f.read(buf, 7) == 7 && strcmp(buf, "line 0\n") == 0, "read back");
  f.close();
  fs.end();

  // write the RAM disk back to the image and check it
  std::vector<uint8_t> out(data.size());
  CHECK(ram.disk_read(0, out.data(), 0, data.size() / 512) == RES_OK, "read ram");
  std::ofstream o(img, std::ios::binary);
  o.write((const char*)out.data(), out.size());
  o.close();
  CHECK(run(tool("e2fsck") + " -fn " + img) == 0, "e2fsck");
  TEST_EXIT_OK();
}

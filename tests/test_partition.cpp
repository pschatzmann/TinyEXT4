// Partition detection: GPT disk with two ext4 partitions (1 KB and 4 KB
// blocks), selected automatically and by index
#include <cstring>
#include <fstream>
#include <vector>

#include "TinyEXT4.h"
#include "test_common.h"

using namespace tinyext4;

static std::vector<uint8_t> readFile(const char* path) {
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  std::vector<uint8_t> data((size_t)in.tellg());
  in.seekg(0);
  in.read((char*)data.data(), data.size());
  return data;
}

int main() {
  if (!haveTool("mke2fs") || !haveTool("e2fsck")) TEST_SKIP("e2fsprogs not installed");
  CHECK(run(tool("mke2fs") + " -q -F -t ext4 -b 1024 -L first p1.img 8M") == 0, "mke2fs 1");
  CHECK(run(tool("mke2fs") + " -q -F -t ext4 -L second p2.img 16M") == 0, "mke2fs 2");
  auto p1 = readFile("p1.img"), p2 = readFile("p2.img");
  const uint32_t start1 = 2048, start2 = start1 + p1.size() / 512;
  std::vector<uint8_t> disk((start2 + p2.size() / 512 + 64) * 512, 0);
  // protective MBR
  disk[0x1BE + 4] = 0xEE;
  put32(&disk[0x1BE + 8], 1);
  disk[510] = 0x55;
  disk[511] = 0xAA;
  // GPT header (LBA 1) and entries (LBA 2): CRCs are not checked by TinyEXT4
  memcpy(&disk[512], "EFI PART", 8);
  put32(&disk[512 + 0x48], 2);
  put32(&disk[512 + 0x50], 128);
  put32(&disk[512 + 0x54], 128);
  const uint32_t starts[2] = {start1, start2};
  for (int j = 0; j < 2; j++) {
    uint8_t* e = &disk[1024 + j * 128];
    memset(e, 0xAF, 16);  // type GUID (any non zero value)
    put32(e + 0x20, starts[j]);
  }
  memcpy(&disk[start1 * 512], p1.data(), p1.size());
  memcpy(&disk[start2 * 512], p2.data(), p2.size());
  {
    std::ofstream out("gpt.img", std::ios::binary);
    out.write((const char*)disk.data(), disk.size());
  }

  FileIO io("gpt.img");
  {
    Ext4FS fs;
    CHECK(fs.begin(io, true), fs.lastErrorText());
    CHECK(strcmp(fs.volumeName(), "first") == 0 && fs.blockSize() == 1024, "auto: first");
  }
  {
    Ext4FS fs;
    CHECK(fs.begin(io, false, 1), fs.lastErrorText());
    CHECK(strcmp(fs.volumeName(), "second") == 0 && fs.blockSize() == 4096, "index 1");
    CHECK(fs.mkdir("/on_second"), "mkdir");
    fs.end();
  }
  {
    Ext4FS fs;
    CHECK(!fs.begin(io, true, 2), "index 2 does not exist");
    CHECK(fs.lastError() == Error::NotExt4, "NotExt4");
  }
  io.end();
  CHECK(run("dd if=gpt.img of=p2check.img bs=512 skip=" + std::to_string(start2) +
            " count=" + std::to_string(p2.size() / 512)) == 0,
        "dd");
  CHECK(run(tool("e2fsck") + " -fn p2check.img") == 0, "e2fsck");
  TEST_EXIT_OK();
}

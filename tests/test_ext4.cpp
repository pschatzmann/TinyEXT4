// Tests TinyEXT4 against images created by mke2fs. After the changes the
// image is verified with e2fsck and the files are compared with debugfs.
//
// usage: test_ext4 <config>   (see configs[] below)

#include <sys/wait.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <vector>

#include "TinyEXT4.h"
#include "test_common.h"

using namespace tinyext4;
namespace fs = std::filesystem;

struct Config {
  const char* name;
  const char* mkfs;  // mke2fs options
  const char* size;
  bool writable;     // expected after begin(io, false)
  int partition;     // wrap the file system into an MBR at this sector (0: no)
  uint16_t cache;    // cache sectors
};

static const Config configs[] = {
    {"ext4_4k", "-t ext4", "64M", true, 0, 4},
    {"ext4_1k", "-t ext4 -b 1024", "32M", true, 0, 4},
    {"ext4_2k_groups", "-t ext4 -b 2048 -g 2048 -N 4096", "48M", true, 0, 2},
    {"ext4_nocsum", "-t ext4 -O ^metadata_csum,uninit_bg", "32M", true, 0, 4},
    {"ext4_no64bit", "-t ext4 -O ^64bit -b 1024", "32M", true, 0, 8},
    {"ext4_nojournal", "-t ext4 -O ^has_journal,^flex_bg", "32M", true, 0, 4},
    {"ext4_inode128", "-t ext4 -I 128", "32M", true, 0, 4},
    {"ext4_mbr", "-t ext4", "32M", true, 2048, 4},
    {"ext3", "-t ext3", "32M", false, 0, 4},
    {"ext2", "-t ext2 -b 1024", "16M", false, 0, 4},
};

static std::string work;
static std::string image;
static std::string fsimage;  // image of the file system without partition table

static std::vector<uint8_t> readHost(const fs::path& p) {
  std::ifstream in(p, std::ios::binary | std::ios::ate);
  std::vector<uint8_t> data((size_t)in.tellg());
  in.seekg(0);
  in.read((char*)data.data(), data.size());
  return data;
}

static void writeHost(const fs::path& p, size_t len, uint32_t seed) {
  std::vector<char> data(len);
  for (size_t j = 0; j < len; j++) data[j] = (char)pattern(j, seed);
  std::ofstream out(p, std::ios::binary);
  out.write(data.data(), len);
}

/// source tree for mke2fs -d
static void createSourceTree(const fs::path& src) {
  fs::remove_all(src);
  fs::create_directories(src / "sub" / "deep" / "deeper");
  fs::create_directories(src / "big_dir");
  fs::create_directories(src / "empty_dir");
  writeHost(src / "hello.txt", 13, 1);
  writeHost(src / "empty.txt", 0, 2);
  writeHost(src / "one_block.bin", 4096, 3);
  writeHost(src / "large.bin", 3 * 1024 * 1024 + 123, 4);
  writeHost(src / "sub" / "a.txt", 100, 5);
  writeHost(src / "sub" / "deep" / "deeper" / "z.bin", 70000, 6);
  for (int j = 0; j < 300; j++) {
    char name[80];
    snprintf(name, sizeof(name), "entry_with_a_rather_long_file_name_%04d.txt", j);
    writeHost(src / "big_dir" / name, j, 100 + j);
  }
  fs::create_symlink("hello.txt", src / "short_link");
  std::string longTarget = "sub/deep/deeper/";
  while (longTarget.size() < 100) longTarget += "x";
  fs::create_symlink(longTarget, src / "long_link");
}

static std::vector<uint8_t> readAll(Ext4FS& ext, const std::string& path) {
  File f = ext.open(path.c_str());
  CHECK(f, "open " + path + ": " + ext.lastErrorText());
  std::vector<uint8_t> data(f.size());
  // read with odd chunk sizes to cover partial sectors and blocks
  size_t pos = 0, chunk = 1;
  while (pos < data.size()) {
    size_t n = std::min(chunk, data.size() - pos);
    size_t r = f.read(data.data() + pos, n);
    CHECK(r == n, "short read " + path);
    pos += n;
    chunk = chunk * 3 + 7;
    if (chunk > 300000) chunk = 1;
  }
  uint8_t b;
  CHECK(f.read(&b, 1) == 0, "read beyond end " + path);
  return data;
}

/// compares all files below the host directory with the ext4 content
static void compareTree(Ext4FS& ext, const fs::path& host, const std::string& path) {
  std::map<std::string, bool> expected;
  for (auto& e : fs::directory_iterator(host)) {
    std::string name = e.path().filename().string();
    std::string child = path + "/" + name;
    expected[name] = false;
    if (fs::is_symlink(e.symlink_status())) {
      char target[512];
      CHECK(ext.readlink(child.c_str(), target, sizeof(target)), "readlink " + child);
      CHECK(fs::read_symlink(e.path()).string() == target, "symlink target " + child);
    } else if (fs::is_directory(e.status())) {
      FileInfo info;
      CHECK(ext.stat(child.c_str(), info) && info.isDirectory(), "stat dir " + child);
      compareTree(ext, e.path(), child);
    } else {
      auto want = readHost(e.path());
      auto got = readAll(ext, child);
      CHECK(want == got, "content " + child);
    }
  }
  // the directory listing must match exactly
  File dir = ext.open(path.empty() ? "/" : path.c_str());
  CHECK(dir && dir.isDirectory(), "open dir " + path);
  DirEntry e;
  int count = 0;
  while (dir.readDir(e)) {
    if (path.empty() && strcmp(e.name, "lost+found") == 0) continue;
    CHECK(expected.count(e.name) == 1, std::string("unexpected entry ") + e.name);
    CHECK(!expected[e.name], std::string("duplicate entry ") + e.name);
    expected[e.name] = true;
    count++;
  }
  CHECK(count == (int)expected.size(), "entry count " + path);
}

static void fsck(const char* when) {
  std::string cmd = tool("e2fsck") + " -fn " + fsimage + " > " + work + "/fsck.log 2>&1";
  int rc = system(cmd.c_str());
  if (rc != 0) {
    system(("cat " + work + "/fsck.log").c_str());
    CHECK(false, std::string("e2fsck failed ") + when);
  }
}

/// copies the file system out of the partitioned image (for e2fsck/debugfs)
static void extractPartition(const Config& c) {
  if (!c.partition) return;
  std::string cmd = "dd if=" + image + " of=" + fsimage + " bs=512 skip=" +
                    std::to_string(c.partition) + " status=none";
  CHECK(system(cmd.c_str()) == 0, "dd");
}

static void writeFile(Ext4FS& ext, const char* path, size_t len, uint32_t seed,
                      const char* mode = "w") {
  File f = ext.open(path, mode);
  CHECK(f, std::string("create ") + path + ": " + ext.lastErrorText());
  std::vector<uint8_t> data(len);
  for (size_t j = 0; j < len; j++) data[j] = pattern(j, seed);
  // write in irregular chunks
  size_t pos = 0, chunk = 5;
  while (pos < len) {
    size_t n = std::min(chunk, len - pos);
    CHECK(f.write(data.data() + pos, n) == n, std::string("write ") + path + ": " +
                                                    ext.lastErrorText());
    pos += n;
    chunk = chunk * 2 + 11;
    if (chunk > 200000) chunk = 3;
  }
  f.close();
}

static bool checkPattern(const std::vector<uint8_t>& d, size_t from, size_t len,
                         uint32_t seed, size_t patternOffset = 0) {
  for (size_t j = 0; j < len; j++)
    if (d[from + j] != pattern(j + patternOffset, seed)) return false;
  return true;
}

static void testReadOnly(const Config& c, Ext4FS& ext, const fs::path& src) {
  compareTree(ext, src, "");
  FileInfo info;
  CHECK(!ext.stat("/does/not/exist", info), "stat missing");
  CHECK(ext.lastError() == Error::NotFound, "NotFound");
  CHECK(!ext.stat("/hello.txt/x", info), "stat below file");
  CHECK(ext.lastError() == Error::NotDir, "NotDir");
  // openNextFile
  File root = ext.open("/");
  int n = 0;
  for (File f = root.openNextFile(); f; f = root.openNextFile()) n++;
  CHECK(n == 10, "openNextFile count");
  // seek
  File f = ext.open("/large.bin");
  CHECK(f.seek(1000000), "seek");
  uint8_t b[10];
  CHECK(f.read(b, 10) == 10 && b[0] == pattern(1000000, 4) && b[9] == pattern(1000009, 4),
        "seek read");
  CHECK(f.peek() == pattern(1000010, 4), "peek");
  CHECK(!f.seek(f.size() + 1), "seek beyond end read only");
  (void)c;
}

static void testWrite(Ext4FS& ext) {
  uint64_t freeBefore = ext.freeBytes();
  uint32_t inodesBefore = ext.freeInodes();

  // new files of different sizes
  const size_t sizes[] = {0, 1, 511, 512, 1023, 1024, 4095, 4096, 4097, 100000, 1500000};
  for (size_t j = 0; j < sizeof(sizes) / sizeof(sizes[0]); j++) {
    char path[40];
    snprintf(path, sizeof(path), "/new_%zu.bin", sizes[j]);
    writeFile(ext, path, sizes[j], 1000 + (uint32_t)j);
    auto d = readAll(ext, path);
    CHECK(d.size() == sizes[j] && checkPattern(d, 0, d.size(), 1000 + (uint32_t)j),
          std::string("verify ") + path);
  }

  // append
  writeFile(ext, "/append.txt", 3000, 7);
  writeFile(ext, "/append.txt", 5000, 8, "a");
  {
    auto d = readAll(ext, "/append.txt");
    CHECK(d.size() == 8000 && checkPattern(d, 0, 3000, 7) && checkPattern(d, 3000, 5000, 8),
          "append content");
  }

  // overwrite in the middle with r+
  {
    File f = ext.open("/new_100000.bin", "r+");
    CHECK(f, "open r+");
    CHECK(f.seek(5000), "seek r+");
    std::vector<uint8_t> d(9000);
    for (size_t j = 0; j < d.size(); j++) d[j] = pattern(j, 99);
    CHECK(f.write(d.data(), d.size()) == d.size(), "overwrite");
    f.close();
    auto r = readAll(ext, "/new_100000.bin");
    CHECK(r.size() == 100000 && checkPattern(r, 0, 5000, 1009) &&
              checkPattern(r, 5000, 9000, 99) && checkPattern(r, 14000, 86000, 1009, 14000),
          "overwrite content");
  }

  // Stream / Print style writes and seek beyond the end (sparse file)
  {
    File f = ext.open("/sparse.bin", "w+");
    CHECK(f.write("start") == 5, "write str");
    CHECK(f.seek(200000), "seek beyond end");
    CHECK(f.write("end") == 3, "write after hole");
    f.close();
    auto r = readAll(ext, "/sparse.bin");
    CHECK(r.size() == 200003 && memcmp(r.data(), "start", 5) == 0 &&
              memcmp(r.data() + 200000, "end", 3) == 0,
          "sparse content");
    for (size_t j = 5; j < 200000; j++) CHECK(r[j] == 0, "hole is zero");
  }

  // truncate
  {
    File f = ext.open("/new_1500000.bin", "r+");
    CHECK(f.truncate(70001), "truncate");
    f.close();
    auto r = readAll(ext, "/new_1500000.bin");
    CHECK(r.size() == 70001 && checkPattern(r, 0, 70001, 1010), "truncate content");
    f = ext.open("/new_1500000.bin", "r+");
    CHECK(f.truncate(80000), "grow with truncate");
    f.close();
    r = readAll(ext, "/new_1500000.bin");
    CHECK(r.size() == 80000 && checkPattern(r, 0, 70001, 1010), "grown content");
    for (size_t j = 70001; j < 80000; j++) CHECK(r[j] == 0, "grown part is zero");
    writeFile(ext, "/new_4097.bin", 10, 55);  // "w" truncates
    CHECK(readAll(ext, "/new_4097.bin").size() == 10, "w truncates");
  }

  // directories
  CHECK(ext.mkdir("/d1"), std::string("mkdir: ") + ext.lastErrorText());
  CHECK(ext.mkdir("/d1/d2/"), "mkdir trailing slash");
  CHECK(ext.mkdir("/d1/d2/d3"), "mkdir nested");
  CHECK(!ext.mkdir("/d1"), "mkdir exists");
  CHECK(ext.lastError() == Error::Exists, "Exists");
  CHECK(!ext.mkdir("/nope/x"), "mkdir missing parent");
  writeFile(ext, "/d1/d2/d3/f.txt", 1234, 42);
  CHECK(!ext.rmdir("/d1/d2"), "rmdir not empty");
  CHECK(ext.lastError() == Error::NotEmpty, "NotEmpty");
  CHECK(!ext.remove("/d1"), "remove dir");
  CHECK(ext.lastError() == Error::IsDir, "IsDir");

  // grow a new directory to several blocks, then remove some entries
  CHECK(ext.mkdir("/many"), "mkdir many");
  for (int j = 0; j < 400; j++) {
    char path[120];
    snprintf(path, sizeof(path), "/many/a_file_name_that_needs_some_space_%03d", j);
    writeFile(ext, path, j % 7, j);
  }
  for (int j = 0; j < 400; j += 3) {
    char path[120];
    snprintf(path, sizeof(path), "/many/a_file_name_that_needs_some_space_%03d", j);
    CHECK(ext.remove(path), std::string("remove ") + path);
  }
  {
    File d = ext.open("/many");
    DirEntry e;
    int n = 0;
    while (d.readDir(e)) n++;
    CHECK(n == 400 - 134, "many count");
  }
  // reuse of freed directory slots
  writeFile(ext, "/many/a_file_name_that_needs_some_space_000", 3, 1);

  // add and remove entries in the directory created by mke2fs (htree)
  writeFile(ext, "/big_dir/added_by_tinyext4.txt", 77, 77);
  CHECK(ext.remove("/big_dir/entry_with_a_rather_long_file_name_0150.txt"), "remove in htree");
  CHECK(ext.exists("/big_dir/added_by_tinyext4.txt"), "exists in htree");

  // rename
  CHECK(ext.rename("/hello.txt", "/hello2.txt"), "rename");
  CHECK(!ext.exists("/hello.txt") && ext.exists("/hello2.txt"), "rename result");
  CHECK(ext.rename("/hello2.txt", "/d1/d2/hello3.txt"), "move file");
  CHECK(ext.rename("/new_1.bin", "/new_511.bin"), "rename replaces file");
  CHECK(readAll(ext, "/new_511.bin").size() == 1, "replaced content");
  CHECK(ext.rename("/d1/d2", "/sub/d2moved"), "move dir");
  CHECK(ext.exists("/sub/d2moved/d3/f.txt"), "moved dir content");
  CHECK(!ext.rename("/sub", "/sub/d2moved/x"), "move dir into itself");
  {
    // ".." must point to the new parent
    FileInfo sub, up;
    CHECK(ext.stat("/sub", sub) && ext.stat("/sub/d2moved/..", up) && sub.inode == up.inode,
          "dotdot updated");
  }

  // fragmented files: two files written block by block alternately get many
  // extents, which needs extent tree blocks (depth 1 or 2)
  {
    File a = ext.open("/frag_a.bin", "w");
    File b = ext.open("/frag_b.bin", "w");
    std::vector<uint8_t> blk(ext.blockSize());
    const int blocks = 1200;
    for (int j = 0; j < blocks; j++) {
      for (size_t k = 0; k < blk.size(); k++) blk[k] = pattern(j * blk.size() + k, 11);
      CHECK(a.write(blk.data(), blk.size()) == blk.size(), "frag a");
      for (size_t k = 0; k < blk.size(); k++) blk[k] = pattern(j * blk.size() + k, 12);
      CHECK(b.write(blk.data(), blk.size()) == blk.size(),
            std::string("frag b: ") + ext.lastErrorText());
    }
    a.close();
    b.close();
    auto ra = readAll(ext, "/frag_a.bin");
    CHECK(ra.size() == blocks * blk.size() && checkPattern(ra, 0, ra.size(), 11), "frag a data");
    // cut a in the middle of the tree, remove b completely
    File t = ext.open("/frag_a.bin", "r+");
    CHECK(t.truncate(blocks / 3 * blk.size() + 17), "truncate fragmented");
    t.close();
    ra = readAll(ext, "/frag_a.bin");
    CHECK(ra.size() == blocks / 3 * blk.size() + 17 && checkPattern(ra, 0, ra.size(), 11),
          "frag a truncated");
  }
  CHECK(ext.remove("/frag_b.bin"), "remove fragmented");

  // remove all files that we created: the free space must be restored
  CHECK(ext.remove("/frag_a.bin"), "remove frag a");
  ext.sync();

  // read only open of a directory for write
  CHECK(!ext.open("/sub", "w"), "open dir for write");
  CHECK(ext.freeBytes() < freeBefore && ext.freeInodes() < inodesBefore, "space used");
}

/// files written by testWrite, verified with debugfs (independent reader)
static void verifyWithDebugfs() {
  struct Item {
    const char* path;
    size_t len;
    uint32_t seed;
  } items[] = {{"/new_4096.bin", 4096, 1007},
               {"/sub/d2moved/d3/f.txt", 1234, 42},
               {"/append.txt", 3000, 7},
               {"/new_1500000.bin", 70001, 1010},
               {"/big_dir/added_by_tinyext4.txt", 77, 77}};
  for (auto& it : items) {
    std::string out = work + "/dump.bin";
    std::string cmd = tool("debugfs") + " -R 'dump " + it.path + " " + out + "' " +
                      fsimage + " >/dev/null 2>&1";
    CHECK(system(cmd.c_str()) == 0, "debugfs dump");
    auto d = readHost(out);
    CHECK(d.size() >= it.len && checkPattern(d, 0, it.len, it.seed),
          std::string("debugfs content ") + it.path);
  }
}

int main(int argc, char** argv) {
  if (argc < 2) {
    printf("usage: %s <config>\n", argv[0]);
    return 2;
  }
  if (!haveTool("mke2fs") || !haveTool("e2fsck") || !haveTool("debugfs"))
    TEST_SKIP("e2fsprogs (mke2fs, e2fsck, debugfs) not installed");
  const Config* cfg = nullptr;
  for (auto& c : configs)
    if (strcmp(c.name, argv[1]) == 0) cfg = &c;
  CHECK(cfg != nullptr, std::string("unknown config ") + argv[1]);
  const Config& c = *cfg;

  work = (fs::current_path() / (std::string("work_") + c.name)).string();
  fs::remove_all(work);
  fs::create_directories(work);
  fs::path src = fs::path(work) / "src";
  createSourceTree(src);
  fsimage = work + "/fs.img";
  image = c.partition ? work + "/disk.img" : fsimage;
  std::string cmd = tool("mke2fs") + " -q -F -E lazy_itable_init=1 " + c.mkfs + " -d " +
                    src.string() + " " + fsimage + " " + c.size;
  CHECK(run(cmd) == 0, "mke2fs: " + cmd);
  // mke2fs -d does not index directories: e2fsck -D creates the htree of big_dir
  run(tool("e2fsck") + " -fyD " + fsimage);
  if (c.partition) {
    // disk image with an MBR and one Linux partition
    uint64_t fsSize = fs::file_size(fsimage);
    std::vector<uint8_t> mbr(512, 0);
    uint8_t* e = &mbr[0x1BE];
    e[4] = 0x83;
    put32(e + 8, c.partition);
    put32(e + 12, (uint32_t)(fsSize / 512));
    mbr[510] = 0x55;
    mbr[511] = 0xAA;
    std::ofstream out(image, std::ios::binary);
    out.write((const char*)mbr.data(), 512);
    out.close();
    CHECK(system(("dd if=" + fsimage + " of=" + image + " bs=512 seek=" +
                  std::to_string(c.partition) + " status=none")
                     .c_str()) == 0,
          "dd");
  }
  fsck("after mke2fs");

  // read only pass
  {
    FileIO io(image.c_str());
    Ext4FS ext;
    ext.setCacheSectors(c.cache);
    CHECK(ext.begin(io, true), std::string("mount ro: ") + ext.lastErrorText());
    CHECK(ext.isReadOnly(), "read only");
    CHECK(!ext.open("/x", "w"), "create on read only");
    CHECK(ext.lastError() == Error::ReadOnly, "ReadOnly error");
    testReadOnly(c, ext, src);
    ext.end();
  }

  // read write pass
  {
    FileIO io(image.c_str());
    Ext4FS ext;
    ext.setCacheSectors(c.cache);
    CHECK(ext.begin(io, false), std::string("mount rw: ") + ext.lastErrorText());
    CHECK(ext.isReadOnly() == !c.writable, "writable as expected");
    if (!c.writable) {
      CHECK(!ext.mkdir("/x"), "mkdir on read only");
      ext.end();
      fsck("read only");
      TEST_EXIT_OK();
    }
    testWrite(ext);
    ext.end();
  }
  extractPartition(c);
  fsck("after writing");
  verifyWithDebugfs();

  // remount: the unchanged files and the new ones must be readable
  {
    FileIO io(image.c_str());
    Ext4FS ext;
    CHECK(ext.begin(io, false), "remount");
    auto d = readAll(ext, "/sub/d2moved/d3/f.txt");
    CHECK(d.size() == 1234 && checkPattern(d, 0, 1234, 42), "content after remount");
    d = readAll(ext, "/large.bin");
    CHECK(checkPattern(d, 0, d.size(), 4), "large.bin after remount");
    // remove everything we created and check with e2fsck again
    const char* files[] = {"/append.txt", "/sparse.bin", "/sub/d2moved/d3/f.txt",
                           "/sub/d2moved/hello3.txt"};
    for (auto f : files) CHECK(ext.remove(f), std::string("cleanup ") + f);
    CHECK(ext.rmdir("/sub/d2moved/d3") && ext.rmdir("/sub/d2moved") && ext.rmdir("/d1"),
          "cleanup dirs");
    File m = ext.open("/many");
    DirEntry e;
    std::vector<std::string> names;
    while (m.readDir(e)) names.push_back(std::string("/many/") + e.name);
    m.close();
    for (auto& n : names) CHECK(ext.remove(n.c_str()), "cleanup many");
    CHECK(ext.rmdir("/many"), "rmdir many");
    ext.end();
  }
  extractPartition(c);
  fsck("after cleanup");
  TEST_EXIT_OK();
}

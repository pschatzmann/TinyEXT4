// Desktop example: access an ext4 image (or a block device) from the command
// line using TinyEXT4 and the FileIO driver.
//
//   ext4tool disk.img info
//   ext4tool disk.img ls /dir
//   ext4tool disk.img cat /file
//   ext4tool disk.img get /file local_file
//   ext4tool disk.img put local_file /file
//   ext4tool disk.img mkdir /dir | rmdir /dir | rm /file | mv /a /b

#include <cstdio>
#include <cstring>

#include "TinyEXT4.h"

using namespace tinyext4;

static int usage() {
  printf("usage: ext4tool <image> info|ls|cat|get|put|mkdir|rmdir|rm|mv [args]\n");
  return 2;
}

static int failed(Ext4FS& fs, const char* what) {
  printf("%s: %s\n", what, fs.lastErrorText());
  return 1;
}

int main(int argc, char** argv) {
  if (argc < 3) return usage();
  const char* cmd = argv[2];
  bool readOnly = !strcmp(cmd, "info") || !strcmp(cmd, "ls") || !strcmp(cmd, "cat") ||
                  !strcmp(cmd, "get");
  FileIO io(argv[1]);
  Ext4FS fs;
  if (!fs.begin(io, readOnly)) return failed(fs, "mount");

  if (!strcmp(cmd, "info")) {
    printf("volume:     %s\n", fs.volumeName());
    printf("block size: %u\n", (unsigned)fs.blockSize());
    printf("total:      %llu bytes\n", (unsigned long long)fs.totalBytes());
    printf("free:       %llu bytes\n", (unsigned long long)fs.freeBytes());
    printf("free inodes:%u\n", (unsigned)fs.freeInodes());
  } else if (!strcmp(cmd, "ls")) {
    const char* path = argc > 3 ? argv[3] : "/";
    File dir = fs.open(path);
    if (!dir) return failed(fs, path);
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
      printf("%c %06o %10llu %s\n", f.isDirectory() ? 'd' : '-', f.mode() & 07777,
             (unsigned long long)f.size(), f.name());
    }
  } else if (!strcmp(cmd, "cat") && argc > 3) {
    File f = fs.open(argv[3]);
    if (!f) return failed(fs, argv[3]);
    uint8_t buf[4096];
    size_t n;
    while ((n = f.read(buf, sizeof(buf))) > 0) fwrite(buf, 1, n, stdout);
  } else if (!strcmp(cmd, "get") && argc > 4) {
    File f = fs.open(argv[3]);
    if (!f) return failed(fs, argv[3]);
    FILE* out = fopen(argv[4], "wb");
    if (!out) return 1;
    uint8_t buf[4096];
    size_t n;
    while ((n = f.read(buf, sizeof(buf))) > 0) fwrite(buf, 1, n, out);
    fclose(out);
  } else if (!strcmp(cmd, "put") && argc > 4) {
    FILE* in = fopen(argv[3], "rb");
    if (!in) return 1;
    File f = fs.open(argv[4], "w");
    if (!f) return failed(fs, argv[4]);
    uint8_t buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
      if (f.write(buf, n) != n) return failed(fs, "write");
    }
    fclose(in);
    f.close();
  } else if (!strcmp(cmd, "mkdir") && argc > 3) {
    if (!fs.mkdir(argv[3])) return failed(fs, "mkdir");
  } else if (!strcmp(cmd, "rmdir") && argc > 3) {
    if (!fs.rmdir(argv[3])) return failed(fs, "rmdir");
  } else if (!strcmp(cmd, "rm") && argc > 3) {
    if (!fs.remove(argv[3])) return failed(fs, "rm");
  } else if (!strcmp(cmd, "mv") && argc > 4) {
    if (!fs.rename(argv[3], argv[4])) return failed(fs, "mv");
  } else {
    return usage();
  }
  fs.end();
  return 0;
}

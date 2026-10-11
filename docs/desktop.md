# Desktop Build and Tests

The library is plain C++17 and doesn't need the Arduino API, so it builds directly on Linux and macOS. On the desktop `File` is not a `Stream`, and `FileIO` accesses image files. You need CMake, a C++17 compiler and, for the tests, e2fsprogs (`mke2fs`, `e2fsck`, `debugfs`):

```bash
sudo apt install cmake g++ e2fsprogs      # Debian / Ubuntu
cmake -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Options: `-DTINYEXT4_SANITIZE=OFF` (on by default: address and undefined behavior sanitizer), `-DTINYEXT4_BUILD_TESTS=OFF`, `-DTINYEXT4_BUILD_EXAMPLES=OFF`.

To use the library in your own CMake project:

```cmake
add_subdirectory(TinyEXT4)
target_link_libraries(myapp PRIVATE tinyext4)
```

## ext4tool

[examples/desktop/ext4tool.cpp](../examples/desktop/ext4tool.cpp) accesses images (or block devices) from the command line without mounting them:

```bash
truncate -s 64M disk.img && mkfs.ext4 -q disk.img
./build/examples/desktop/ext4tool disk.img put README.md /readme.md
./build/examples/desktop/ext4tool disk.img mkdir /docs
./build/examples/desktop/ext4tool disk.img ls /
./build/examples/desktop/ext4tool disk.img cat /readme.md
./build/examples/desktop/ext4tool disk.img mv /readme.md /docs/readme.md
./build/examples/desktop/ext4tool disk.img info
e2fsck -fn disk.img
```

## Tests

| Test | |
|---|---|
| `util` | CRC32C and CRC16 check values, endian helpers, the sector cache (eviction, bulk transfers, coherence, partition offset), the allocator, mounting a non-ext4 device |
| `partition` | GPT disk with two ext4 partitions: automatic detection, selection by index, writing to the second one, `e2fsck` |
| `ramio` | ext4 image in a `RamIO`: mkdir, appending with `write(const char*)`, owner and time callback, `e2fsck` |
| `ext4_4k`, `ext4_1k`, `ext4_2k_groups`, `ext4_nocsum`, `ext4_no64bit`, `ext4_nojournal`, `ext4_inode128`, `ext4_mbr` | full read and write test, see below |
| `ext3`, `ext2` | read test; the file system must be mounted read-only and refuse changes |

Each file system test:

1. creates a source tree with files of different sizes, nested directories, a 300-entry directory and symbolic links, and builds an image with `mke2fs -d`. `e2fsck -D` turns the large directory into an htree.
2. mounts read-only and compares every file, directory listing and symlink target with the source tree. Reads use irregular chunk sizes. It also tests `seek`, `peek` and the error codes.
3. mounts read/write and creates files of 0 bytes to 1.5 MB, appends, overwrites in the middle, writes sparse files, shrinks and grows files, creates nested directories, grows a directory to several blocks and removes every third entry, adds to the htree directory, renames and moves files and directories (and checks `..`), and writes two files block by block alternately. That fragments them into 1200 extents each, so the extent tree grows to depth 2 with 1 KB blocks. Then it truncates the fragmented file in the middle of the tree and deletes both.
4. runs `e2fsck -fn` on the image and reads the new files with `debugfs`.
5. mounts again, checks the files, deletes everything that was created and runs `e2fsck -fn` again.

Configurations with a cache of only 2 sectors stress the cache eviction.

The work files stay in `build/tests/work_<config>` for inspection. The whole suite runs in a few seconds.

On each push to `main`, [doxygen-gh-pages.yml](../.github/workflows/doxygen-gh-pages.yml) runs Doxygen and publishes `docs/html` to the `doxygen` branch, which GitHub Pages serves as the [API documentation](https://pschatzmann.github.io/TinyEXT4/annotated.html). Locally: `doxygen Doxyfile`.

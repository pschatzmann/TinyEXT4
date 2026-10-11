# Memory

ext4 blocks are usually 4 KB, and many embedded implementations keep one or more whole blocks in RAM. TinyEXT4 doesn't: it accesses metadata by byte offset through a small LRU cache of 512-byte sectors, and computes checksums over a block by streaming it through the cache. Bulk file data skips the cache: whole sectors go directly between the driver and your buffer.

| Item | RAM |
|---|---|
| Sector cache | `n` × ~520 bytes, heap (default `n = 4`: ~2 KB) |
| `Ext4FS` object | ~350 bytes |
| each open `File` | ~450 bytes, of which 256 are the name buffer |
| stack during directory changes | < 1 KB |

On a SAMD21 (MKR Zero) the read example uses 34 KB of flash and 2.9 KB of static RAM (including Serial and SPI). The SD driver is the same as in TinyFATFS.

## Tuning

```c++
fs.setCacheSectors(16);   // before begin(): more cache = fewer driver calls
```

More sectors help with large directories and fragmented files, especially when writing. Two is the minimum, and the default of 4 is enough for sequential reading and writing.

Compile time settings (define them before `#include "TinyEXT4.h"`):

| Define | Default | |
|---|---|---|
| `TINYEXT4_CACHE_SECTORS` | 4 | default cache size |
| `TINYEXT4_NAME_MAX` | 255 | name buffer in `File` and `DirEntry`; longer names are cut off |
| `TINYEXT4_WRITE` | 1 | 0: read-only build, smaller code |
| `TINYEXT4_MAX_TRANSFER_SECTORS` | 128 | max sectors per driver call |
| `TINYEXT4_DIRTY_GROUPS` | 8 | groups whose bitmap checksums are updated at `sync()` |
| `TINYEXT4_LOG` | 0 | 1: print diagnostics |

## PSRAM allocator

The cache buffers and the `RamIO` sectors are allocated through `tinyext4::Allocator` ([Allocator.h](../src/tinyext4/Allocator.h)), which prefers PSRAM:

- ESP32: `heap_caps_malloc(size, MALLOC_CAP_SPIRAM)`
- RP2350 with PSRAM (`RP2350_PSRAM_CS` defined by the board): `pmalloc()`
- otherwise, or when PSRAM is full: `malloc()`

For containers there is `AllocatorExt<T>`, an STL-compatible allocator, e.g. `std::vector<uint8_t, AllocatorExt<uint8_t>>`.

To keep the cache in internal RAM (it's faster than PSRAM, and some DMA drivers need it), or to put it anywhere else, pass another allocator:

```c++
InternalAllocator internal;     // malloc() only
fs.setAllocator(internal);      // before begin()

class MyAllocator : public tinyext4::Allocator {
  void* allocate(size_t size) override { ... }
  void release(void* p) override { ... }
};
```

`end()` releases the cache buffers.

// SPDX-License-Identifier: MIT
#pragma once
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <new>

#if defined(ESP32)
#include <esp_heap_caps.h>
#endif
#if defined(ARDUINO_ARCH_RP2040)
#include <Arduino.h>
#endif

namespace tinyext4 {

/**
 * @brief Memory allocator that prefers PSRAM (SPI RAM) when the board
 * provides it: ESP32 (heap_caps_malloc with MALLOC_CAP_SPIRAM) and RP2350
 * with PSRAM (pmalloc). Otherwise and when PSRAM is exhausted the normal
 * heap is used. Allocated memory is zeroed.
 *
 * Subclass it and pass it to Ext4FS::setAllocator() / RamIO to place the
 * buffers somewhere else.
 */
class Allocator {
 public:
  virtual ~Allocator() = default;

  virtual void* allocate(size_t size) {
    void* p = nullptr;
#if defined(ESP32)
    p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#elif defined(ARDUINO_ARCH_RP2040) && defined(RP2350_PSRAM_CS)
    p = pmalloc(size);
#endif
    if (p == nullptr) p = malloc(size);
    if (p != nullptr) memset(p, 0, size);
    return p;
  }

  /// free() releases PSRAM blocks as well on ESP32 and RP2350
  virtual void release(void* ptr) { free(ptr); }
};

/// Allocator which only uses the internal RAM (e.g. for DMA buffers)
class InternalAllocator : public Allocator {
 public:
  void* allocate(size_t size) override {
    void* p = malloc(size);
    if (p != nullptr) memset(p, 0, size);
    return p;
  }
};

/// default allocator instance
inline Allocator& defaultAllocator() {
  static Allocator allocator;
  return allocator;
}

/**
 * @brief std compatible allocator (e.g. std::vector<T, AllocatorExt<T>>)
 * which uses defaultAllocator(): PSRAM if available.
 */
template <class T>
struct AllocatorExt {
  typedef T value_type;
  AllocatorExt() = default;
  template <class U>
  AllocatorExt(const AllocatorExt<U>&) {}
  T* allocate(size_t n) {
    T* p = (T*)defaultAllocator().allocate(n * sizeof(T));
#if defined(__cpp_exceptions)
    if (p == nullptr) throw std::bad_alloc();
#endif
    return p;
  }
  void deallocate(T* p, size_t) { defaultAllocator().release(p); }
  template <class U>
  bool operator==(const AllocatorExt<U>&) const { return true; }
  template <class U>
  bool operator!=(const AllocatorExt<U>&) const { return false; }
};

}  // namespace tinyext4

#ifndef PSRAM_ALLOCATOR_H
#define PSRAM_ALLOCATOR_H

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <vector>

template <class T> struct PsramAllocator {
  typedef T value_type;

  PsramAllocator() = default;
  template <class U>
  constexpr PsramAllocator(const PsramAllocator<U> &) noexcept {}

  T *allocate(std::size_t n) {
    if (n > std::size_t(-1) / sizeof(T))
      throw std::bad_alloc();
    // Allocate current item count in PSRAM
    void *p = heap_caps_malloc(n * sizeof(T), MALLOC_CAP_SPIRAM);
    if (!p)
      throw std::bad_alloc();
    return static_cast<T *>(p);
  }

  void deallocate(T *p, std::size_t) noexcept { heap_caps_free(p); }
};

template <class T, class U>
bool operator==(const PsramAllocator<T> &, const PsramAllocator<U> &) {
  return true;
}

template <class T, class U>
bool operator!=(const PsramAllocator<T> &, const PsramAllocator<U> &) {
  return false;
}

// Allocator for ArduinoJson (non-template)
struct SpiRamAllocator {
  void *allocate(size_t size) {
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
  }
  void deallocate(void *pointer) { heap_caps_free(pointer); }
  void *reallocate(void *ptr, size_t new_size) {
    return heap_caps_realloc(ptr, new_size, MALLOC_CAP_SPIRAM);
  }
};

#endif

// Общие помощники тестов: CHECK без внешних фреймворков и FakeMemory —
// "память цели" с фиктивным базовым адресом, читаемая через RegionReader так
// же, как снимок чужого процесса.

#pragma once

#include "memrefl/memory_reader.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <vector>

#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
                   #cond);                                                   \
      std::exit(1);                                                          \
    }                                                                        \
  } while (0)

namespace memrefl::testing {

template <class F>
inline bool Throws(F&& f) {
  try {
    f();
  } catch (const std::invalid_argument&) {
    return true;
  }
  return false;
}

// "Память цели": буфер фиксированного размера, адреса начинаются с base.
class FakeMemory {
 public:
  explicit FakeMemory(std::size_t size = 1 << 16, std::uintptr_t base = 0x10000000)
      : bytes_(size, 0), base_(base) {}

  std::uintptr_t Alloc(std::size_t size, std::size_t align = 8) {
    used_ = (used_ + align - 1) / align * align;
    const std::uintptr_t address = base_ + used_;
    used_ += size;
    CHECK(used_ <= bytes_.size());
    return address;
  }

  void Write(std::uintptr_t address, const void* data, std::size_t size) {
    CHECK(address >= base_ && address - base_ + size <= bytes_.size());
    std::memcpy(&bytes_[address - base_], data, size);
  }

  template <class T>
  void Put(std::uintptr_t address, T value) {
    Write(address, &value, sizeof(T));
  }

  void PutPointer(std::uintptr_t address, std::uintptr_t value, std::size_t width) {
    const std::uint64_t wide = value;
    Write(address, &wide, width);  // little-endian: младшие width байт
  }

  // Строка с завершающим нулём; возвращает адрес.
  std::uintptr_t Utf8(std::string_view s) {
    const std::uintptr_t a = Alloc(s.size() + 1, 1);
    Write(a, s.data(), s.size());
    return a;
  }

  std::uintptr_t Utf16(std::u16string_view s) {
    const std::uintptr_t a = Alloc((s.size() + 1) * 2, 2);
    for (std::size_t i = 0; i < s.size(); ++i) Put<std::uint16_t>(a + 2 * i, s[i]);
    return a;
  }

  std::uintptr_t base() const { return base_; }
  std::uintptr_t end() const { return base_ + bytes_.size(); }

  std::shared_ptr<const MemoryReader> Reader() const {
    return std::make_shared<RegionReader>(MemoryRegion::Snapshot(bytes_.data(), bytes_.size(), base_));
  }

 private:
  std::vector<std::uint8_t> bytes_;
  std::uintptr_t base_;
  std::size_t used_ = 0;
};

}  // namespace memrefl::testing

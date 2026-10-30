// memrefl — доступ к памяти исследуемого процесса.
//
// Модуль 1 сканировал заранее известные регионы (секции модуля). Таблицы имён
// (модуль 2) и объекты (модуль 3) разбросаны по куче, а указатели в них могут
// быть мусорными, поэтому чтение идёт через MemoryReader. Он возвращает false
// вместо падения процесса.
//
//   * ProcessMemoryReader::Self() — свой процесс, но безопасно: ReadProcessMemory
//     (Windows) / process_vm_readv (Linux) не падают на невалидном адресе.
//   * ProcessMemoryReader(handle/pid) — другой процесс (нужны права на чтение:
//     PROCESS_VM_READ на Windows, ptrace-доступ на Linux).
//   * RegionReader — снимки и дампы памяти (те же MemoryRegion, что в модуле 1);
//     удобен для офлайн-анализа и тестов.
//
// PointerPath описывает цепочку указателей ("[[base] + 0x10] + 0x8") и
// разрешает её при каждом обращении: массив в живом процессе мог быть
// перевыделен, и закешированный адрес устарел бы.

#pragma once

#include "memrefl/signature_scanner.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__linux__)
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>
#endif

namespace memrefl {

class MemoryReader {
 public:
  virtual ~MemoryReader() = default;

  // Читает size байт по адресу цели. false, если хотя бы один байт недоступен;
  // содержимое out в этом случае не определено.
  virtual bool ReadBytes(std::uintptr_t address, void* out, std::size_t size) const = 0;

  template <class T>
  std::optional<T> Read(std::uintptr_t address) const {
    static_assert(std::is_trivially_copyable_v<T>, "Read<T> requires a trivially copyable type");
    T value;
    if (!ReadBytes(address, &value, sizeof(T))) return std::nullopt;
    return value;
  }

  // Беззнаковое целое шириной 1..8 байт (little-endian, как в x86/ARM).
  std::optional<std::uint64_t> ReadUnsigned(std::uintptr_t address, std::size_t size) const {
    if (size == 0 || size > sizeof(std::uint64_t)) return std::nullopt;
    std::uint64_t value = 0;
    if (!ReadBytes(address, &value, size)) return std::nullopt;
    return value;
  }

  // Указатель цели шириной pointer_size: 8 для 64-битной цели, 4 для 32-битной
  // (например, WOW64-процесс, читаемый 64-битным инструментом).
  std::optional<std::uintptr_t> ReadPointer(std::uintptr_t address, std::size_t pointer_size = sizeof(void*)) const {
    const std::optional<std::uint64_t> value = ReadUnsigned(address, pointer_size);
    if (!value || *value > (std::numeric_limits<std::uintptr_t>::max)()) return std::nullopt;
    return static_cast<std::uintptr_t>(*value);
  }
};

// Чтение из снимков памяти: буферы с адресами цели (см. MemoryRegion в модуле 1).
// Буферы не копируются — изменения в них видны сразу; они должны жить дольше
// читателя.
class RegionReader final : public MemoryReader {
 public:
  explicit RegionReader(std::vector<MemoryRegion> regions) : regions_(NormalizeRegions(std::move(regions))) {}
  explicit RegionReader(MemoryRegion region) : RegionReader(std::vector<MemoryRegion>{region}) {}

  bool ReadBytes(std::uintptr_t address, void* out, std::size_t size) const override {
    if (size == 0) return true;
    const MemoryRegion* region = FindRegion(regions_, address, size);
    if (region == nullptr) return false;
    std::memcpy(out, region->data + (address - region->base), size);
    return true;
  }

  const std::vector<MemoryRegion>& regions() const noexcept { return regions_; }

 private:
  std::vector<MemoryRegion> regions_;
};

#if defined(_WIN32) || defined(__linux__)

#define MEMREFL_HAS_PROCESS_READER 1

class ProcessMemoryReader final : public MemoryReader {
 public:
#if defined(_WIN32)
  // handle с правом PROCESS_VM_READ; владение не передаётся.
  explicit ProcessMemoryReader(HANDLE process) noexcept : process_(process) {}
  static ProcessMemoryReader Self() noexcept { return ProcessMemoryReader(GetCurrentProcess()); }

  bool ReadBytes(std::uintptr_t address, void* out, std::size_t size) const override {
    if (size == 0) return true;
    SIZE_T done = 0;
    return ReadProcessMemory(process_, reinterpret_cast<LPCVOID>(address), out, size, &done) != 0 &&
           done == size;
  }

 private:
  HANDLE process_;
#else
  explicit ProcessMemoryReader(pid_t pid) noexcept : pid_(pid) {}
  static ProcessMemoryReader Self() noexcept { return ProcessMemoryReader(getpid()); }

  bool ReadBytes(std::uintptr_t address, void* out, std::size_t size) const override {
    if (size == 0) return true;
    // Ядро проверяет адрес само: невалидная память даёт EFAULT или частичное
    // чтение, а не SIGSEGV.
    iovec local{out, size};
    iovec remote{reinterpret_cast<void*>(address), size};
    return process_vm_readv(pid_, &local, 1, &remote, 1, 0) == static_cast<ssize_t>(size);
  }

 private:
  pid_t pid_;
#endif
};

#endif

// Адрес, заданный цепочкой указателей. Перед каждым смещением читается
// указатель по текущему адресу:
//   PointerPath(base)               -> base
//   PointerPath(base, {0})          -> *base
//   PointerPath(base, {0x10, 0x8})  -> *(*base + 0x10) + 0x8
// Нулевой указатель на любом шаге — nullopt.
class PointerPath {
 public:
  // Результат с причиной неудачи: на каком шаге и почему оборвалась цепочка.
  struct Trace {
    std::optional<std::uintptr_t> address;
    std::size_t failed_step = 0;  // индекс смещения, перед которым не удалось прочитать указатель
    bool null_pointer = false;    // указатель прочитан, но равен нулю (иначе — память недоступна)
  };

  PointerPath() = default;
  PointerPath(std::uintptr_t base) : base_(base) {}  // NOLINT: неявное — удобно в конфигурациях
  PointerPath(std::uintptr_t base, std::initializer_list<std::ptrdiff_t> offsets) : base_(base), offsets_(offsets) {}

  std::optional<std::uintptr_t> Resolve(const MemoryReader& reader, std::size_t pointer_size = sizeof(void*)) const {
    return ResolveTraced(reader, pointer_size).address;
  }

  Trace ResolveTraced(const MemoryReader& reader, std::size_t pointer_size = sizeof(void*)) const {
    Trace trace;
    std::uintptr_t address = base_;
    for (std::size_t step = 0; step < offsets_.size(); ++step) {
      const std::optional<std::uintptr_t> next = reader.ReadPointer(address, pointer_size);
      if (!next || *next == 0) {
        trace.failed_step = step;
        trace.null_pointer = next.has_value();
        return trace;
      }
      address = *next + static_cast<std::uintptr_t>(offsets_[step]);
    }
    trace.address = address;
    return trace;
  }

  // Построение пути по шагам: сдвиг текущего адреса и переход по указателю.
  PointerPath& Offset(std::ptrdiff_t delta) {
    if (offsets_.empty()) {
      base_ += static_cast<std::uintptr_t>(delta);
    } else {
      offsets_.back() += delta;
    }
    return *this;
  }
  PointerPath& Dereference() {
    offsets_.push_back(0);
    return *this;
  }

  // "[[0x10002000]+0x30]+0x8" — для логов и отчётов.
  std::string ToString() const {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "0x%llX", static_cast<unsigned long long>(base_));
    std::string out = buffer;
    for (const std::ptrdiff_t offset : offsets_) {
      out = "[" + out + "]";
      if (offset != 0) {
        const unsigned long long magnitude =
            offset < 0 ? 0ull - static_cast<unsigned long long>(offset) : static_cast<unsigned long long>(offset);
        std::snprintf(buffer, sizeof(buffer), "%c0x%llX", offset < 0 ? '-' : '+', magnitude);
        out += buffer;
      }
    }
    return out;
  }

  std::uintptr_t base() const noexcept { return base_; }
  const std::vector<std::ptrdiff_t>& offsets() const noexcept { return offsets_; }

 private:
  std::uintptr_t base_ = 0;
  std::vector<std::ptrdiff_t> offsets_;
};

}  // namespace memrefl

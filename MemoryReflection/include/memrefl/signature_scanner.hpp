// memrefl — модуль 1: поиск сигнатур в памяти.
//
// SignatureScanner ищет Pattern в наборе регионов памяти и возвращает адреса
// совпадений. Регион — это байты, которые можно читать в НАШЕМ процессе, плюс
// адрес, под которым они находятся в исследуемом адресном пространстве:
//   * свой процесс:        data == base (MemoryRegion::InProcess);
//   * копия чужой памяти:  буфер после ReadProcessMemory или дамп, base — адрес
//                          в цели (MemoryRegion::Snapshot);
//   * файл на диске:       секции PE-файла с адресами как в IDA
//                          (PeImageRegions из module_regions.hpp).
// Найденные адреса, Read() и ResolveRelative() работают в адресах цели, поэтому
// код поиска одинаков для всех трёх случаев.
//
// Алгоритм для каждого региона:
//   1. Pattern заранее выбрал два "якоря" — самые редкие полностью заданные байты.
//   2. SSE2: одна итерация проверяет 16 позиций начала сразу: два невыровненных
//      чтения по смещениям якорей, два сравнения, AND, movemask. Остаются только
//      позиции, где совпали ОБА якоря, — в реальном коде это единицы на килобайт.
//   3. Без SSE2 или при единственном якоре: memchr по редкому байту (в
//      стандартных библиотеках он векторизован).
//   4. Каждый кандидат проверяется целиком: (byte & mask) == value.
//   Паттерн без полностью заданных байтов (только маски вида "4?") проверяется
//   в каждой позиции.
//
// Совпадения могут перекрываться ("AB AB" в "AB AB AB" — два совпадения) и
// возвращаются по возрастанию адреса. Смежные регионы склеиваются, поэтому
// сигнатура, пересекающая их границу, тоже находится.
//
// После создания сканер не меняется: константные методы можно вызывать из
// нескольких потоков одновременно.

#pragma once

#include "memrefl/pattern.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#if !defined(MEMREFL_NO_SIMD) &&                                                     \
    (defined(__SSE2__) || (defined(_M_X64) && !defined(_M_ARM64EC)) || \
     (defined(_M_IX86_FP) && _M_IX86_FP >= 2))
#define MEMREFL_HAS_SSE2 1
#include <emmintrin.h>
#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#endif
#endif

namespace memrefl {

struct MemoryRegion {
  std::uintptr_t base = 0;             // адрес data[0] в исследуемом адресном пространстве
  const std::uint8_t* data = nullptr;  // откуда читать байты в нашем процессе
  std::size_t size = 0;

  // Память своего процесса: адреса совпадают с указателями.
  static MemoryRegion InProcess(const void* address, std::size_t size) noexcept {
    return {reinterpret_cast<std::uintptr_t>(address), static_cast<const std::uint8_t*>(address), size};
  }

  // Копия чужой памяти (ReadProcessMemory, дамп): байты лежат в copy,
  // а в цели начинаются с remote_base.
  static MemoryRegion Snapshot(const void* copy, std::size_t size, std::uintptr_t remote_base) noexcept {
    return {remote_base, static_cast<const std::uint8_t*>(copy), size};
  }
};

// Сортирует регионы по адресу, отбрасывает пустые и склеивает смежные (и по
// адресу в цели, и по расположению байтов у нас). Регионы не должны перекрываться.
inline std::vector<MemoryRegion> NormalizeRegions(std::vector<MemoryRegion> regions) {
  regions.erase(std::remove_if(regions.begin(), regions.end(),
                               [](const MemoryRegion& r) { return r.size == 0 || r.data == nullptr; }),
                regions.end());
  std::sort(regions.begin(), regions.end(),
            [](const MemoryRegion& a, const MemoryRegion& b) { return a.base < b.base; });
  std::vector<MemoryRegion> merged;
  for (const MemoryRegion& r : regions) {
    if (!merged.empty()) {
      MemoryRegion& last = merged.back();
      const bool contiguous =
          last.base + last.size == r.base &&
          reinterpret_cast<std::uintptr_t>(last.data) + last.size == reinterpret_cast<std::uintptr_t>(r.data);
      if (contiguous) {
        last.size += r.size;
        continue;
      }
    }
    merged.push_back(r);
  }
  return merged;
}

// Регион из нормализованного списка, целиком содержащий [address, address + size).
inline const MemoryRegion* FindRegion(const std::vector<MemoryRegion>& regions, std::uintptr_t address,
                                      std::size_t size) noexcept {
  // Первый регион, начинающийся строго после address; нужный — перед ним.
  const auto it = std::upper_bound(regions.begin(), regions.end(), address,
                                   [](std::uintptr_t a, const MemoryRegion& r) { return a < r.base; });
  if (it == regions.begin()) return nullptr;
  const MemoryRegion& region = *std::prev(it);
  const std::uintptr_t offset = address - region.base;
  if (offset >= region.size || size > region.size - offset) return nullptr;
  return &region;
}

namespace detail {

// Все функции сканирования получают count — число допустимых позиций начала
// (size - pattern.size() + 1) — и вызывают on_match(offset) для каждого
// совпадения. on_match возвращает false, чтобы прервать поиск; тогда и сама
// функция возвращает false.

template <class OnMatch>
bool ScanNaive(const std::uint8_t* data, std::size_t count, const Pattern& p, OnMatch& on_match) {
  for (std::size_t i = 0; i < count; ++i) {
    if (p.MatchesAt(data + i) && !on_match(i)) return false;
  }
  return true;
}

template <class OnMatch>
bool ScanWithMemchr(const std::uint8_t* data, std::size_t count, const Pattern& p, OnMatch& on_match) {
  const std::size_t a = p.anchor();
  // Якорь кандидата i лежит в data[i + a], поэтому ищем его в [a, a + count).
  const std::uint8_t* cur = data + a;
  const std::uint8_t* const end = data + a + count;
  while (cur < end) {
    const auto* hit = static_cast<const std::uint8_t*>(
        std::memchr(cur, p.values()[a], static_cast<std::size_t>(end - cur)));
    if (hit == nullptr) break;
    const std::size_t pos = static_cast<std::size_t>(hit - data) - a;
    if (p.MatchesAt(data + pos) && !on_match(pos)) return false;
    cur = hit + 1;
  }
  return true;
}

#if defined(MEMREFL_HAS_SSE2)

inline unsigned CountTrailingZeros(unsigned v) noexcept {
#if defined(_MSC_VER) && !defined(__clang__)
  unsigned long index;
  _BitScanForward(&index, v);
  return static_cast<unsigned>(index);
#else
  return static_cast<unsigned>(__builtin_ctz(v));
#endif
}

template <class OnMatch>
bool ScanWithSse2(const std::uint8_t* data, std::size_t count, const Pattern& p, OnMatch& on_match) {
  const std::uint8_t* const first = data + p.anchor();
  const std::uint8_t* const second = data + p.second_anchor();
  const __m128i v1 = _mm_set1_epi8(static_cast<char>(p.values()[p.anchor()]));
  const __m128i v2 = _mm_set1_epi8(static_cast<char>(p.values()[p.second_anchor()]));

  // Блок из 16 позиций начала [i, i + 16): байт 0xFF там, где совпали оба
  // якоря. Блок читается только при i + 16 <= count, и тогда самое дальнее
  // чтение i + 15 + якорь не выходит за регион, т.к. якорь < p.size().
  auto block = [&](std::size_t i) {
    const __m128i b1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(first + i));
    const __m128i b2 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(second + i));
    return _mm_and_si128(_mm_cmpeq_epi8(b1, v1), _mm_cmpeq_epi8(b2, v2));
  };
  // Полная проверка кандидатов блока; бит k в bits — позиция i + k.
  auto verify = [&](std::size_t i, unsigned bits) {
    while (bits != 0) {
      const std::size_t pos = i + CountTrailingZeros(bits);
      if (p.MatchesAt(data + pos) && !on_match(pos)) return false;
      bits &= bits - 1;  // снять младший бит
    }
    return true;
  };
  auto bits_of = [](__m128i mask) { return static_cast<unsigned>(_mm_movemask_epi8(mask)); };

  std::size_t i = 0;
  // Основной цикл — 64 позиции за итерацию. Кандидаты в реальном коде редки,
  // поэтому обычно хватает одной проверки "в 4 блоках пусто".
  for (; i + 64 <= count; i += 64) {
    const __m128i m0 = block(i), m1 = block(i + 16), m2 = block(i + 32), m3 = block(i + 48);
    if (bits_of(_mm_or_si128(_mm_or_si128(m0, m1), _mm_or_si128(m2, m3))) == 0) continue;
    if (!verify(i, bits_of(m0)) || !verify(i + 16, bits_of(m1)) || !verify(i + 32, bits_of(m2)) ||
        !verify(i + 48, bits_of(m3))) {
      return false;
    }
  }
  for (; i + 16 <= count; i += 16) {
    if (!verify(i, bits_of(block(i)))) return false;
  }
  // Хвост короче 16 позиций.
  for (; i < count; ++i) {
    if (p.MatchesAt(data + i) && !on_match(i)) return false;
  }
  return true;
}

#endif  // MEMREFL_HAS_SSE2

template <class OnMatch>
bool ScanBytes(const std::uint8_t* data, std::size_t size, const Pattern& p, OnMatch& on_match) {
  if (p.size() == 0 || size < p.size()) return true;
  const std::size_t count = size - p.size() + 1;
#if defined(MEMREFL_HAS_SSE2)
  if (p.second_anchor() != Pattern::npos) return ScanWithSse2(data, count, p, on_match);
#endif
  if (p.anchor() != Pattern::npos) return ScanWithMemchr(data, count, p, on_match);
  return ScanNaive(data, count, p, on_match);
}

}  // namespace detail

class SignatureScanner {
 public:
  static constexpr std::size_t npos = static_cast<std::size_t>(-1);

  SignatureScanner() = default;

  explicit SignatureScanner(MemoryRegion region)
      : SignatureScanner(std::vector<MemoryRegion>{region}) {}

  // Регионы нормализуются (см. NormalizeRegions).
  explicit SignatureScanner(std::vector<MemoryRegion> regions)
      : regions_(NormalizeRegions(std::move(regions))) {}

  const std::vector<MemoryRegion>& regions() const noexcept { return regions_; }
  bool empty() const noexcept { return regions_.empty(); }

  // --- Поиск -----------------------------------------------------------------

  // Вызывает callback(address) для каждого совпадения по возрастанию адреса.
  // Если callback возвращает bool, false прекращает поиск.
  template <class F>
  void ForEachMatch(const Pattern& pattern, F&& callback) const {
    for (const MemoryRegion& region : regions_) {
      auto on_match = [&](std::size_t offset) -> bool {
        const std::uintptr_t address = region.base + offset;
        if constexpr (std::is_void_v<std::invoke_result_t<F&, std::uintptr_t>>) {
          callback(address);
          return true;
        } else {
          return static_cast<bool>(callback(address));
        }
      };
      if (!detail::ScanBytes(region.data, region.size, pattern, on_match)) return;
    }
  }

  std::optional<std::uintptr_t> FindFirst(const Pattern& pattern) const {
    std::optional<std::uintptr_t> result;
    ForEachMatch(pattern, [&](std::uintptr_t address) {
      result = address;
      return false;
    });
    return result;
  }

  std::vector<std::uintptr_t> FindAll(const Pattern& pattern, std::size_t max_results = npos) const {
    std::vector<std::uintptr_t> result;
    if (max_results == 0) return result;
    ForEachMatch(pattern, [&](std::uintptr_t address) {
      result.push_back(address);
      return result.size() < max_results;
    });
    return result;
  }

  // Адрес, только если совпадение ровно одно. Основной метод для сигнатур
  // функций: если после обновления сигнатура стала находить два места,
  // молча взять первое — значит получить неверный адрес.
  std::optional<std::uintptr_t> FindUnique(const Pattern& pattern) const {
    const std::vector<std::uintptr_t> matches = FindAll(pattern, 2);
    if (matches.size() != 1) return std::nullopt;
    return matches.front();
  }

  // Перегрузки для строк разбирают сигнатуру при каждом вызове и бросают
  // std::invalid_argument при ошибке формата. Для повторного поиска храните Pattern.
  std::optional<std::uintptr_t> FindFirst(std::string_view signature) const {
    return FindFirst(Pattern(signature));
  }
  std::vector<std::uintptr_t> FindAll(std::string_view signature, std::size_t max_results = npos) const {
    return FindAll(Pattern(signature), max_results);
  }
  std::optional<std::uintptr_t> FindUnique(std::string_view signature) const {
    return FindUnique(Pattern(signature));
  }

  // --- Чтение по адресам цели -------------------------------------------------

  bool Contains(std::uintptr_t address, std::size_t size = 1) const noexcept {
    return RegionFor(address, size) != nullptr;
  }

  // Читает T по адресу цели; nullopt, если байты не лежат целиком в одном регионе.
  template <class T>
  std::optional<T> Read(std::uintptr_t address) const noexcept {
    static_assert(std::is_trivially_copyable_v<T>, "Read<T> requires a trivially copyable type");
    const MemoryRegion* region = RegionFor(address, sizeof(T));
    if (region == nullptr) return std::nullopt;
    T value;
    std::memcpy(&value, region->data + (address - region->base), sizeof(T));
    return value;
  }

  // Цель инструкции с 32-битным относительным смещением:
  //   E8 rel32               call  target        -> ResolveRelative(addr, 1, 5)
  //   E9 rel32               jmp   target        -> ResolveRelative(addr, 1, 5)
  //   48 8B 05 disp32        mov   rax, [rip+d]  -> ResolveRelative(addr, 3, 7)
  //   48 8D 0D disp32        lea   rcx, [rip+d]  -> ResolveRelative(addr, 3, 7)
  // target = адрес инструкции + её длина + disp32 (смещение знаковое).
  std::optional<std::uintptr_t> ResolveRelative(std::uintptr_t instruction, std::size_t disp_offset,
                                                std::size_t instruction_size) const noexcept {
    const std::optional<std::int32_t> disp = Read<std::int32_t>(instruction + disp_offset);
    if (!disp) return std::nullopt;
    return instruction + instruction_size + static_cast<std::uintptr_t>(static_cast<std::intptr_t>(*disp));
  }

 private:
  const MemoryRegion* RegionFor(std::uintptr_t address, std::size_t size) const noexcept {
    return FindRegion(regions_, address, size);
  }

  std::vector<MemoryRegion> regions_;  // по возрастанию base, без перекрытий
};

}  // namespace memrefl

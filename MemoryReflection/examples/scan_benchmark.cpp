// Скорость поиска: наивный перебор против SignatureScanner на реальном
// машинном коде. Берём исполняемые секции загруженных модулей (своего exe и
// системной библиотеки), копируем подряд до ~64 МБ и ищем типичные сигнатуры.
// Оба способа считают ВСЕ совпадения, поэтому их количество должно совпасть.

#include "memrefl/module_regions.hpp"
#include "memrefl/signature_scanner.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

namespace {

constexpr std::size_t kTargetBytes = 64u << 20;

std::vector<std::uint8_t> CollectCodeBytes() {
  std::vector<std::uint8_t> code;
#if defined(MEMREFL_HAS_LOADED_MODULES)
#if defined(_WIN32)
  const char* const modules[] = {"", "ntdll.dll", "kernel32.dll"};
#else
  const char* const modules[] = {"", "libc.so.6", "libstdc++.so.6"};
#endif
  for (const char* name : modules) {
    for (const memrefl::MemoryRegion& r : memrefl::LoadedModuleRegions(name)) {
      code.insert(code.end(), r.data, r.data + r.size);
    }
  }
#endif
  if (code.empty()) {
    // Нет доступа к модулям — синтетика с "кодовым" распределением байтов.
    std::mt19937 rng(1);
    const std::uint8_t common[] = {0x48, 0x8B, 0x89, 0x00, 0x0F, 0xFF, 0xE8, 0x24, 0x83, 0xC0, 0x4C, 0x8D};
    code.resize(1 << 20);
    for (auto& b : code) b = rng() % 2 ? common[rng() % sizeof(common)] : static_cast<std::uint8_t>(rng());
  }
  // Размножаем до kTargetBytes: копируем начало буфера в его конец.
  const std::size_t unique = code.size();
  while (code.size() < kTargetBytes) {
    const std::size_t old_size = code.size();
    const std::size_t chunk = (std::min)(unique, kTargetBytes - old_size);
    code.resize(old_size + chunk);
    std::copy_n(code.begin(), chunk, code.begin() + static_cast<std::ptrdiff_t>(old_size));
  }
  return code;
}

std::size_t NaiveCount(const std::vector<std::uint8_t>& data, const memrefl::Pattern& p) {
  std::size_t count = 0;
  for (std::size_t i = 0; i + p.size() <= data.size(); ++i) count += p.MatchesAt(&data[i]) ? 1 : 0;
  return count;
}

template <class F>
double BestSeconds(F&& run) {
  double best = 1e9;
  for (int i = 0; i < 3; ++i) {
    const auto start = std::chrono::steady_clock::now();
    run();
    best = (std::min)(best, std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
  }
  return best;
}

}  // namespace

int main() {
  const std::vector<std::uint8_t> data = CollectCodeBytes();
  const memrefl::SignatureScanner scanner(memrefl::MemoryRegion::InProcess(data.data(), data.size()));
  const double megabytes = static_cast<double>(data.size()) / (1 << 20);
  std::printf("scanning %.1f MB of machine code (%s)\n\n", megabytes,
#if defined(MEMREFL_HAS_SSE2)
              "SSE2 two-anchor filter"
#else
              "memchr anchor"
#endif
  );

  const char* const signatures[] = {
      "48 8B 05 ?? ?? ?? ?? 48 85 C0 74 ??",  // загрузка глобальной переменной + проверка на null
      "48 89 5C 24 ?? 57 48 83 EC ??",        // типичный пролог функции MSVC x64
      "E8 ?? ?? ?? ?? 48 8B D8",              // call + сохранение результата
      "55 48 89 E5 41 57",                    // пролог GCC с frame pointer
      "DE AD BE EF ?? 13 37",                 // отсутствует: полный проход
  };

  std::printf("%-40s %9s %12s %12s %8s\n", "signature", "matches", "naive MB/s", "scanner MB/s", "speedup");
  for (const char* text : signatures) {
    const memrefl::Pattern pattern(text);
    std::size_t naive_matches = 0;
    std::size_t scanner_matches = 0;
    const double naive = BestSeconds([&] { naive_matches = NaiveCount(data, pattern); });
    const double fast = BestSeconds([&] { scanner_matches = scanner.FindAll(pattern).size(); });
    if (naive_matches != scanner_matches) {
      std::printf("MISMATCH for %s: naive %zu, scanner %zu\n", text, naive_matches, scanner_matches);
      return 1;
    }
    std::printf("%-40s %9zu %12.0f %12.0f %7.1fx\n", text, scanner_matches, megabytes / naive, megabytes / fast,
                naive / fast);
  }
  return 0;
}

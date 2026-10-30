// Тесты модуля 1 (поиск сигнатур) без внешних зависимостей.
// Собираются дважды: с SSE2 и с -DMEMREFL_NO_SIMD, чтобы проверить оба пути
// сканирования. Главный тест — сравнение со "наивным" эталоном на случайных
// данных: оптимизации не должны менять результат.

#include "memrefl/module_regions.hpp"
#include "memrefl/pattern.hpp"
#include "memrefl/signature_scanner.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
                   #cond);                                                   \
      std::exit(1);                                                          \
    }                                                                        \
  } while (0)

using memrefl::MemoryRegion;
using memrefl::Pattern;
using memrefl::SignatureScanner;
using Bytes = std::vector<std::uint8_t>;

// Эталон: проверка паттерна в каждой позиции, без якорей и SIMD.
static std::vector<std::uintptr_t> NaiveFindAll(const Bytes& data, std::uintptr_t base, const Pattern& p) {
  std::vector<std::uintptr_t> out;
  for (std::size_t i = 0; i + p.size() <= data.size(); ++i) {
    bool ok = true;
    for (std::size_t k = 0; k < p.size() && ok; ++k) ok = (data[i + k] & p.masks()[k]) == p.values()[k];
    if (ok) out.push_back(base + i);
  }
  return out;
}

static SignatureScanner ScannerFor(const Bytes& data, std::uintptr_t base) {
  return SignatureScanner(MemoryRegion::Snapshot(data.data(), data.size(), base));
}

template <class T>
static void Put(Bytes& data, std::size_t offset, T value) {
  std::memcpy(data.data() + offset, &value, sizeof(T));
}

// --- Pattern -----------------------------------------------------------------

static void TestParseFormats() {
  const Pattern p("48 89 F8 ?? 0F 1F");
  CHECK(p.size() == 6);
  CHECK(p.ToString() == "48 89 F8 ?? 0F 1F");
  CHECK(p.values()[0] == 0x48 && p.masks()[0] == 0xFF);
  CHECK(p.masks()[3] == 0x00);

  // Одиночный '?', нижний регистр, слитная запись, лишние пробелы.
  CHECK(Pattern("48 89 f8 ? 0f 1f").ToString() == p.ToString());
  CHECK(Pattern("  4889F8??0F1F \t\r\n").ToString() == p.ToString());
  CHECK(Pattern("4889 F8 ?? 0F1F").ToString() == p.ToString());

  // Полубайтовые маски.
  const Pattern nibbles("4? ?F");
  CHECK(nibbles.values()[0] == 0x40 && nibbles.masks()[0] == 0xF0);
  CHECK(nibbles.values()[1] == 0x0F && nibbles.masks()[1] == 0x0F);
  CHECK(nibbles.ToString() == "4? ?F");
  CHECK(nibbles.anchor() == Pattern::npos);
}

static void TestParseErrors() {
  const char* const bad[] = {"", "   ", "??", "? ?? ?", "4", "48 8", "488", "GG", "48 0x8B", "48-8B", "48,8B"};
  for (const char* text : bad) {
    std::string error;
    CHECK(!Pattern::TryParse(text, &error));
    CHECK(!error.empty());
    bool threw = false;
    try {
      Pattern p(text);
    } catch (const std::invalid_argument&) {
      threw = true;
    }
    CHECK(threw);
  }
  std::string error;
  CHECK(!Pattern::TryParse("48 8B XY", &error));
  CHECK(error.find("\"XY\" at offset 6") != std::string::npos);
  CHECK(Pattern::TryParse("48 8B", nullptr).has_value());
}

static void TestAnchors() {
  // 05 — "редкий" байт, 85 и C0 — "частые", 48 и 8B — "очень частые".
  const Pattern p("48 8B 05 ?? ?? ?? ?? 48 85 C0");
  CHECK(p.anchor() == 2);
  CHECK(p.second_anchor() == 8);

  const Pattern single("?? E8 ??");
  CHECK(single.anchor() == 1);
  CHECK(single.second_anchor() == Pattern::npos);

  // При равной частоте выбираются более ранние байты.
  const Pattern same("11 22 33");
  CHECK(same.anchor() == 0 && same.second_anchor() == 1);
}

static void TestCodeStyle() {
  const Pattern p = Pattern::FromCodeStyle("\x48\x8B\x05\x00\x00\x00\x00\xC3", "xxx????x");
  CHECK(p.ToString() == "48 8B 05 ?? ?? ?? ?? C3");

  bool threw = false;
  try {
    Pattern::FromCodeStyle("\x48\x8B", "x.");
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  CHECK(threw);

  threw = false;
  try {
    Pattern::FromCodeStyle("\x48\x8B", "??");
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  CHECK(threw);
}

// --- Поиск ---------------------------------------------------------------------

static void TestFindBasic() {
  const std::uintptr_t base = 0x401000;
  const Bytes data = {0x90, 0x48, 0x8B, 0x05, 0x11, 0x22, 0x33, 0x44, 0xC3,
                      0x48, 0x8B, 0x05, 0x55, 0x66, 0x77, 0x88, 0xC3};
  const SignatureScanner scanner = ScannerFor(data, base);

  CHECK(scanner.FindFirst("48 8B 05 ?? ?? ?? ??") == base + 1);
  CHECK((scanner.FindAll("48 8B 05 ?? ?? ?? ??") == std::vector<std::uintptr_t>{base + 1, base + 9}));
  CHECK(scanner.FindAll("48 8B 05 ?? ?? ?? ??", 1).size() == 1);
  CHECK(scanner.FindAll("48 8B 05", 0).empty());
  CHECK(!scanner.FindUnique("48 8B 05 ?? ?? ?? ??"));  // два места — неоднозначно
  CHECK(scanner.FindUnique("48 8B 05 5? 66") == base + 9);
  CHECK(!scanner.FindFirst("48 8B 06"));

  // Паттерн длиннее данных и пустой сканер.
  CHECK(!ScannerFor(Bytes{0x48, 0x8B}, base).FindFirst("48 8B 05"));
  CHECK(!SignatureScanner().FindFirst("48"));

  // ForEachMatch с void-обработчиком и с досрочной остановкой.
  int calls = 0;
  scanner.ForEachMatch(Pattern("C3"), [&](std::uintptr_t) { ++calls; });
  CHECK(calls == 2);
  calls = 0;
  scanner.ForEachMatch(Pattern("C3"), [&](std::uintptr_t) { return ++calls < 1; });
  CHECK(calls == 1);
}

static void TestEdgesAndOverlaps() {
  // Маски в начале и в конце: окно должно целиком помещаться в данные.
  CHECK(ScannerFor(Bytes{0x11, 0x48}, 0).FindFirst("?? 48") == 0u);
  CHECK(!ScannerFor(Bytes{0x48, 0x11}, 0).FindFirst("?? 48"));
  CHECK(ScannerFor(Bytes{0x48, 0x11}, 0).FindFirst("48 ??") == 0u);
  CHECK(!ScannerFor(Bytes{0x11, 0x48}, 0).FindFirst("48 ??"));

  // Совпадение в самой последней позиции длинного буфера (хвост после SIMD-блоков).
  Bytes tail(100, 0x00);
  tail[97] = 0xDE;
  tail[98] = 0xAD;
  tail[99] = 0xBE;
  CHECK(ScannerFor(tail, 0x1000).FindUnique("DE AD BE") == 0x1000u + 97);

  // Перекрывающиеся совпадения возвращаются все.
  const Bytes same(5, 0xAB);
  CHECK((ScannerFor(same, 0).FindAll("AB AB") == std::vector<std::uintptr_t>{0, 1, 2, 3}));
  CHECK(ScannerFor(same, 0).FindAll("AB ?? AB").size() == 3);
}

// Случайные данные из маленького алфавита дают много частичных совпадений,
// а случайные паттерны покрывают все пути: 0, 1 и 2+ якоря, маски по краям,
// размеры меньше, равные и больше 16-байтового SIMD-блока.
static void TestMatchesNaiveOnRandomData() {
  std::mt19937 rng(12345);
  const std::uint8_t alphabet[] = {0x00, 0x48, 0x8B, 0x05, 0xE8, 0xAB};
  const std::uintptr_t base = 0x10000000;
  auto rand_below = [&](std::size_t n) { return static_cast<std::size_t>(rng() % n); };

  for (int iteration = 0; iteration < 4000; ++iteration) {
    const std::size_t size = iteration % 10 == 0 ? 1000 + rand_below(4000) : rand_below(80);
    Bytes data(size);
    for (auto& b : data) b = alphabet[rand_below(sizeof(alphabet))];
    // Иногда — произвольные байты, чтобы полубайтовые маски тоже совпадали по-разному.
    if (iteration % 7 == 0) {
      for (auto& b : data) b = static_cast<std::uint8_t>(rng());
    }

    const std::size_t length = 1 + rand_below(12);
    const bool copy_from_data = size >= length && rand_below(2) == 0;
    const std::size_t from = copy_from_data ? rand_below(size - length + 1) : 0;

    std::string text;
    bool has_fixed_bits = false;
    for (std::size_t k = 0; k < length; ++k) {
      const std::uint8_t byte = copy_from_data ? data[from + k] : alphabet[rand_below(sizeof(alphabet))];
      char token[4];
      const std::size_t kind = rand_below(10);
      if (kind < 2) {
        std::snprintf(token, sizeof(token), "??");
      } else if (kind == 2) {
        std::snprintf(token, sizeof(token), "%X?", byte >> 4);
        has_fixed_bits = true;
      } else if (kind == 3) {
        std::snprintf(token, sizeof(token), "?%X", byte & 0x0F);
        has_fixed_bits = true;
      } else {
        std::snprintf(token, sizeof(token), "%02X", byte);
        has_fixed_bits = true;
      }
      text += token;
      text += ' ';
    }
    const bool forced = !has_fixed_bits;
    if (forced) text.replace(0, 2, "48");

    const Pattern pattern(text);
    const SignatureScanner scanner = ScannerFor(data, base);
    const std::vector<std::uintptr_t> expected = NaiveFindAll(data, base, pattern);
    const std::vector<std::uintptr_t> actual = scanner.FindAll(pattern);
    if (actual != expected) {
      std::fprintf(stderr, "mismatch: pattern \"%s\", size %zu, expected %zu, got %zu\n", text.c_str(), size,
                   expected.size(), actual.size());
    }
    CHECK(actual == expected);
    CHECK(scanner.FindFirst(pattern) == (expected.empty() ? std::nullopt : std::optional(expected.front())));
    if (copy_from_data && !forced) CHECK(!expected.empty());
  }
}

static void TestRegions() {
  Bytes buffer(32, 0x00);
  buffer[14] = 0xCA;
  buffer[15] = 0xFE;
  buffer[16] = 0xBA;
  buffer[17] = 0xBE;
  buffer[20] = 0x77;
  const MemoryRegion first = MemoryRegion::Snapshot(buffer.data(), 16, 0x1000);
  const MemoryRegion second_contiguous = MemoryRegion::Snapshot(buffer.data() + 16, 16, 0x1010);
  const MemoryRegion second_far = MemoryRegion::Snapshot(buffer.data() + 16, 16, 0x5000);

  // Смежные регионы склеиваются (порядок на входе не важен), и сигнатура
  // на их границе находится.
  const SignatureScanner merged({second_contiguous, first, MemoryRegion{}});
  CHECK(merged.regions().size() == 1);
  CHECK(merged.regions()[0].size == 32);
  CHECK(merged.FindUnique("CA FE BA BE") == 0x100Eu);

  // Несмежные — нет: через разрыв совпадения не бывает, адреса — по своей базе.
  const SignatureScanner split({second_far, first});
  CHECK(split.regions().size() == 2);
  CHECK(!split.FindFirst("CA FE BA BE"));
  CHECK(split.FindUnique("77") == 0x5004u);
  CHECK(split.FindAll("00 00").front() == 0x1000u);  // результаты по возрастанию адреса
}

static void TestReadAndResolveRelative() {
  const std::uintptr_t base = 0x401000;
  Bytes code(13, 0x00);
  code[0] = 0xE8;  // call rel32
  Put<std::int32_t>(code, 1, 0x100);
  code[5] = 0x48;  // mov rax, [rip + disp32]
  code[6] = 0x8B;
  code[7] = 0x05;
  Put<std::int32_t>(code, 8, -0x20);
  code[12] = 0xC3;
  const SignatureScanner scanner = ScannerFor(code, base);

  const auto call = scanner.FindUnique("E8 ?? ?? ?? ?? 48 8B 05");
  CHECK(call == base);
  CHECK(scanner.ResolveRelative(*call, 1, 5) == base + 5 + 0x100);

  const auto load = scanner.FindUnique("48 8B 05 ?? ?? ?? ?? C3");
  CHECK(load == base + 5);
  CHECK(scanner.ResolveRelative(*load, 3, 7) == base + 12 - 0x20);  // отрицательное смещение

  CHECK(scanner.Read<std::uint32_t>(base + 1) == 0x100u);
  CHECK(scanner.Read<std::uint8_t>(base + 12) == 0xC3);
  CHECK(!scanner.Read<std::uint32_t>(base + 10));  // выходит за конец на 1 байт
  CHECK(!scanner.Read<std::uint8_t>(base - 1));
  CHECK(!scanner.ResolveRelative(base + 11, 1, 5));
  CHECK(scanner.Contains(base) && scanner.Contains(base, 13) && !scanner.Contains(base, 14));
}

// --- PE ----------------------------------------------------------------------

// Минимальный синтетический PE-образ: .text (код) и .rdata (строка).
static Bytes BuildPe(bool pe64, memrefl::PeLayout layout) {
  const bool mapped = layout == memrefl::PeLayout::Mapped;
  Bytes img(mapped ? 0x3000 : 0x800, 0x00);
  Put<std::uint16_t>(img, 0, 0x5A4D);
  Put<std::uint32_t>(img, 0x3C, 0x80);
  Put<std::uint32_t>(img, 0x80, 0x00004550);
  const std::size_t file_header = 0x84;
  const std::uint16_t optional_size = pe64 ? 0xF0 : 0xE0;
  Put<std::uint16_t>(img, file_header, pe64 ? 0x8664 : 0x014C);
  Put<std::uint16_t>(img, file_header + 2, 2);
  Put<std::uint16_t>(img, file_header + 16, optional_size);
  const std::size_t optional = file_header + 20;
  Put<std::uint16_t>(img, optional, pe64 ? 0x20B : 0x10B);
  if (pe64) {
    Put<std::uint64_t>(img, optional + 24, 0x10000000);
  } else {
    Put<std::uint32_t>(img, optional + 28, 0x10000000);
  }
  Put<std::uint32_t>(img, optional + 56, 0x3000);
  Put<std::uint32_t>(img, optional + 60, 0x400);

  struct Section {
    const char* name;
    std::uint32_t vsize, va, raw_size, raw_offset, flags;
  };
  const Section sections[] = {
      {".text", 0x100, 0x1000, 0x200, 0x400, 0x60000020},
      {".rdata", 0x80, 0x2000, 0x200, 0x600, 0x40000040},
  };
  std::size_t entry = optional + optional_size;
  for (const Section& s : sections) {
    std::memcpy(img.data() + entry, s.name, std::strlen(s.name));
    Put(img, entry + 8, s.vsize);
    Put(img, entry + 12, s.va);
    Put(img, entry + 16, s.raw_size);
    Put(img, entry + 20, s.raw_offset);
    Put(img, entry + 36, s.flags);
    entry += 40;
  }
  const std::size_t text = mapped ? 0x1000 : 0x400;
  const std::size_t rdata = mapped ? 0x2000 : 0x600;
  const std::uint8_t code[] = {0x48, 0x8B, 0x05, 0x10, 0x20, 0x30, 0x40, 0x48, 0x85, 0xC0, 0x74, 0x05};
  std::memcpy(img.data() + text + 0x10, code, sizeof(code));
  std::memcpy(img.data() + rdata + 0x20, "hello-rdata", 11);
  return img;
}

static void TestPeImage() {
  using memrefl::PeLayout;
  using memrefl::SectionFilter;
  const char* const code_sig = "48 8B 05 ?? ?? ?? ?? 48 85 C0 74 ??";
  const char* const text_sig = "68 65 6C 6C 6F 2D 72 64 61 74 61";  // "hello-rdata"

  for (const bool pe64 : {true, false}) {
    const Bytes file = BuildPe(pe64, PeLayout::File);
    const auto headers = memrefl::ParsePeHeaders(file.data(), file.size());
    CHECK(headers);
    CHECK(headers->is_64 == pe64);
    CHECK(headers->image_base == 0x10000000u);
    CHECK(headers->size_of_image == 0x3000u);
    CHECK(headers->sections.size() == 2);
    CHECK(headers->sections[0].name == ".text" && headers->sections[0].IsExecutable());
    CHECK(headers->sections[1].name == ".rdata" && !headers->sections[1].IsExecutable());

    // Файл с диска: адреса как в IDA (ImageBase + RVA), секция — до VirtualSize.
    const auto code_regions = memrefl::PeImageRegions(file.data(), file.size(), PeLayout::File);
    CHECK(code_regions.size() == 1);
    CHECK(code_regions[0].base == 0x10001000u && code_regions[0].size == 0x100);
    const SignatureScanner code_only(code_regions);
    CHECK(code_only.FindUnique(code_sig) == 0x10001010u);
    CHECK(!code_only.FindFirst(text_sig));

    const SignatureScanner everything(memrefl::PeImageRegions(file.data(), file.size(), PeLayout::File,
                                                              SectionFilter::All));
    CHECK(everything.FindUnique(text_sig) == 0x10002020u);

    // Дамп загруженного модуля с другой базой (ASLR).
    const Bytes dump = BuildPe(pe64, PeLayout::Mapped);
    const SignatureScanner from_dump(memrefl::PeImageRegions(dump.data(), dump.size(), PeLayout::Mapped,
                                                             SectionFilter::All, 0x20000000));
    CHECK(from_dump.FindUnique(code_sig) == 0x20001010u);
    CHECK(from_dump.FindUnique(text_sig) == 0x20002020u);
    CHECK(from_dump.ResolveRelative(0x20001010, 3, 7) == 0x20001017u + 0x40302010u);
  }

  // Повреждённые заголовки: ни одного чтения за границу, просто nullopt.
  Bytes pe = BuildPe(true, PeLayout::File);
  CHECK(!memrefl::ParsePeHeaders(pe.data(), 0x100));  // таблица секций обрезана
  CHECK(memrefl::PeImageRegions(pe.data(), 0x100, PeLayout::File).empty());
  CHECK(!memrefl::ParsePeHeaders(pe.data(), 1));

  Bytes bad = pe;
  bad[0] = 'X';
  CHECK(!memrefl::ParsePeHeaders(bad.data(), bad.size()));
  bad = pe;
  Put<std::uint32_t>(bad, 0x3C, 0xFFFFFFF0);  // e_lfanew далеко за пределами
  CHECK(!memrefl::ParsePeHeaders(bad.data(), bad.size()));
  bad = pe;
  Put<std::uint32_t>(bad, 0x80, 0x12345678);  // не "PE\0\0"
  CHECK(!memrefl::ParsePeHeaders(bad.data(), bad.size()));
  bad = pe;
  Put<std::uint16_t>(bad, 0x84 + 20, 0x999);  // неизвестный Magic
  CHECK(!memrefl::ParsePeHeaders(bad.data(), bad.size()));
  bad = pe;
  Put<std::uint16_t>(bad, 0x84 + 2, 0xFFFF);  // секций больше, чем помещается
  CHECK(!memrefl::ParsePeHeaders(bad.data(), bad.size()));

  // Секция, выходящая за конец файла, усекается.
  bad = pe;
  Put<std::uint32_t>(bad, 0x84 + 20 + 0xF0 + 20, 0x780);  // .text: PointerToRawData = 0x780
  const auto clipped = memrefl::PeImageRegions(bad.data(), bad.size(), PeLayout::File);
  CHECK(clipped.size() == 1 && clipped[0].size == 0x80);
}

// --- Модули текущего процесса -----------------------------------------------------

// Под AddressSanitizer этот тест пропускается: сканирование образа модуля
// намеренно читает .rodata целиком, в том числе "красные зоны", которыми ASan
// окружает глобальные объекты, и ASan считает это ошибкой. Поиск в буферах
// (все тесты выше) под ASan проверяется полностью.
#if defined(__SANITIZE_ADDRESS__)
#define MEMREFL_TEST_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define MEMREFL_TEST_ASAN 1
#endif
#endif

#if defined(MEMREFL_HAS_LOADED_MODULES) && !defined(MEMREFL_TEST_ASAN)
#define MEMREFL_TEST_LOADED_MODULES 1

#if defined(_MSC_VER)
#define NOINLINE __declspec(noinline)
#else
#define NOINLINE __attribute__((noinline))
#endif

NOINLINE int ProbeFunction(int x) {
  int result = x;
  for (int i = 0; i < x; ++i) result = result * 31 + i;
  return result;
}

static const char kMarker[] = "memrefl::marker::5f3a9c";

static void TestLoadedModule(const char* argv0) {
  using memrefl::SectionFilter;

  // Код своей функции: берём её первые байты как сигнатуру и ищем в коде
  // главного модуля — адрес функции должен оказаться среди совпадений.
  const SignatureScanner code = memrefl::ScannerForModule();
  CHECK(!code.empty());
  const auto function = reinterpret_cast<std::uintptr_t>(&ProbeFunction);
  CHECK(code.Contains(function, 16));
  const Pattern prologue =
      Pattern::FromCodeStyle(reinterpret_cast<const void*>(function), std::string(16, 'x'));
  const std::vector<std::uintptr_t> hits = code.FindAll(prologue);
  CHECK(std::find(hits.begin(), hits.end(), function) != hits.end());

  // Данные (строка в .rodata/.rdata) видны только с SectionFilter::All.
  const SignatureScanner all = memrefl::ScannerForModule({}, SectionFilter::All);
  const Pattern marker = Pattern::FromCodeStyle(kMarker, std::string(sizeof(kMarker), 'x'));
  const std::vector<std::uintptr_t> marker_hits = all.FindAll(marker);
  CHECK(std::find(marker_hits.begin(), marker_hits.end(), reinterpret_cast<std::uintptr_t>(kMarker)) !=
        marker_hits.end());
  CHECK(all.Read<char>(reinterpret_cast<std::uintptr_t>(kMarker)) == 'm');

  CHECK(memrefl::LoadedModuleRegions("definitely_not_loaded_module_42.so").empty());
  CHECK(memrefl::ScannerForModule("definitely_not_loaded_module_42.so").empty());

#if defined(_WIN32)
  CHECK(!memrefl::LoadedModuleRegions("kernel32.dll").empty());
  CHECK(!memrefl::LoadedModuleRegions("KERNEL32.DLL").empty());
  (void)argv0;
#else
#if defined(__GLIBC__)
  CHECK(!memrefl::LoadedModuleRegions("libc.so.6").empty());
#endif
  // Главный модуль находится и по имени файла.
  const std::string self = argv0;
  const std::string self_name = self.substr(self.rfind('/') == std::string::npos ? 0 : self.rfind('/') + 1);
  CHECK(!memrefl::LoadedModuleRegions(self_name).empty());
#endif
}

#endif  // MEMREFL_TEST_LOADED_MODULES

int main(int argc, char** argv) {
  (void)argc;
  TestParseFormats();
  TestParseErrors();
  TestAnchors();
  TestCodeStyle();
  TestFindBasic();
  TestEdgesAndOverlaps();
  TestMatchesNaiveOnRandomData();
  TestRegions();
  TestReadAndResolveRelative();
  TestPeImage();
#if defined(MEMREFL_TEST_LOADED_MODULES)
  TestLoadedModule(argv[0]);
#else
  (void)argv;
#endif
#if defined(MEMREFL_HAS_SSE2)
  std::puts("all signature scanner tests passed (SSE2 path)");
#else
  std::puts("all signature scanner tests passed (scalar path)");
#endif
  return 0;
}

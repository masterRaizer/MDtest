// memrefl — модуль 1: откуда брать память для сканирования.
//
//   * LoadedModuleRegions("game.exe")  — секции модуля, загруженного в ТЕКУЩИЙ
//     процесс (Windows: PE-заголовки + VirtualQuery; Linux: dl_iterate_phdr).
//   * PeImageRegions(buffer, ...)      — PE-образ в буфере: файл с диска
//     (PeLayout::File, адреса как в IDA) или дамп модуля, снятый из другого
//     процесса (PeLayout::Mapped). Работает на любой ОС — удобно для офлайн-
//     проверки сигнатур на новой версии бинарника до её запуска.
//   * ParsePeHeaders()                 — разбор заголовков PE32/PE32+ с проверкой
//     всех границ (данные могут быть повреждены или намеренно искажены).
//
// Порядок байтов PE — little-endian; разбор рассчитан на little-endian хост.

#pragma once

#include "memrefl/signature_scanner.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#define MEMREFL_HAS_LOADED_MODULES 1
#elif defined(__linux__)
#include <link.h>
#include <unistd.h>
#define MEMREFL_HAS_LOADED_MODULES 1
#endif

namespace memrefl {

// Какие части модуля сканировать.
enum class SectionFilter {
  Code,  // только исполняемые секции/сегменты — для сигнатур функций
  All,   // всё, что есть в образе: код и данные (строки, vtable, константы)
};

// --- PE ----------------------------------------------------------------------

struct PeSection {
  static constexpr std::uint32_t kContainsCode = 0x00000020;  // IMAGE_SCN_CNT_CODE
  static constexpr std::uint32_t kMemExecute = 0x20000000;    // IMAGE_SCN_MEM_EXECUTE
  static constexpr std::uint32_t kMemRead = 0x40000000;       // IMAGE_SCN_MEM_READ

  std::string name;                   // до 8 символов, без завершающих нулей
  std::uint32_t virtual_address = 0;  // RVA — смещение от базы загруженного образа
  std::uint32_t virtual_size = 0;     // реальный размер в памяти
  std::uint32_t raw_offset = 0;       // смещение в файле (PointerToRawData)
  std::uint32_t raw_size = 0;         // размер в файле, выровнен по FileAlignment
  std::uint32_t characteristics = 0;

  bool IsExecutable() const noexcept { return (characteristics & (kContainsCode | kMemExecute)) != 0; }

  // Размер в загруженном образе. Некоторые линкеры оставляют VirtualSize = 0.
  std::size_t MappedSize() const noexcept { return virtual_size ? virtual_size : raw_size; }

  // Сколько байтов секции есть в файле: хвост сверх VirtualSize — выравнивание,
  // а часть сверх SizeOfRawData (например, .bss) в файле отсутствует.
  std::size_t FileSize() const noexcept {
    if (virtual_size == 0) return raw_size;
    return virtual_size < raw_size ? virtual_size : raw_size;
  }
};

struct PeHeaders {
  bool is_64 = false;
  std::uint64_t image_base = 0;  // предпочтительный адрес загрузки (без учёта ASLR)
  std::uint32_t size_of_image = 0;
  std::uint32_t size_of_headers = 0;
  std::vector<PeSection> sections;
};

namespace detail {

// Чтение с проверкой границ; смещение 64-битное, чтобы сумма не переполнилась
// и на 32-битном хосте.
template <class T>
bool ReadAt(const std::uint8_t* data, std::size_t size, std::uint64_t offset, T& out) noexcept {
  if (offset > size || sizeof(T) > size - offset) return false;
  std::memcpy(&out, data + offset, sizeof(T));
  return true;
}

inline bool SectionWanted(const PeSection& section, SectionFilter filter) noexcept {
  return filter == SectionFilter::All || section.IsExecutable();
}

}  // namespace detail

// Разбирает заголовки PE-образа. Достаточно первых size байт образа, в которых
// лежат заголовки (обычно одна страница). Заголовки идут в начале и в файле,
// и в загруженном модуле, поэтому функция подходит для обоих раскладов.
inline std::optional<PeHeaders> ParsePeHeaders(const void* image, std::size_t size) {
  const auto* data = static_cast<const std::uint8_t*>(image);
  std::uint16_t mz = 0;
  std::uint32_t nt = 0;  // e_lfanew
  std::uint32_t signature = 0;
  if (!detail::ReadAt(data, size, 0, mz) || mz != 0x5A4D) return std::nullopt;  // "MZ"
  if (!detail::ReadAt(data, size, 0x3C, nt)) return std::nullopt;
  if (!detail::ReadAt(data, size, nt, signature) || signature != 0x00004550) return std::nullopt;  // "PE\0\0"

  // IMAGE_FILE_HEADER (20 байт) сразу за сигнатурой.
  const std::uint64_t file_header = std::uint64_t{nt} + 4;
  std::uint16_t section_count = 0;
  std::uint16_t optional_size = 0;
  if (!detail::ReadAt(data, size, file_header + 2, section_count) ||
      !detail::ReadAt(data, size, file_header + 16, optional_size)) {
    return std::nullopt;
  }

  // IMAGE_OPTIONAL_HEADER32/64: отличаются полем ImageBase, а SizeOfImage
  // и SizeOfHeaders в обоих лежат по смещениям 56 и 60.
  const std::uint64_t optional = file_header + 20;
  std::uint16_t magic = 0;
  PeHeaders headers;
  if (optional_size < 64 || !detail::ReadAt(data, size, optional, magic)) return std::nullopt;
  if (magic == 0x20B) {  // PE32+
    headers.is_64 = true;
    if (!detail::ReadAt(data, size, optional + 24, headers.image_base)) return std::nullopt;
  } else if (magic == 0x10B) {  // PE32
    std::uint32_t base32 = 0;
    if (!detail::ReadAt(data, size, optional + 28, base32)) return std::nullopt;
    headers.image_base = base32;
  } else {
    return std::nullopt;
  }
  if (!detail::ReadAt(data, size, optional + 56, headers.size_of_image) ||
      !detail::ReadAt(data, size, optional + 60, headers.size_of_headers)) {
    return std::nullopt;
  }

  // Таблица секций (IMAGE_SECTION_HEADER по 40 байт) — сразу за optional header.
  const std::uint64_t table = optional + optional_size;
  headers.sections.reserve(section_count);
  for (std::uint16_t i = 0; i < section_count; ++i) {
    const std::uint64_t entry = table + std::uint64_t{i} * 40;
    char name[8];
    PeSection s;
    if (!detail::ReadAt(data, size, entry, name) ||
        !detail::ReadAt(data, size, entry + 8, s.virtual_size) ||
        !detail::ReadAt(data, size, entry + 12, s.virtual_address) ||
        !detail::ReadAt(data, size, entry + 16, s.raw_size) ||
        !detail::ReadAt(data, size, entry + 20, s.raw_offset) ||
        !detail::ReadAt(data, size, entry + 36, s.characteristics)) {
      return std::nullopt;
    }
    std::size_t name_length = 0;
    while (name_length < sizeof(name) && name[name_length] != '\0') ++name_length;
    s.name.assign(name, name_length);
    headers.sections.push_back(std::move(s));
  }
  return headers;
}

// Как расположены секции в буфере.
enum class PeLayout {
  File,    // как на диске: секция по PointerToRawData
  Mapped,  // как в памяти процесса: секция по RVA (дамп загруженного модуля)
};

// Для load_base: брать ImageBase из заголовка — адреса совпадут с IDA/Ghidra.
inline constexpr std::uintptr_t kHeaderImageBase = 0;

// Регионы секций PE-образа, лежащего в буфере. Адреса регионов —
// load_base + RVA; для дампа из процесса передайте реальную базу модуля
// (из-за ASLR она отличается от ImageBase). Пустой результат — образ не разобран.
inline std::vector<MemoryRegion> PeImageRegions(const void* image, std::size_t size, PeLayout layout,
                                                SectionFilter filter = SectionFilter::Code,
                                                std::uintptr_t load_base = kHeaderImageBase) {
  const std::optional<PeHeaders> headers = ParsePeHeaders(image, size);
  if (!headers) return {};
  const auto* bytes = static_cast<const std::uint8_t*>(image);
  const std::uintptr_t base =
      load_base != kHeaderImageBase ? load_base : static_cast<std::uintptr_t>(headers->image_base);

  std::vector<MemoryRegion> regions;
  for (const PeSection& s : headers->sections) {
    if (!detail::SectionWanted(s, filter)) continue;
    const std::size_t offset = layout == PeLayout::File ? s.raw_offset : s.virtual_address;
    std::size_t length = layout == PeLayout::File ? s.FileSize() : s.MappedSize();
    if (length == 0 || offset >= size) continue;
    if (length > size - offset) length = size - offset;  // усечённый файл/дамп
    regions.push_back(MemoryRegion::Snapshot(bytes + offset, length, base + s.virtual_address));
  }
  return regions;
}

// --- Модули текущего процесса -------------------------------------------------
//
// LoadedModuleRegions(name, filter): читаемые регионы модуля, загруженного в
// текущий процесс. Пустое имя — главный исполняемый файл. Имя сравнивается с
// именем файла модуля ("game.exe", "libfoo.so"); на Linux можно передать и
// полный путь. Модуль не найден — пустой вектор.

#if defined(_WIN32)

namespace detail {

inline bool IsReadableProtection(DWORD protect) noexcept {
  if (protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
  return (protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                     PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

// Добавляет только реально читаемые части диапазона: защитники и упаковщики
// меняют права страниц после загрузки, и чтение PAGE_NOACCESS/PAGE_GUARD
// уронило бы процесс. Соседние части потом склеит SignatureScanner.
inline void AppendReadableRanges(const std::uint8_t* begin, std::size_t size, std::vector<MemoryRegion>& out) {
  const std::uint8_t* p = begin;
  const std::uint8_t* const end = begin + size;
  while (p < end) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(p, &mbi, sizeof(mbi)) == 0) break;
    const auto* block_end = static_cast<const std::uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
    const auto* chunk_end = block_end < end ? block_end : end;
    if (mbi.State == MEM_COMMIT && IsReadableProtection(mbi.Protect)) {
      out.push_back(MemoryRegion::InProcess(p, static_cast<std::size_t>(chunk_end - p)));
    }
    p = chunk_end;
  }
}

inline HMODULE FindLoadedModule(std::string_view name) {
  if (name.empty()) return GetModuleHandleW(nullptr);
  // Имя в UTF-8 -> UTF-16: ANSI-версия зависела бы от кодовой страницы системы.
  const int length = MultiByteToWideChar(CP_UTF8, 0, name.data(), static_cast<int>(name.size()), nullptr, 0);
  if (length <= 0) return nullptr;
  std::wstring wide(static_cast<std::size_t>(length), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, name.data(), static_cast<int>(name.size()), &wide[0], length);
  return GetModuleHandleW(wide.c_str());
}

}  // namespace detail

inline std::vector<MemoryRegion> LoadedModuleRegions(std::string_view module_name,
                                                     SectionFilter filter = SectionFilter::Code) {
  const HMODULE module = detail::FindLoadedModule(module_name);
  if (module == nullptr) return {};
  const auto* base = reinterpret_cast<const std::uint8_t*>(module);

  // Заголовки лежат в первых страницах образа; сколько из них можно читать,
  // узнаём у VirtualQuery, а не верим полям заголовка.
  MEMORY_BASIC_INFORMATION mbi{};
  if (VirtualQuery(base, &mbi, sizeof(mbi)) == 0 || mbi.State != MEM_COMMIT ||
      !detail::IsReadableProtection(mbi.Protect)) {
    return {};
  }
  const auto header_bytes =
      static_cast<std::size_t>(static_cast<const std::uint8_t*>(mbi.BaseAddress) + mbi.RegionSize - base);
  const std::optional<PeHeaders> headers = ParsePeHeaders(base, header_bytes);
  if (!headers) return {};

  std::vector<MemoryRegion> regions;
  for (const PeSection& s : headers->sections) {
    if (!detail::SectionWanted(s, filter) || s.virtual_address >= headers->size_of_image) continue;
    std::size_t length = s.MappedSize();
    if (length > headers->size_of_image - s.virtual_address) length = headers->size_of_image - s.virtual_address;
    detail::AppendReadableRanges(base + s.virtual_address, length, regions);
  }
  return regions;
}

#elif defined(__linux__)

namespace detail {

inline std::string SelfExecutablePath() {
  char buffer[4096];
  const ssize_t n = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
  return n > 0 ? std::string(buffer, static_cast<std::size_t>(n)) : std::string();
}

inline bool ModuleNameMatches(const char* loader_name, bool is_main, std::string_view wanted) {
  if (wanted.empty()) return is_main;
  // У главной программы загрузчик не хранит имя — берём путь из /proc.
  std::string path = loader_name != nullptr ? loader_name : "";
  if (path.empty() && is_main) path = SelfExecutablePath();
  if (wanted.find('/') != std::string_view::npos) return path == wanted;
  const std::size_t slash = path.rfind('/');
  return std::string_view(path).substr(slash == std::string::npos ? 0 : slash + 1) == wanted;
}

}  // namespace detail

inline std::vector<MemoryRegion> LoadedModuleRegions(std::string_view module_name,
                                                     SectionFilter filter = SectionFilter::Code) {
  struct Search {
    std::string_view name;
    SectionFilter filter;
    bool is_first = true;  // dl_iterate_phdr перечисляет главную программу первой
    std::vector<MemoryRegion> regions;
  } search{module_name, filter, true, {}};

  dl_iterate_phdr(
      [](dl_phdr_info* info, std::size_t, void* context) -> int {
        auto& s = *static_cast<Search*>(context);
        const bool is_main = s.is_first;
        s.is_first = false;
        if (!detail::ModuleNameMatches(info->dlpi_name, is_main, s.name)) return 0;
        // Сегменты PT_LOAD — то, что загрузчик отобразил в память
        // (адрес = база модуля + p_vaddr).
        for (ElfW(Half) i = 0; i < info->dlpi_phnum; ++i) {
          const ElfW(Phdr)& ph = info->dlpi_phdr[i];
          if (ph.p_type != PT_LOAD || !(ph.p_flags & PF_R)) continue;
          if (s.filter == SectionFilter::Code && !(ph.p_flags & PF_X)) continue;
          s.regions.push_back(MemoryRegion::InProcess(
              reinterpret_cast<const void*>(info->dlpi_addr + ph.p_vaddr), ph.p_memsz));
        }
        return 1;  // модуль найден — перебор окончен
      },
      &search);
  return std::move(search.regions);
}

#endif

#if defined(MEMREFL_HAS_LOADED_MODULES)

// Сканер по модулю текущего процесса. Если модуль не найден, сканер пуст
// (empty() == true) и ничего не находит.
inline SignatureScanner ScannerForModule(std::string_view module_name = {},
                                         SectionFilter filter = SectionFilter::Code) {
  return SignatureScanner(LoadedModuleRegions(module_name, filter));
}

#endif

}  // namespace memrefl

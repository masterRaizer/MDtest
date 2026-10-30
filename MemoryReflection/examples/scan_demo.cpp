// Демонстрация: одна и та же сигнатура находит функцию в двух "версиях"
// бинарника, хотя после обновления код сдвинулся, а адреса операндов изменились.
//
// Имитируем фрагмент секции .text. Функция GetObjectRegistry():
//   48 8B 05 xx xx xx xx    mov  rax, [rip + g_ObjectRegistry]
//   48 85 C0                test rax, rax
//   74 xx                   jz   short ...
//   ...
// И одно место её вызова:
//   E8 xx xx xx xx          call GetObjectRegistry
//   48 8B D8                mov  rbx, rax
// Сигнатуры маскируют то, что меняется между версиями (смещения rel32/disp32),
// и сохраняют то, что не меняется (опкоды и регистры).

#include "memrefl/module_regions.hpp"
#include "memrefl/signature_scanner.hpp"

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

struct FakeBuild {
  const char* name;
  std::size_t padding_before_function;  // в новой версии перед функцией появился код
  std::size_t distance_to_call;         // и вызов переехал
  std::int32_t registry_disp;           // глобальная переменная тоже сдвинулась
};

// Собирает содержимое .text для "версии".
std::vector<std::uint8_t> BuildText(const FakeBuild& build) {
  std::vector<std::uint8_t> text(0x400, 0xCC);  // int3-заполнитель, как между функциями у MSVC
  const std::size_t function = build.padding_before_function;
  const std::uint8_t body[] = {0x48, 0x8B, 0x05, 0, 0, 0, 0, 0x48, 0x85, 0xC0, 0x74, 0x05, 0x48, 0x8B, 0x40, 0x08, 0xC3};
  std::memcpy(&text[function], body, sizeof(body));
  std::memcpy(&text[function + 3], &build.registry_disp, 4);

  const std::size_t call = function + build.distance_to_call;
  text[call] = 0xE8;
  // rel32 отсчитывается от конца инструкции call (call + 5).
  const auto rel = static_cast<std::int32_t>(static_cast<std::intptr_t>(function) -
                                             static_cast<std::intptr_t>(call + 5));
  std::memcpy(&text[call + 1], &rel, 4);
  const std::uint8_t after_call[] = {0x48, 0x8B, 0xD8};
  std::memcpy(&text[call + 5], after_call, sizeof(after_call));
  return text;
}

void AnalyzeBuild(const FakeBuild& build) {
  const std::uintptr_t text_base = 0x10001000;
  const std::vector<std::uint8_t> text = BuildText(build);
  // Буфер — "снимок" секции: байты у нас, адреса — как в исследуемом процессе.
  const memrefl::SignatureScanner scanner(memrefl::MemoryRegion::Snapshot(text.data(), text.size(), text_base));

  // Подготовленные сигнатуры: разбор один раз, поиск сколько угодно.
  static const memrefl::Pattern kFunction("48 8B 05 ?? ?? ?? ?? 48 85 C0 74 ??");
  static const memrefl::Pattern kCallSite("E8 ?? ?? ?? ?? 48 8B D8");

  std::printf("[%s]\n", build.name);

  // Путь 1: сигнатура самой функции.
  const auto function = scanner.FindUnique(kFunction);
  // Путь 2: сигнатура места вызова + разрешение rel32 у call.
  const auto call = scanner.FindUnique(kCallSite);
  const auto via_call = call ? scanner.ResolveRelative(*call, 1, 5) : std::nullopt;
  if (!function || !via_call) {
    std::printf("  signature not found or ambiguous: needs updating\n");
    return;
  }
  // Из инструкции mov rax, [rip + disp32] достаём адрес глобальной переменной.
  const auto registry = scanner.ResolveRelative(*function, 3, 7);

  std::printf("  GetObjectRegistry  = 0x%" PRIXPTR " (by own signature)\n", *function);
  std::printf("  call site          = 0x%" PRIXPTR " -> target 0x%" PRIXPTR " %s\n", *call, *via_call,
              *via_call == *function ? "(matches)" : "(MISMATCH)");
  std::printf("  &g_ObjectRegistry  = 0x%" PRIXPTR "\n", *registry);
}

}  // namespace

int main() {
  const FakeBuild v1{"build 1.0", 0x40, 0x120, 0x00012340};
  const FakeBuild v2{"build 1.1", 0x7C, 0x1A8, 0x00013F20};
  AnalyzeBuild(v1);
  AnalyzeBuild(v2);

#if defined(MEMREFL_HAS_LOADED_MODULES)
  // Тот же сканер для своего процесса: секции кода главного модуля.
  const memrefl::SignatureScanner self = memrefl::ScannerForModule();
  std::size_t bytes = 0;
  for (const auto& region : self.regions()) bytes += region.size;
  std::printf("[this process]\n  main module code: %zu region(s), %zu bytes\n", self.regions().size(), bytes);
  if (const auto first_call = self.FindFirst("E8 ?? ?? ?? ??")) {
    std::printf("  first call rel32 at 0x%" PRIXPTR " -> 0x%" PRIXPTR "\n", *first_call,
                *self.ResolveRelative(*first_call, 1, 5));
  }
#endif
  return 0;
}

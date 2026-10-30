// Демонстрация Generic Data Resolver на живых структурах этого процесса.
//
// "Приложение" (верхняя часть файла) хранит данные так, как это часто
// делают C++-программы:
//   * g_TypeNames — массив указателей на UTF-8 имена типов; объект хранит
//     type_id = (индекс << 1) | флаг;
//   * g_Properties — хеш-таблица с цепочками: ключ — FNV-1a хеш имени
//     свойства, значение — UTF-16 строка {const char16_t* data; int32 length}.
//
// "Инструмент" (main) знает только адреса таблиц (их можно найти сигнатурами
// из модуля 1) и раскладку структур. Всё читается через ProcessMemoryReader:
// мусорный указатель даёт nullopt, а не падение.

#include "memrefl/data_resolver.hpp"

#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#endif

// --- "Приложение" ----------------------------------------------------------------

namespace app {

const char* g_TypeNames[] = {"None", "Transform", "Camera", "\xD0\x9C\xD0\xB5\xD1\x88Renderer"};  // "МешRenderer"

struct WideName {
  const char16_t* data;
  std::int32_t length;
};

struct PropertyNode {
  std::uint32_t hash;
  std::uint32_t flags;
  PropertyNode* next;
  WideName name;
};

constexpr std::uint32_t kBucketCount = 8;
PropertyNode* g_Properties[kBucketCount] = {};

void RegisterProperty(PropertyNode& node, std::u16string_view name) {
  node.hash = static_cast<std::uint32_t>(memrefl::HashString(memrefl::Utf16ToUtf8(name)));
  node.name = {name.data(), static_cast<std::int32_t>(name.size())};
  PropertyNode*& head = g_Properties[node.hash % kBucketCount];
  node.next = head;
  head = &node;
}

PropertyNode g_Nodes[3];

struct Object {
  std::uint32_t type_id;        // (индекс << 1) | флаг "активен"
  std::uint32_t property_hash;  // какое свойство анимируется
};

}  // namespace app

// --- "Инструмент" ------------------------------------------------------------------

int main() {
#if defined(_WIN32)
  SetConsoleOutputCP(CP_UTF8);
#endif
  app::RegisterProperty(app::g_Nodes[0], u"Position");
  app::RegisterProperty(app::g_Nodes[1], u"FieldOfView");
  app::RegisterProperty(app::g_Nodes[2], u"\x0426\x0432\x0435\x0442");  // "Цвет"
  const app::Object objects[] = {
      {(1u << 1) | 1, static_cast<std::uint32_t>(memrefl::HashString("Position"))},
      {(2u << 1) | 0, static_cast<std::uint32_t>(memrefl::HashString("FieldOfView"))},
      {(3u << 1) | 1, static_cast<std::uint32_t>(memrefl::HashString("\xD0\xA6\xD0\xB2\xD0\xB5\xD1\x82"))},
      {(9u << 1) | 1, static_cast<std::uint32_t>(memrefl::HashString("Scale"))},  // нет ни там, ни там
      {(0u << 1) | 0, static_cast<std::uint32_t>(memrefl::HashString("Rotation"))},  // есть только в словаре
  };

  const auto reader = std::make_shared<memrefl::ProcessMemoryReader>(memrefl::ProcessMemoryReader::Self());

  // 1. Типы: плоский массив указателей, ID = (индекс << 1) | флаг.
  memrefl::ArrayTableSpec types;
  types.array = reinterpret_cast<std::uintptr_t>(app::g_TypeNames);
  types.count = sizeof(app::g_TypeNames) / sizeof(app::g_TypeNames[0]);
  const memrefl::GenericDataResolver type_names(reader, types, memrefl::KeyConverter::BitField(1, ~0ull));

  // 2. Свойства: хеш-таблица в памяти + словарь известных имён на случай,
  //    если свойство ещё не зарегистрировано в процессе.
  memrefl::HashTableSpec properties;
  properties.buckets = reinterpret_cast<std::uintptr_t>(app::g_Properties);
  properties.bucket_count = app::kBucketCount;
  properties.key_offset = offsetof(app::PropertyNode, hash);
  properties.key_size = sizeof(std::uint32_t);
  properties.next_offset = offsetof(app::PropertyNode, next);
  properties.value_offset = offsetof(app::PropertyNode, name);
  properties.value_is_pointer = false;  // WideName лежит прямо в узле
  properties.string.encoding = memrefl::Encoding::Utf16LE;
  properties.string.chars_by_pointer = true;
  properties.string.chars_offset = offsetof(app::WideName, data);
  properties.string.length_offset = offsetof(app::WideName, length);
  properties.string.length_size = sizeof(std::int32_t);

  memrefl::DictionarySpec known;
  known.names = {"Position", "Rotation", "Scale3D"};  // например, из документации или прошлых сессий

  const memrefl::GenericDataResolver property_names(std::make_shared<memrefl::FallbackNameTable>(
      std::vector<std::shared_ptr<const memrefl::NameTable>>{memrefl::MakeNameTable(reader, properties),
                                                             memrefl::MakeNameTable(nullptr, known)}));

  // printf выравнивает по байтам, а в UTF-8 символ занимает 1-4 байта:
  // дополняем пробелами по числу кодовых точек.
  auto pad = [](std::string s, std::size_t width) {
    std::size_t chars = 0;
    for (const char c : s) chars += (static_cast<unsigned char>(c) & 0xC0) != 0x80;
    if (chars < width) s.append(width - chars, ' ');
    return s;
  };

  std::printf("%-8s %-10s %s %-12s %s\n", "object", "type_id", pad("type", 16).c_str(), "property", "name");
  for (std::size_t i = 0; i < sizeof(objects) / sizeof(objects[0]); ++i) {
    const app::Object& object = objects[i];
    std::printf("%-8zu 0x%-8" PRIX32 " %s 0x%08" PRIX32 "   %s\n", i, object.type_id,
                pad(type_names.Label(object.type_id), 16).c_str(), object.property_hash,
                property_names.Label(object.property_hash).c_str());
  }

  // Выгрузка всей таблицы типов — например, для сохранения вместе с отчётом.
  std::printf("\nall types:");
  type_names.ForEachName([](std::uint64_t index, const std::string& name) {
    std::printf(" [%" PRIu64 "]=%s", index, name.c_str());
    return true;
  });

  const memrefl::ResolverStats stats = property_names.stats();
  std::printf("\nproperty resolver: %" PRIu64 " lookups, %" PRIu64 " resolved, %" PRIu64 " unresolved\n",
              stats.lookups, stats.resolved, stats.unresolved);
  return 0;
}

// Демонстрация всех трёх модулей вместе: доступ к свойствам объектов по имени.
//
// "Приложение" (namespace app) устроено как многие C++-программы с рефлексией:
//   * g_Names — таблица имён, объекты и метаданные хранят только ID;
//   * TypeInfo/PropInfo — описания типов: имя, родитель, список свойств
//     {имя, смещение, тег типа, вложенный тип};
//   * у каждого объекта первым полем идёт указатель на его TypeInfo;
//   * корень данных — глобальная переменная g_World, к которой обращается код
//     "mov rax, [rip + g_World]; test rax, rax; jz ...".
//
// "Инструмент" (main) не использует типы приложения, только адреса и раскладку:
//   1. модуль 1 — находит g_World по сигнатуре этой инструкции;
//   2. модуль 2 — переводит ID имён в строки (GenericDataResolver над g_Names);
//   3. модуль 3 — строит схемы из TypeInfo и читает значения по путям вроде
//      "Hero.Bag.Items[1].Quantity". Всё читается через ProcessMemoryReader,
//      мусорные указатели не роняют процесс.

#include "memrefl/data_resolver.hpp"
#include "memrefl/memory_reader.hpp"
#include "memrefl/pattern.hpp"
#include "memrefl/property_fetcher.hpp"
#include "memrefl/reflection_loader.hpp"
#include "memrefl/signature_scanner.hpp"

#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#endif

// --- "Приложение" ---------------------------------------------------------------------

namespace app {

enum NameId : std::uint32_t {
  kNone, kObject, kItem, kInventory, kCharacter, kWorld, kDisplayName, kQuantity, kWeight, kItems,
  kHealth, kDefense, kBag, kHero, kSword, kPotion, kElixir, kHeroName,
};
const char* g_Names[] = {"None", "Object", "Item", "Inventory", "Character", "World", "DisplayName", "Quantity",
                         "Weight", "Items", "Health", "Defense", "Bag", "Hero", "Sword", "Potion", "Elixir",
                         "\xD0\x93\xD0\xB5\xD1\x80\xD0\xBE\xD0\xB9"};  // "Герой"

enum Tag : std::uint32_t { kInt32 = 1, kFloat = 2, kObjectPointer = 3, kObjectArray = 4, kNameTag = 5 };

struct TypeInfo;
struct PropInfo {
  std::uint32_t name;
  std::uint32_t offset;
  std::uint32_t tag;
  std::uint32_t reserved;
  const TypeInfo* inner;
  const PropInfo* next;
};
struct TypeInfo {
  std::uint32_t name;
  std::uint32_t reserved;
  const TypeInfo* super;
  const PropInfo* first;
};

struct Object {
  const TypeInfo* type;
  std::uint32_t name;
};
struct Item {
  Object header;
  std::int32_t quantity;
  float weight;
};
struct Inventory {
  Object header;
  Item** items;
  std::int32_t count;
};
struct Character {
  Object header;
  std::int32_t health;
  float defense;
  Inventory* bag;
};
struct World {
  Object header;
  Character* hero;
};

extern const TypeInfo kObjectType, kItemType, kInventoryType, kCharacterType, kWorldType;

const PropInfo kObjectName{kDisplayName, offsetof(Object, name), kNameTag, 0, nullptr, nullptr};
const TypeInfo kObjectType{kObject, 0, nullptr, &kObjectName};

const PropInfo kItemWeight{kWeight, offsetof(Item, weight), kFloat, 0, nullptr, nullptr};
const PropInfo kItemQuantity{kQuantity, offsetof(Item, quantity), kInt32, 0, nullptr, &kItemWeight};
const TypeInfo kItemType{kItem, 0, &kObjectType, &kItemQuantity};

const PropInfo kInventoryItems{kItems, offsetof(Inventory, items), kObjectArray, 0, &kItemType, nullptr};
const TypeInfo kInventoryType{kInventory, 0, &kObjectType, &kInventoryItems};

const PropInfo kCharacterBag{kBag, offsetof(Character, bag), kObjectPointer, 0, &kInventoryType, nullptr};
const PropInfo kCharacterDefense{kDefense, offsetof(Character, defense), kFloat, 0, nullptr, &kCharacterBag};
const PropInfo kCharacterHealth{kHealth, offsetof(Character, health), kInt32, 0, nullptr, &kCharacterDefense};
const TypeInfo kCharacterType{kCharacter, 0, &kObjectType, &kCharacterHealth};

const PropInfo kWorldHero{kHero, offsetof(World, hero), kObjectPointer, 0, &kCharacterType, nullptr};
const TypeInfo kWorldType{kWorld, 0, &kObjectType, &kWorldHero};

Item g_Sword{{&kItemType, kSword}, 1, 3.5f};
Item g_Potion{{&kItemType, kPotion}, 5, 0.25f};
Item g_Elixir{{&kItemType, kElixir}, 2, 0.5f};
Item* g_BagItems[] = {&g_Sword, &g_Potion};
Inventory g_Bag{{&kInventoryType, kBag}, g_BagItems, 2};
Character g_Hero{{&kCharacterType, kHeroName}, 90, 12.5f, &g_Bag};
World g_WorldObject{{&kWorldType, kWorld}, &g_Hero};
World* g_World = &g_WorldObject;

// Фрагмент машинного кода, обращающегося к g_World:
//   48 8B 05 rel32   mov  rax, [rip + g_World]
//   48 85 C0         test rax, rax
//   74 05            jz   short ...
// Настоящий компилятор сгенерировал бы его сам; здесь он собирается при
// старте, чтобы пример не зависел от компилятора. Массив лежит в том же
// модуле, что и g_World, поэтому смещение помещается в 32 бита.
unsigned char g_Code[64];

void EmitCode() {
  std::memset(g_Code, 0xCC, sizeof(g_Code));
  unsigned char* insn = g_Code + 16;
  const unsigned char prefix[] = {0x48, 0x8B, 0x05};
  const unsigned char suffix[] = {0x48, 0x85, 0xC0, 0x74, 0x05};
  std::memcpy(insn, prefix, sizeof(prefix));
  const auto rel = static_cast<std::int32_t>(reinterpret_cast<std::intptr_t>(&g_World) -
                                             reinterpret_cast<std::intptr_t>(insn + 7));
  std::memcpy(insn + 3, &rel, sizeof(rel));
  std::memcpy(insn + 7, suffix, sizeof(suffix));
}

}  // namespace app

// --- "Инструмент" ----------------------------------------------------------------------

namespace {

void Print(const char* path, const memrefl::Result<memrefl::PropertyValue>& value) {
  if (value) {
    std::printf("  %-30s = %s\n", path, memrefl::FormatValue(*value).c_str());
  } else {
    std::printf("  %-30s ! %s: %s\n", path, memrefl::FetchErrorCodeName(value.error().code),
                value.error().message.c_str());
  }
}

}  // namespace

int main() {
#if defined(_WIN32)
  SetConsoleOutputCP(CP_UTF8);
#endif
  app::EmitCode();
  const auto reader = std::make_shared<memrefl::ProcessMemoryReader>(memrefl::ProcessMemoryReader::Self());

  // 1. Модуль 1: адрес g_World по сигнатуре инструкции. В реальном инструменте
  //    это ScannerForModule("app.exe"); здесь сканируется только app::g_Code,
  //    чтобы та же последовательность в коде самого примера не сделала
  //    сигнатуру неоднозначной.
  const memrefl::SignatureScanner code(memrefl::MemoryRegion::InProcess(app::g_Code, sizeof(app::g_Code)));
  const auto world_global =
      code.FindRelativeTarget(memrefl::Pattern("48 8B 05 ?? ?? ?? ?? 48 85 C0 74 ??"), 3, 7);
  if (!world_global) {
    std::puts("signature not found");
    return 1;
  }
  std::printf("&g_World found by signature: 0x%" PRIXPTR " (%s)\n", *world_global,
              *world_global == reinterpret_cast<std::uintptr_t>(&app::g_World) ? "correct" : "WRONG");

  // 2. Модуль 2: ID имён -> строки.
  memrefl::ArrayTableSpec names_table;
  names_table.array = reinterpret_cast<std::uintptr_t>(app::g_Names);
  names_table.count = sizeof(app::g_Names) / sizeof(app::g_Names[0]);
  const auto names = std::make_shared<memrefl::GenericDataResolver>(reader, names_table);

  // 3. Модуль 3: схемы из TypeInfo/PropInfo приложения.
  memrefl::ReflectionLayout layout;
  layout.object_type_offset = offsetof(app::Object, type);
  layout.type_name_offset = offsetof(app::TypeInfo, name);
  layout.super_offset = offsetof(app::TypeInfo, super);
  layout.list_offset = offsetof(app::TypeInfo, first);
  layout.next_offset = offsetof(app::PropInfo, next);
  layout.property_name_offset = offsetof(app::PropInfo, name);
  layout.property_offset_offset = offsetof(app::PropInfo, offset);
  layout.property_tag_offset = offsetof(app::PropInfo, tag);
  layout.property_inner_type_offset = offsetof(app::PropInfo, inner);
  layout.tags[app::kInt32] = memrefl::ScalarProperty("", memrefl::PropertyType::Int32, 0);
  layout.tags[app::kFloat] = memrefl::ScalarProperty("", memrefl::PropertyType::Float, 0);
  layout.tags[app::kObjectPointer] = memrefl::ObjectProperty("", 0);
  layout.tags[app::kObjectArray] = memrefl::DynamicArrayProperty(
      "", 0, memrefl::PropertyType::StructPointer, offsetof(app::Inventory, count) - offsetof(app::Inventory, items));
  layout.tags[app::kNameTag] = memrefl::NameProperty("", 0);

  memrefl::PropertyFetcher fetcher(reader);
  fetcher.SetReflection(std::make_shared<memrefl::ReflectionLoader>(reader, names, layout)).SetNameResolver(names);

  // Корень — *g_World: если приложение пересоздаст мир, путь останется верным.
  const memrefl::ObjectRef world{memrefl::PointerPath(*world_global, {0})};
  std::printf("\nroot object type: %s\n", fetcher.TypeName(world).value_or("?").c_str());
  for (const char* path : {"Hero.DisplayName", "Hero.Health", "Hero.Defense", "Hero.Bag.Items[0].DisplayName",
                           "Hero.Bag.Items[1].Quantity", "Hero.Bag.Items[1].Weight", "Hero.Bag.Items[2].Quantity",
                           "Hero.Helth", "Hero.Health.Value"}) {
    Print(path, fetcher.Fetch(world, path));
  }
  std::printf("  %-30s = %zu\n", "count(Hero.Bag.Items)", fetcher.Count(world, "Hero.Bag.Items").value_or(0));

  // Get<T> с проверкой типа и диапазона.
  const int health = fetcher.Get<int>(world, "Hero.Health").value_or(-1);
  const auto as_byte = fetcher.Get<std::uint8_t>(world, "Hero.Health");
  const auto as_int = fetcher.Get<int>(world, "Hero.Defense");
  std::printf("\nGet<int>(Hero.Health) = %d; Get<uint8_t> ok = %s; Get<int>(Hero.Defense): %s\n", health,
              as_byte ? "yes" : "no", as_int ? "ok" : memrefl::FetchErrorCodeName(as_int.error().code));

  // Locate один раз — затем Read() по готовому PointerPath, пока приложение меняет данные.
  const auto location = fetcher.Locate(world, "Hero.Bag.Items[1].Quantity");
  std::printf("\nHero.Bag.Items[1].Quantity lives at %s\n", location->path.ToString().c_str());
  app::g_BagItems[1] = &app::g_Elixir;  // приложение заменило предмет
  app::g_Hero.health = 75;
  std::printf("after the app changed its data: Items[1].Quantity = %s, Hero.Health = %s\n",
              memrefl::FormatValue(fetcher.Read(*location).value()).c_str(),
              memrefl::FormatValue(fetcher.Fetch(world, "Hero.Health").value()).c_str());

  // Выгрузка всех свойств объекта.
  std::printf("\ndump of Hero:\n");
  // *(*g_World + offsetof(World, hero)): объект героя, тип — из памяти.
  const memrefl::ObjectRef hero{
      memrefl::PointerPath(*world_global, {static_cast<std::ptrdiff_t>(offsetof(app::World, hero)), 0})};
  const auto dump = fetcher.Dump(hero);
  for (const memrefl::PropertyEntry& entry : dump.value()) {
    std::printf("  %-12s %-14s +0x%02zX  %-10s %s\n", entry.name.c_str(), memrefl::PropertyTypeName(entry.type),
                entry.offset, entry.owner_type.c_str(),
                entry.value ? memrefl::FormatValue(*entry.value).c_str() : entry.value.error().message.c_str());
  }
  return 0;
}

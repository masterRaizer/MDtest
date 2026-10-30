// Тесты модуля 3 (PropertyFetcher). Объекты раскладываются байт за байтом в
// FakeMemory, поэтому проверяются точные смещения, 32-битные указатели,
// повреждённые данные и метаданные рефлексии в памяти цели.

#include "memrefl/data_resolver.hpp"
#include "memrefl/memory_reader.hpp"
#include "memrefl/property_fetcher.hpp"
#include "memrefl/property_schema.hpp"
#include "memrefl/reflection_loader.hpp"
#include "test_support.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace memrefl;
using memrefl::testing::FakeMemory;
using memrefl::testing::Throws;

namespace {

template <class T>
bool Fails(const Result<T>& result, FetchErrorCode code) {
  if (result) return false;
  if (result.error().code != code) {
    std::fprintf(stderr, "  expected %s, got %s: %s\n", FetchErrorCodeName(code),
                 FetchErrorCodeName(result.error().code), result.error().message.c_str());
    return false;
  }
  return true;
}

bool Contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

std::shared_ptr<const GenericDataResolver> Dictionary(std::vector<std::pair<std::uint64_t, std::string>> entries) {
  DictionarySpec spec;
  spec.entries = std::move(entries);
  return std::make_shared<GenericDataResolver>(nullptr, spec);
}

// --- Разбор пути, Result, преобразования -----------------------------------------------

void TestPathParsing() {
  const auto path = detail::ParsePropertyPath("Inventory.Items[12].Quantity");
  CHECK(path && path->size() == 3);
  CHECK((*path)[0].name == "Inventory" && !(*path)[0].index);
  CHECK((*path)[1].name == "Items" && (*path)[1].index == std::size_t{12});
  CHECK(detail::ParsePropertyPath("A")->size() == 1);
  CHECK(detail::ParsePropertyPath("Display Name.X")->front().name == "Display Name");

  for (const char* bad : {"", ".", "A.", ".A", "A..B", "[1]", "A[", "A[]", "A[x]", "A[1", "A[1]x", "A[1][2]", "A]",
                          "A[99999999999999999999999]"}) {
    const auto parsed = detail::ParsePropertyPath(bad);
    CHECK(Fails(parsed, FetchErrorCode::InvalidPath));
  }
  CHECK(Contains(detail::ParsePropertyPath("A[x]").error().message, "expected array index"));
}

void TestResultAndConversions() {
  const Result<int> ok = 5;
  const Result<int> bad = FetchError{FetchErrorCode::NullPointer, "boom"};
  CHECK(ok && *ok == 5 && ok.value_or(1) == 5);
  CHECK(!bad && bad.value_or(1) == 1 && bad.error().message == "boom");
  bool threw = false;
  try {
    (void)bad.value();
  } catch (const BadResultAccess& e) {
    threw = e.error().code == FetchErrorCode::NullPointer && Contains(e.what(), "NullPointer: boom");
  }
  CHECK(threw);
  bool logic = false;
  try {
    (void)ok.error();
  } catch (const std::logic_error&) {
    logic = true;
  }
  CHECK(logic);

  const PropertyValue minus = std::int64_t{-129};
  CHECK(ConvertValue<int>(minus).value() == -129);
  CHECK(Fails(ConvertValue<std::int8_t>(minus), FetchErrorCode::ValueOutOfRange));
  CHECK(Fails(ConvertValue<unsigned>(minus), FetchErrorCode::ValueOutOfRange));
  CHECK(ConvertValue<double>(minus).value() == -129.0);
  const PropertyValue big = std::numeric_limits<std::uint64_t>::max();
  CHECK(Fails(ConvertValue<std::int64_t>(big), FetchErrorCode::ValueOutOfRange));
  CHECK(ConvertValue<std::uint64_t>(big).value() == std::numeric_limits<std::uint64_t>::max());
  const PropertyValue real = 2.5;
  CHECK(ConvertValue<float>(real).value() == 2.5f);
  CHECK(Fails(ConvertValue<int>(real), FetchErrorCode::TypeMismatch));
  CHECK(Fails(ConvertValue<bool>(minus), FetchErrorCode::TypeMismatch));
  CHECK(Fails(ConvertValue<std::string>(minus), FetchErrorCode::TypeMismatch));
  CHECK(ConvertValue<Address>(PropertyValue(Address{0x10})).value() == Address{0x10});

  CHECK(FormatValue(PropertyValue(true)) == "true");
  CHECK(FormatValue(PropertyValue(std::int64_t{-5})) == "-5");
  CHECK(FormatValue(PropertyValue(12.5)) == "12.5");
  CHECK(FormatValue(PropertyValue(std::string("x"))) == "x");
  CHECK(FormatValue(PropertyValue(Address{0xAB})) == "0xAB");
}

void TestSchemaRegistry() {
  TypeSchema schema("A");
  schema.Add("X", PropertyType::Int32, 0);
  CHECK(Throws([&] { schema.Add("X", PropertyType::Int32, 4); }));
  CHECK(Throws([&] { schema.Add("", PropertyType::Int32, 4); }));
  SchemaRegistry registry;
  registry.Add(std::move(schema));
  CHECK(Throws([&] { registry.Add(TypeSchema("A")); }));
  CHECK(registry.Find("A") && !registry.Find("B"));
}

// --- Статические схемы --------------------------------------------------------------

// Раскладка (64-битные указатели):
//   Actor     { u32 Id @0; u8 flags @4 (IsActive = бит 0, IsHidden = бит 1); Actor* Owner @8 }
//   Character : Actor { i32 Health @0x10; float Defense @0x14; i16 Level @0x18; u8 Team @0x1A;
//                       name-id Class @0x1C; double Experience @0x20; i64 Gold @0x28;
//                       char Title[16] @0x30; {char16_t* data; i32 len} Nickname @0x40;
//                       Stats Stats @0x50; Inventory* Inventory @0x68; i32 Slots[4] @0x70;
//                       void* Raw @0x80; {u32* data; i32 count} Tags @0x88; Object* Target @0x98 }
//   Stats     { i32 Strength @0; float Speed @4; i32 Resist[3] @8 }
//   Inventory { Item** Items @0, i32 count @8; i32 Capacity @12; {ItemRecord* data; i32 count} Records @16 }
//   Item      { i32 Quantity @0; name-id Name @4; float Weight @8 }
//   ItemRecord{ u16 Code @0; i32 Value @4; char Label[8] @8 } — 16 байт
std::shared_ptr<const SchemaRegistry> MakeSchemas() {
  auto registry = std::make_shared<SchemaRegistry>();
  TypeSchema actor("Actor");
  actor.Add("Id", PropertyType::UInt32, 0x00)
      .Add(BoolProperty("IsActive", 0x04, 0x01))
      .Add(BoolProperty("IsHidden", 0x04, 0x02))
      .Add(ObjectProperty("Owner", 0x08, "Actor"));
  registry->Add(std::move(actor));

  StringFormat nickname;
  nickname.encoding = Encoding::Utf16LE;
  nickname.chars_by_pointer = true;
  nickname.pointer_size = 8;
  nickname.length_offset = 8;
  nickname.length_size = 4;
  TypeSchema character("Character", "Actor");
  character.Add("Health", PropertyType::Int32, 0x10)
      .Add("Defense", PropertyType::Float, 0x14)
      .Add("Level", PropertyType::Int16, 0x18)
      .Add("Team", PropertyType::UInt8, 0x1A)
      .Add(NameProperty("Class", 0x1C))
      .Add("Experience", PropertyType::Double, 0x20)
      .Add("Gold", PropertyType::Int64, 0x28)
      .Add(StringProperty("Title", 0x30))
      .Add(StringProperty("Nickname", 0x40, nickname))
      .Add(StructProperty("Stats", 0x50, "Stats"))
      .Add(ObjectProperty("Inventory", 0x68, "Inventory"))
      .Add(InlineArrayProperty("Slots", 0x70, PropertyType::Int32, 4))
      .Add("Raw", PropertyType::Pointer, 0x80)
      .Add(DynamicArrayProperty("Tags", 0x88, PropertyType::Name, 8))
      .Add(ObjectProperty("Target", 0x98));  // без типа: нужна рефлексия
  registry->Add(std::move(character));

  TypeSchema stats("Stats");
  stats.Add("Strength", PropertyType::Int32, 0)
      .Add("Speed", PropertyType::Float, 4)
      .Add(InlineArrayProperty("Resist", 8, PropertyType::Int32, 3));
  registry->Add(std::move(stats));

  PropertyDesc items = DynamicArrayProperty("Items", 0, PropertyType::StructPointer, 8);
  items.struct_type = "Item";
  PropertyDesc records = DynamicArrayProperty("Records", 16, PropertyType::Struct, 8, 4, 16);
  records.struct_type = "ItemRecord";
  TypeSchema inventory("Inventory");
  inventory.Add(std::move(items)).Add("Capacity", PropertyType::Int32, 12).Add(std::move(records));
  registry->Add(std::move(inventory));

  TypeSchema item("Item");
  item.Add("Quantity", PropertyType::Int32, 0).Add(NameProperty("Name", 4)).Add("Weight", PropertyType::Float, 8);
  registry->Add(std::move(item));

  TypeSchema record("ItemRecord");
  record.Add("Code", PropertyType::UInt16, 0).Add("Value", PropertyType::Int32, 4).Add(StringProperty("Label", 8));
  registry->Add(std::move(record));
  return registry;
}

struct World {
  FakeMemory mem;
  std::uintptr_t player = 0, inventory = 0, sword = 0, potion = 0, other_potion = 0, items = 0;

  World() {
    player = mem.Alloc(0xA0);
    mem.Put<std::uint32_t>(player + 0x00, 77);
    mem.Put<std::uint8_t>(player + 0x04, 0x01);
    mem.Put<std::int32_t>(player + 0x10, 250);
    mem.Put<float>(player + 0x14, 12.5f);
    mem.Put<std::int16_t>(player + 0x18, -3);
    mem.Put<std::uint8_t>(player + 0x1A, 2);
    mem.Put<std::uint32_t>(player + 0x1C, 1);  // "Warrior"
    mem.Put<double>(player + 0x20, 1234.75);
    mem.Put<std::int64_t>(player + 0x28, -1234567890123ll);
    mem.Write(player + 0x30, "Knight", 7);
    mem.PutPointer(player + 0x40, mem.Utf16(u"\x0401\x0436\x0438\x043A"), 8);  // "Ёжик"
    mem.Put<std::int32_t>(player + 0x48, 4);
    mem.Put<std::int32_t>(player + 0x50, 18);         // Stats.Strength
    mem.Put<float>(player + 0x54, 1.25f);             // Stats.Speed
    for (int i = 0; i < 3; ++i) mem.Put<std::int32_t>(player + 0x58 + 4 * i, 10 * (i + 1));  // Resist
    for (int i = 0; i < 4; ++i) mem.Put<std::int32_t>(player + 0x70 + 4 * i, 100 + i);       // Slots
    mem.PutPointer(player + 0x80, 0xCAFE, 8);

    const std::uintptr_t tags = mem.Alloc(8);
    mem.Put<std::uint32_t>(tags, 10);
    mem.Put<std::uint32_t>(tags + 4, 11);
    mem.PutPointer(player + 0x88, tags, 8);
    mem.Put<std::int32_t>(player + 0x90, 2);

    sword = MakeItem(1, 2, 3.5f);
    potion = MakeItem(5, 3, 0.25f);
    other_potion = MakeItem(9, 3, 0.5f);
    items = mem.Alloc(16);
    mem.PutPointer(items, sword, 8);
    mem.PutPointer(items + 8, potion, 8);

    const std::uintptr_t records = mem.Alloc(32);
    mem.Put<std::uint16_t>(records, 0xBEEF);
    mem.Put<std::int32_t>(records + 4, -1);
    mem.Write(records + 8, "first", 6);
    mem.Put<std::uint16_t>(records + 16, 7);
    mem.Put<std::int32_t>(records + 20, 42);
    mem.Write(records + 24, "second", 7);

    inventory = mem.Alloc(32);
    mem.PutPointer(inventory, items, 8);
    mem.Put<std::int32_t>(inventory + 8, 2);
    mem.Put<std::int32_t>(inventory + 12, 16);
    mem.PutPointer(inventory + 16, records, 8);
    mem.Put<std::int32_t>(inventory + 24, 2);
    mem.PutPointer(player + 0x68, inventory, 8);
  }

  std::uintptr_t MakeItem(std::int32_t quantity, std::uint32_t name, float weight) {
    const std::uintptr_t item = mem.Alloc(16);
    mem.Put<std::int32_t>(item, quantity);
    mem.Put<std::uint32_t>(item + 4, name);
    mem.Put<float>(item + 8, weight);
    return item;
  }

  PropertyFetcher Fetcher() const {
    PropertyFetcher fetcher(mem.Reader(), MakeSchemas());
    fetcher.SetNameResolver(Dictionary({{1, "Warrior"}, {2, "Sword"}, {3, "Potion"}, {10, "fire"}, {11, "ice"}}));
    return fetcher;
  }
};

void TestScalars() {
  World w;
  const PropertyFetcher f = w.Fetcher();
  const ObjectRef player{w.player, "Character"};
  CHECK(f.Get<int>(player, "Health").value() == 250);
  CHECK(f.Get<float>(player, "Defense").value() == 12.5f);
  CHECK(f.Get<int>(player, "Level").value() == -3);  // знаковое расширение int16
  CHECK(f.Get<std::uint8_t>(player, "Team").value() == 2);
  CHECK(f.Get<std::string>(player, "Class").value() == "Warrior");
  CHECK(f.Get<double>(player, "Experience").value() == 1234.75);
  CHECK(f.Get<std::int64_t>(player, "Gold").value() == -1234567890123ll);
  CHECK(f.Get<std::string>(player, "Title").value() == "Knight");
  CHECK(f.Get<std::string>(player, "Nickname").value() == "\xD0\x81\xD0\xB6\xD0\xB8\xD0\xBA");
  CHECK(f.Get<Address>(player, "Raw").value() == Address{0xCAFE});

  // Унаследованные свойства и битовые поля.
  CHECK(f.Get<std::uint32_t>(player, "Id").value() == 77);
  CHECK(f.Get<bool>(player, "IsActive").value());
  CHECK(!f.Get<bool>(player, "IsHidden").value());
  CHECK(f.Locate(player, "Id")->owner_type == "Actor");
  CHECK(f.Locate(player, "Health")->owner_type == "Character");
  CHECK(f.TypeName(player).value() == "Character");

  // Безопасные преобразования.
  CHECK(f.Get<double>(player, "Health").value() == 250.0);
  CHECK(f.Get<std::uint8_t>(player, "Health").value() == 250);
  w.mem.Put<std::int32_t>(w.player + 0x10, 300);
  CHECK(Fails(f.Get<std::uint8_t>(player, "Health"), FetchErrorCode::ValueOutOfRange));
  CHECK(Fails(f.Get<unsigned>(player, "Level"), FetchErrorCode::ValueOutOfRange));
  CHECK(Fails(f.Get<int>(player, "Defense"), FetchErrorCode::TypeMismatch));
  CHECK(Fails(f.Get<std::string>(player, "Health"), FetchErrorCode::TypeMismatch));
  CHECK(f.Fetch(player, "Health").value() == PropertyValue(std::int64_t{300}));
  CHECK(FormatValue(f.Fetch(player, "Defense").value()) == "12.5");

  // Имя, которого нет в таблице, и отсутствующий resolver.
  w.mem.Put<std::uint32_t>(w.player + 0x1C, 99);
  const auto unresolved = f.Get<std::string>(player, "Class");
  CHECK(Fails(unresolved, FetchErrorCode::NameUnresolved) && Contains(unresolved.error().message, "0x63"));
  const PropertyFetcher no_names(w.mem.Reader(), MakeSchemas());
  CHECK(Fails(no_names.Get<std::string>(player, "Class"), FetchErrorCode::UnsupportedType));
}

void TestNestedAndArrays() {
  World w;
  const PropertyFetcher f = w.Fetcher();
  const ObjectRef player{w.player, "Character"};

  // Вложенная структура и массивы внутри объекта.
  CHECK(f.Get<int>(player, "Stats.Strength").value() == 18);
  CHECK(f.Get<float>(player, "Stats.Speed").value() == 1.25f);
  CHECK(f.Get<int>(player, "Stats.Resist[2]").value() == 30);
  CHECK(Fails(f.Get<int>(player, "Stats.Resist[3]"), FetchErrorCode::IndexOutOfRange));
  CHECK(f.Get<int>(player, "Slots[3]").value() == 103);
  CHECK(f.Get<Address>(player, "Stats").value() == Address{w.player + 0x50});
  CHECK(f.Get<Address>(player, "Slots").value() == Address{w.player + 0x70});

  // Через указатель: объект, динамический массив указателей, массив структур.
  CHECK(f.Get<Address>(player, "Inventory").value() == Address{w.inventory});
  CHECK(f.Get<int>(player, "Inventory.Capacity").value() == 16);
  CHECK(f.Get<int>(player, "Inventory.Items[0].Quantity").value() == 1);
  CHECK(f.Get<int>(player, "Inventory.Items[1].Quantity").value() == 5);
  CHECK(f.Get<std::string>(player, "Inventory.Items[1].Name").value() == "Potion");
  CHECK(f.Get<float>(player, "Inventory.Items[0].Weight").value() == 3.5f);
  CHECK(f.Get<Address>(player, "Inventory.Items[1]").value() == Address{w.potion});
  CHECK(f.Get<Address>(player, "Inventory.Items").value() == Address{w.items});  // адрес данных
  const auto out_of_range = f.Get<int>(player, "Inventory.Items[2].Quantity");
  CHECK(Fails(out_of_range, FetchErrorCode::IndexOutOfRange) && Contains(out_of_range.error().message, "count is 2"));
  CHECK(f.Get<std::uint16_t>(player, "Inventory.Records[0].Code").value() == 0xBEEF);
  CHECK(f.Get<int>(player, "Inventory.Records[1].Value").value() == 42);
  CHECK(f.Get<std::string>(player, "Inventory.Records[1].Label").value() == "second");
  CHECK(f.Get<std::string>(player, "Tags[1]").value() == "ice");

  CHECK(f.Count(player, "Inventory.Items").value() == 2);
  CHECK(f.Count(player, "Slots").value() == 4);
  CHECK(f.Count(player, "Tags").value() == 2);
  CHECK(Fails(f.Count(player, "Health"), FetchErrorCode::TypeMismatch));

  // Мусорная длина массива.
  w.mem.Put<std::int32_t>(w.inventory + 8, 0x7FFFFFFF);
  CHECK(Fails(f.Get<int>(player, "Inventory.Items[0].Quantity"), FetchErrorCode::ValueOutOfRange));
  CHECK(Fails(f.Count(player, "Inventory.Items"), FetchErrorCode::ValueOutOfRange));
  // Пустой указатель на данные при ненулевой длине.
  w.mem.Put<std::int32_t>(w.inventory + 8, 2);
  w.mem.PutPointer(w.inventory, 0, 8);
  CHECK(Fails(f.Get<int>(player, "Inventory.Items[0].Quantity"), FetchErrorCode::NullPointer));
}

void TestErrors() {
  World w;
  const PropertyFetcher f = w.Fetcher();
  const ObjectRef player{w.player, "Character"};

  const auto typo = f.Get<int>(player, "Helth");
  CHECK(Fails(typo, FetchErrorCode::UnknownProperty));
  CHECK(Contains(typo.error().message, "did you mean 'Health'"));
  CHECK(!Contains(f.Get<int>(player, "Zzzzzzzz").error().message, "did you mean"));
  CHECK(Fails(f.Get<int>(player, "Inventory.Items[0].Health"), FetchErrorCode::UnknownProperty));
  CHECK(Fails(f.Get<int>(ObjectRef{w.player, "Nope"}, "Health"), FetchErrorCode::UnknownType));
  CHECK(Fails(f.Get<int>(ObjectRef{w.player}, "Health"), FetchErrorCode::UnknownType));  // нет типа и рефлексии
  CHECK(Fails(f.Get<int>(player, "Target.Health"), FetchErrorCode::UnknownType));

  CHECK(Fails(f.Get<int>(player, "Health.Value"), FetchErrorCode::InvalidPath));
  CHECK(Fails(f.Get<int>(player, "Stats[0]"), FetchErrorCode::InvalidPath));
  CHECK(Fails(f.Get<int>(player, "Stats..Speed"), FetchErrorCode::InvalidPath));

  // Пустой указатель: значение — нулевой адрес, переход дальше — NullPointer с путём.
  CHECK(f.Get<Address>(player, "Owner").value() == Address{0});
  const auto null_owner = f.Get<std::uint32_t>(player, "Owner.Id");
  CHECK(Fails(null_owner, FetchErrorCode::NullPointer) && Contains(null_owner.error().message, "Owner.Id"));
  // Указатель в никуда: UnreadableMemory, без падения.
  w.mem.PutPointer(w.player + 0x68, 0xDEAD0000, 8);
  CHECK(Fails(f.Get<int>(player, "Inventory.Capacity"), FetchErrorCode::UnreadableMemory));
  CHECK(Fails(f.Get<int>(ObjectRef{w.mem.end() + 0x1000, "Character"}, "Health"), FetchErrorCode::UnreadableMemory));

  // Цикл наследования в описаниях не вешает поиск.
  auto cyclic = std::make_shared<SchemaRegistry>();
  cyclic->Add(TypeSchema("A", "B").Add("X", PropertyType::Int32, 0));
  cyclic->Add(TypeSchema("B", "A").Add("Y", PropertyType::Int32, 4));
  const PropertyFetcher looped(w.mem.Reader(), cyclic);
  CHECK(looped.Get<int>(ObjectRef{w.player, "A"}, "Y").value() == 1);  // Y @4: байт флагов 0x01 и три нуля
  CHECK(Fails(looped.Get<int>(ObjectRef{w.player, "A"}, "Z"), FetchErrorCode::UnknownProperty));
}

void TestLocateAndPointerPathRoots() {
  World w;
  const PropertyFetcher f = w.Fetcher();
  const ObjectRef player{w.player, "Character"};

  // Locate строит PointerPath; Read() разрешает его заново и видит изменения.
  const auto location = f.Locate(player, "Inventory.Items[1].Quantity");
  CHECK(location && location->owner_type == "Item" && location->property.type == PropertyType::Int32);
  CHECK(location->property.name == "Inventory.Items[1].Quantity");
  char expected[64];
  std::snprintf(expected, sizeof(expected), "[[[0x%llX]]+0x8]", static_cast<unsigned long long>(w.player + 0x68));
  CHECK(location->path.ToString() == expected);
  CHECK(f.Read(*location).value() == PropertyValue(std::int64_t{5}));
  w.mem.PutPointer(w.items + 8, w.other_potion, 8);  // программа заменила предмет
  CHECK(f.Read(*location).value() == PropertyValue(std::int64_t{9}));

  // Корень — глобальный указатель (как после поиска сигнатурой): объект берётся заново.
  const std::uintptr_t global = w.mem.Alloc(8);
  w.mem.PutPointer(global, w.player, 8);
  const ObjectRef via_global{PointerPath(global, {0}), "Character"};
  CHECK(f.Get<int>(via_global, "Stats.Strength").value() == 18);
  const std::uintptr_t clone = w.mem.Alloc(0xA0);
  w.mem.Put<std::int32_t>(clone + 0x50, 99);
  w.mem.PutPointer(global, clone, 8);
  CHECK(f.Get<int>(via_global, "Stats.Strength").value() == 99);
  w.mem.PutPointer(global, 0, 8);
  CHECK(Fails(f.Get<int>(via_global, "Stats.Strength"), FetchErrorCode::NullPointer));
}

void TestDump() {
  World w;
  const PropertyFetcher f = w.Fetcher();
  const auto entries = f.Dump(ObjectRef{w.player, "Character"});
  CHECK(entries && entries->size() == 19);
  CHECK((*entries)[0].name == "Id" && (*entries)[0].owner_type == "Actor");  // родительские — первыми
  CHECK((*entries)[4].name == "Health" && FormatValue((*entries)[4].value.value()) == "250");
  bool saw_target = false;
  for (const PropertyEntry& entry : *entries) {
    if (entry.name == "Target") saw_target = entry.value && entry.value.value() == PropertyValue(Address{0});
  }
  CHECK(saw_target);
  w.mem.Put<std::uint32_t>(w.player + 0x1C, 99);  // ошибка одного свойства не мешает остальным
  const auto partial = f.Dump(ObjectRef{w.player, "Character"});
  CHECK(partial && Fails((*partial)[8].value, FetchErrorCode::NameUnresolved) && (*partial)[9].value);
  CHECK(Fails(f.Dump(ObjectRef{w.player, "Nope"}), FetchErrorCode::UnknownType));
}

void TestThirtyTwoBitTarget() {
  FakeMemory mem;
  const std::uintptr_t inner = mem.Alloc(8);
  mem.Put<std::int32_t>(inner, -42);
  const std::uintptr_t outer = mem.Alloc(16);
  mem.PutPointer(outer, inner, 4);
  mem.Put<std::int32_t>(outer + 4, 2);
  mem.PutPointer(outer + 8, inner, 4);  // массив из двух 32-битных указателей
  mem.PutPointer(outer + 12, inner, 4);

  auto schemas = std::make_shared<SchemaRegistry>();
  PropertyDesc refs = InlineArrayProperty("Refs", 8, PropertyType::StructPointer, 2);
  refs.struct_type = "Inner";
  schemas->Add(TypeSchema("Outer").Add(ObjectProperty("Inner", 0, "Inner")).Add("Count", PropertyType::Int32, 4).Add(refs));
  schemas->Add(TypeSchema("Inner").Add("Value", PropertyType::Int32, 0));

  FetcherOptions options;
  options.pointer_size = 4;
  const PropertyFetcher f(mem.Reader(), schemas, options);
  CHECK(f.Get<int>(ObjectRef{outer, "Outer"}, "Inner.Value").value() == -42);
  CHECK(f.Get<int>(ObjectRef{outer, "Outer"}, "Refs[1].Value").value() == -42);  // шаг = 4 байта
  CHECK(f.Get<Address>(ObjectRef{outer, "Outer"}, "Inner").value() == Address{inner});
}

// --- Динамическая рефлексия -------------------------------------------------------------

// Метаданные в памяти цели (64 бита):
//   TypeInfo { u32 name_id @0; TypeInfo* super @8; PropInfo* first @16 }
//   PropInfo { u32 name_id @0; u32 offset @4; u32 tag @8; TypeInfo* inner @16; PropInfo* next @24 }
//   каждый объект: TypeInfo* type @0
struct ReflectedWorld {
  FakeMemory mem{1 << 16};
  std::uintptr_t object_type = 0, character_type = 0, inventory_type = 0, item_type = 0, potion_type = 0;
  std::uintptr_t hero = 0, inventory = 0;

  enum : std::uint32_t {
    kObject = 100, kCharacter, kHealth, kDefense, kInventory, kItem, kQuantity, kItems, kId, kMystery, kName,
    kPotion, kPotency, kSword
  };

  std::uintptr_t Type(std::uint32_t name, std::uintptr_t super) {
    const std::uintptr_t t = mem.Alloc(24);
    mem.Put<std::uint32_t>(t, name);
    mem.PutPointer(t + 8, super, 8);
    return t;
  }

  // Добавляет дескриптор в конец списка свойств типа.
  std::uintptr_t Prop(std::uintptr_t type, std::uint32_t name, std::uint32_t offset, std::uint32_t tag,
                      std::uintptr_t inner = 0) {
    const std::uintptr_t p = mem.Alloc(32);
    mem.Put<std::uint32_t>(p, name);
    mem.Put<std::uint32_t>(p + 4, offset);
    mem.Put<std::uint32_t>(p + 8, tag);
    mem.PutPointer(p + 16, inner, 8);
    std::uintptr_t link = type + 16;  // поле first, затем next последнего
    for (auto next = mem.Reader()->ReadPointer(link, 8); next && *next; next = mem.Reader()->ReadPointer(link, 8)) {
      link = *next + 24;
    }
    mem.PutPointer(link, p, 8);
    return p;
  }

  std::uintptr_t Object(std::uintptr_t type, std::size_t size) {
    const std::uintptr_t o = mem.Alloc(size);
    mem.PutPointer(o, type, 8);
    return o;
  }

  ReflectedWorld() {
    object_type = Type(kObject, 0);
    Prop(object_type, kId, 8, 1);
    character_type = Type(kCharacter, object_type);
    inventory_type = Type(kInventory, object_type);
    item_type = Type(kItem, object_type);
    potion_type = Type(kPotion, item_type);
    Prop(character_type, kHealth, 12, 1);
    Prop(character_type, kDefense, 16, 2);
    Prop(character_type, kInventory, 24, 3, inventory_type);
    Prop(character_type, kMystery, 32, 99);  // неизвестный тег
    Prop(inventory_type, kItems, 16, 4, item_type);
    Prop(item_type, kQuantity, 12, 1);
    Prop(item_type, kName, 16, 5);
    Prop(potion_type, kPotency, 20, 1);
    Prop(potion_type, kId, 24, 1);  // переопределяет Object.Id

    const std::uintptr_t sword = Object(item_type, 24);
    mem.Put<std::int32_t>(sword + 8, 1001);
    mem.Put<std::int32_t>(sword + 12, 1);
    mem.Put<std::uint32_t>(sword + 16, kSword);
    const std::uintptr_t potion = Object(potion_type, 32);
    mem.Put<std::int32_t>(potion + 8, 1002);
    mem.Put<std::int32_t>(potion + 12, 5);
    mem.Put<std::uint32_t>(potion + 16, kPotion);
    mem.Put<std::int32_t>(potion + 20, 75);
    mem.Put<std::int32_t>(potion + 24, 2002);
    const std::uintptr_t items = mem.Alloc(16);
    mem.PutPointer(items, sword, 8);
    mem.PutPointer(items + 8, potion, 8);

    inventory = Object(inventory_type, 32);
    mem.PutPointer(inventory + 16, items, 8);
    mem.Put<std::int32_t>(inventory + 24, 2);

    hero = Object(character_type, 40);
    mem.Put<std::int32_t>(hero + 8, 7);
    mem.Put<std::int32_t>(hero + 12, 90);
    mem.Put<float>(hero + 16, 4.5f);
    mem.PutPointer(hero + 24, inventory, 8);
  }

  std::shared_ptr<const GenericDataResolver> Names() const {
    return Dictionary({{kObject, "Object"}, {kCharacter, "Character"}, {kHealth, "Health"}, {kDefense, "Defense"},
                       {kInventory, "Inventory"}, {kItem, "Item"}, {kQuantity, "Quantity"}, {kItems, "Items"},
                       {kId, "Id"}, {kMystery, "Mystery"}, {kName, "Name"}, {kPotion, "Potion"},
                       {kPotency, "Potency"}, {kSword, "Sword"}});
  }

  static ReflectionLayout Layout() {
    ReflectionLayout layout;
    layout.pointer_size = 8;
    layout.object_type_offset = 0;
    layout.type_name_offset = 0;
    layout.super_offset = 8;
    layout.list_kind = PropertyListKind::LinkedList;
    layout.list_offset = 16;
    layout.next_offset = 24;
    layout.property_name_offset = 0;
    layout.property_offset_offset = 4;
    layout.property_tag_offset = 8;
    layout.property_inner_type_offset = 16;
    layout.tags[1] = ScalarProperty("", PropertyType::Int32, 0);
    layout.tags[2] = ScalarProperty("", PropertyType::Float, 0);
    layout.tags[3] = ObjectProperty("", 0);
    layout.tags[4] = DynamicArrayProperty("", 0, PropertyType::StructPointer, 8);
    layout.tags[5] = NameProperty("", 0);
    return layout;
  }

  std::shared_ptr<ReflectionLoader> Loader(ReflectionLayout layout = Layout()) const {
    return std::make_shared<ReflectionLoader>(mem.Reader(), Names(), std::move(layout));
  }

  PropertyFetcher Fetcher(std::shared_ptr<const ReflectionLoader> loader) const {
    PropertyFetcher fetcher(mem.Reader());
    fetcher.SetReflection(std::move(loader)).SetNameResolver(Names());
    return fetcher;
  }
};

void TestReflection() {
  ReflectedWorld w;
  const auto loader = w.Loader();
  const PropertyFetcher f = w.Fetcher(loader);
  const ObjectRef hero{w.hero};  // тип не указан — берётся из памяти

  CHECK(f.TypeName(hero).value() == "Character");
  CHECK(f.Get<int>(hero, "Health").value() == 90);
  CHECK(f.Get<float>(hero, "Defense").value() == 4.5f);
  CHECK(f.Get<int>(hero, "Id").value() == 7);  // унаследовано от Object
  CHECK(f.Get<int>(hero, "Inventory.Items[0].Quantity").value() == 1);
  CHECK(f.Get<std::string>(hero, "Inventory.Items[0].Name").value() == "Sword");
  CHECK(f.Count(hero, "Inventory.Items").value() == 2);
  // Элемент — наследник Item: доступны его свойства, а переопределённое Id — его версия.
  CHECK(f.Get<int>(hero, "Inventory.Items[1].Potency").value() == 75);
  CHECK(f.Get<int>(hero, "Inventory.Items[1].Id").value() == 2002);

  // Неизвестный тег: свойство видно в выгрузке, но не читается.
  CHECK(Fails(f.Fetch(hero, "Mystery"), FetchErrorCode::UnsupportedType));
  const auto dump = f.Dump(hero);
  CHECK(dump && dump->size() == 5 && (*dump)[0].name == "Id" && (*dump)[4].type == PropertyType::Unknown);
  const auto typo = f.Get<int>(hero, "Defence");
  CHECK(Fails(typo, FetchErrorCode::UnknownProperty) && Contains(typo.error().message, "'Defense'"));

  // Схемы кешируются по адресу типа.
  const auto first = loader->Load(w.character_type);
  CHECK(first && first.value().get() == loader->Load(w.character_type).value().get());
  loader->ClearCache();
  CHECK(loader->Load(w.character_type).value().get() != first.value().get());

  // Объект без типа и тип без имени в таблице.
  const std::uintptr_t orphan = w.mem.Alloc(16);
  CHECK(Fails(f.Get<int>(ObjectRef{orphan}, "Id"), FetchErrorCode::NullPointer));
  const std::uintptr_t anonymous_type = w.Type(555, w.object_type);
  CHECK(f.TypeName(ObjectRef{w.Object(anonymous_type, 16)}).value() == "<unknown:0x22B>");

  // Статическая схема может ссылаться на объект без типа — тип определяется по памяти.
  auto schemas = std::make_shared<SchemaRegistry>();
  schemas->Add(TypeSchema("Holder").Add(ObjectProperty("Hero", 0)));
  const std::uintptr_t holder = w.mem.Alloc(8);
  w.mem.PutPointer(holder, w.hero, 8);
  PropertyFetcher mixed(w.mem.Reader(), schemas);
  mixed.SetReflection(loader);
  CHECK(mixed.Get<int>(ObjectRef{holder, "Holder"}, "Hero.Inventory.Items[1].Potency").value() == 75);
}

void TestReflectionLayouts() {
  ReflectedWorld w;

  // Повреждённые метаданные: цикл в цепочке родителей и в списке свойств.
  w.mem.PutPointer(w.object_type + 8, w.character_type, 8);  // Object.super = Character
  const auto looped = w.Loader()->Load(w.character_type);
  CHECK(looped && looped.value()->properties().size() == 5);
  const std::uintptr_t first = w.mem.Reader()->ReadPointer(w.item_type + 16, 8).value();
  w.mem.PutPointer(first + 24, first, 8);  // Quantity.next = Quantity
  const auto item = w.Loader()->Load(w.item_type);
  CHECK(item && item.value()->Find("Quantity") && !item.value()->Find("Name"));
  CHECK(Fails(w.Loader()->Load(w.mem.end() + 64), FetchErrorCode::UnreadableMemory));

  // Список свойств — массив указателей {PropInfo** data @32; u32 count @40}.
  FakeMemory& mem = w.mem;
  const std::uintptr_t type = mem.Alloc(48);
  mem.Put<std::uint32_t>(type, ReflectedWorld::kItem);
  const std::uintptr_t props = mem.Alloc(16);
  for (int i = 0; i < 2; ++i) {
    const std::uintptr_t p = mem.Alloc(32);
    mem.Put<std::uint32_t>(p, i == 0 ? ReflectedWorld::kQuantity : ReflectedWorld::kPotency);
    mem.Put<std::uint32_t>(p + 4, 8 + 4 * i);
    mem.Put<std::uint32_t>(p + 8, 1);
    mem.PutPointer(props + 8 * i, p, 8);
  }
  mem.PutPointer(type + 32, props, 8);
  mem.Put<std::uint32_t>(type + 40, 2);
  ReflectionLayout array_layout = ReflectedWorld::Layout();
  array_layout.super_offset.reset();
  array_layout.list_kind = PropertyListKind::PointerArray;
  array_layout.list_offset = 32;
  array_layout.list_count_offset = 40;
  const auto object = w.Object(type, 16);
  mem.Put<std::int32_t>(object + 8, 3);
  mem.Put<std::int32_t>(object + 12, 4);
  const PropertyFetcher f = w.Fetcher(w.Loader(array_layout));
  CHECK(f.Get<int>(ObjectRef{object}, "Quantity").value() == 3);
  CHECK(f.Get<int>(ObjectRef{object}, "Potency").value() == 4);
  mem.Put<std::uint32_t>(type + 40, 100000);
  CHECK(Fails(w.Loader(array_layout)->Load(type), FetchErrorCode::ValueOutOfRange));

  // Свой классификатор вместо таблицы тегов.
  ReflectionLayout custom = ReflectedWorld::Layout();
  custom.tags.clear();
  custom.classify = [](const MemoryReader& reader, std::uintptr_t descriptor) -> std::optional<PropertyDesc> {
    if (reader.ReadUnsigned(descriptor + 8, 4) == 1u) return ScalarProperty("", PropertyType::UInt32, 0);
    return std::nullopt;
  };
  const PropertyFetcher classified = w.Fetcher(w.Loader(custom));
  CHECK(classified.Get<std::uint32_t>(ObjectRef{w.hero}, "Health").value() == 90u);
  CHECK(Fails(classified.Fetch(ObjectRef{w.hero}, "Defense"), FetchErrorCode::UnsupportedType));

  CHECK(Throws([&] { ReflectionLoader l(nullptr, w.Names(), ReflectedWorld::Layout()); }));
  CHECK(Throws([&] { ReflectionLoader l(w.mem.Reader(), nullptr, ReflectedWorld::Layout()); }));
}

void TestConcurrentFetch() {
  ReflectedWorld w;
  const PropertyFetcher f = w.Fetcher(w.Loader());
  std::atomic<bool> ok{true};
  std::vector<std::thread> threads;
  for (int t = 0; t < 4; ++t) {
    threads.emplace_back([&] {
      for (int i = 0; i < 300; ++i) {
        const ObjectRef hero{w.hero};
        if (f.Get<int>(hero, "Health").value_or(-1) != 90) ok = false;
        if (f.Get<int>(hero, "Inventory.Items[1].Potency").value_or(-1) != 75) ok = false;
        if (f.Get<std::string>(hero, "Inventory.Items[0].Name").value_or("") != "Sword") ok = false;
      }
    });
  }
  for (auto& thread : threads) thread.join();
  CHECK(ok);
}

// --- Собственный процесс --------------------------------------------------------------

#if defined(MEMREFL_HAS_PROCESS_READER)

struct RealStats {
  std::int32_t hp;
  float armor;
  const char* label;
  RealStats* next;
};

void TestInProcess() {
  static RealStats tail{5, 0.5f, "tail", reinterpret_cast<RealStats*>(0x10)};  // мусорный указатель
  static RealStats head{100, 2.25f, "head", &tail};

  auto schemas = std::make_shared<SchemaRegistry>();
  StringFormat by_pointer;
  by_pointer.chars_by_pointer = true;
  schemas->Add(TypeSchema("RealStats")
                   .Add("hp", PropertyType::Int32, offsetof(RealStats, hp))
                   .Add("armor", PropertyType::Float, offsetof(RealStats, armor))
                   .Add(StringProperty("label", offsetof(RealStats, label), by_pointer))
                   .Add(ObjectProperty("next", offsetof(RealStats, next), "RealStats")));
  const PropertyFetcher f(std::make_shared<ProcessMemoryReader>(ProcessMemoryReader::Self()), schemas);
  const ObjectRef object{reinterpret_cast<std::uintptr_t>(&head), "RealStats"};
  CHECK(f.Get<int>(object, "hp").value() == 100);
  CHECK(f.Get<double>(object, "armor").value() == 2.25);
  CHECK(f.Get<std::string>(object, "label").value() == "head");
  CHECK(f.Get<std::string>(object, "next.label").value() == "tail");
  CHECK(Fails(f.Get<int>(object, "next.next.hp"), FetchErrorCode::UnreadableMemory));  // 0x10 — без падения
}

#endif

}  // namespace

int main() {
  TestPathParsing();
  TestResultAndConversions();
  TestSchemaRegistry();
  TestScalars();
  TestNestedAndArrays();
  TestErrors();
  TestLocateAndPointerPathRoots();
  TestDump();
  TestThirtyTwoBitTarget();
  TestReflection();
  TestReflectionLayouts();
  TestConcurrentFetch();
#if defined(MEMREFL_HAS_PROCESS_READER)
  TestInProcess();
#endif
  std::puts("all property fetcher tests passed");
  return 0;
}

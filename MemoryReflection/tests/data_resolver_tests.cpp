// Тесты модуля 2 (Generic Data Resolver). Память цели имитируется буфером с
// фиктивным базовым адресом (FakeMemory) и читается через RegionReader — так
// же, как снимок чужого процесса. Отдельно проверяется безопасное чтение
// собственного процесса (ProcessMemoryReader): невалидные указатели не роняют
// процесс.

#include "memrefl/data_resolver.hpp"
#include "memrefl/memory_reader.hpp"
#include "memrefl/name_table.hpp"
#include "memrefl/text.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__linux__)
#include <sys/mman.h>
#endif

using namespace memrefl;
using memrefl::testing::FakeMemory;
using memrefl::testing::Throws;

namespace {

const std::string kFffd = "\xEF\xBF\xBD";  // U+FFFD в UTF-8

// --- Кодировки и хеши ------------------------------------------------------------

void TestUtf8() {
  for (const char* valid : {"", "hello", "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82",  // "Привет"
                            "\xE6\x97\xA5\xE6\x9C\xAC", "\xF0\x9F\x98\x80", "\xEF\xBF\xBD"}) {
    CHECK(IsValidUtf8(valid));
    CHECK(SanitizeUtf8(valid) == valid);
  }
  // Битые последовательности: по одному U+FFFD на "максимальную часть".
  struct Case {
    const char* input;
    std::string expected;
  };
  const Case cases[] = {
      {"\xC0\x80", kFffd + kFffd},                         // overlong
      {"\xE0\x80\x80", kFffd + kFffd + kFffd},             // overlong
      {"\xED\xA0\x80", kFffd + kFffd + kFffd},             // суррогат в UTF-8
      {"\xF4\x90\x80\x80", kFffd + kFffd + kFffd + kFffd},  // > U+10FFFF
      {"\xE2\x82", kFffd},                                 // обрыв в конце
      {"a\xE2\x82" "b", "a" + kFffd + "b"},                // обрыв в середине
      {"\xFF", kFffd},
      {"x\x80y", "x" + kFffd + "y"},
  };
  for (const Case& c : cases) {
    CHECK(!IsValidUtf8(c.input));
    CHECK(SanitizeUtf8(c.input) == c.expected);
    CHECK(IsValidUtf8(SanitizeUtf8(c.input)));
  }
}

void TestUtf16AndLatin1() {
  CHECK(Utf16ToUtf8(u"\x041F\x0440\x0438\x0432\x0435\x0442") ==
        "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82");                  // "Привет"
  CHECK(Utf16ToUtf8(std::u16string{0xD83D, 0xDE00}) == "\xF0\x9F\x98\x80");  // суррогатная пара
  CHECK(Utf16ToUtf8(std::u16string{0xD83D, u'x'}) == kFffd + "x");          // одиночный старший
  CHECK(Utf16ToUtf8(std::u16string{0xDE00}) == kFffd);                      // одиночный младший
  CHECK(Utf16ToUtf8(std::u16string{u'a', 0xD83D}) == "a" + kFffd);          // обрыв пары в конце

  const unsigned char le[] = {'H', 0, 'i', 0, 0x3D, 0xD8, 0x00, 0xDE, 0x42};  // "Hi😀" + лишний байт
  CHECK(ToUtf8(le, sizeof(le), Encoding::Utf16LE) == "Hi\xF0\x9F\x98\x80");
  CHECK(ToUtf8("Caf\xE9", 4, Encoding::Latin1) == "Caf\xC3\xA9");
  CHECK(ToUtf8("ok\xFF", 3, Encoding::Utf8) == "ok" + kFffd);

  const std::string mixed = "A\xD0\x96\xF0\x9F\x98\x80";  // A Ж 😀
  CHECK(Utf8ToUtf16(mixed) == (std::u16string{u'A', 0x0416, 0xD83D, 0xDE00}));
  CHECK(Utf16ToUtf8(Utf8ToUtf16(mixed)) == mixed);
  CHECK(Utf8ToWide(mixed).size() == (sizeof(wchar_t) == 2 ? 4u : 3u));
}

void TestHashes() {
  auto hash = [](HashAlgorithm a, std::string_view s) { return HashBytes(a, s.data(), s.size()); };
  CHECK(hash(HashAlgorithm::Fnv1a32, "") == 0x811C9DC5u);
  CHECK(hash(HashAlgorithm::Fnv1a32, "a") == 0xE40C292Cu);
  CHECK(hash(HashAlgorithm::Fnv1a32, "foobar") == 0xBF9CF968u);
  CHECK(hash(HashAlgorithm::Fnv1_32, "a") == 0x050C5D7Eu);
  CHECK(hash(HashAlgorithm::Fnv1a64, "a") == 0xAF63DC4C8601EC8Cull);
  CHECK(hash(HashAlgorithm::Fnv1a64, "foobar") == 0x85944171F73967E8ull);
  CHECK(hash(HashAlgorithm::Crc32, "123456789") == 0xCBF43926u);
  CHECK(hash(HashAlgorithm::Crc32, "The quick brown fox jumps over the lazy dog") == 0x414FA339u);

  StringHashOptions lower;
  lower.ascii_lowercase = true;
  CHECK(HashString("PlayerName", lower) == HashString("playername"));
  CHECK(HashString("PlayerName") != HashString("playername"));

  StringHashOptions wide;
  wide.input = Encoding::Utf16LE;
  wide.algorithm = HashAlgorithm::Crc32;
  const char utf16_bytes[] = {'I', 0, 'D', 0};
  CHECK(HashString("ID", wide) == HashBytes(HashAlgorithm::Crc32, utf16_bytes, 4));

  StringHashOptions latin1;
  latin1.input = Encoding::Latin1;
  CHECK(HashString("Caf\xC3\xA9", latin1) == hash(HashAlgorithm::Fnv1a32, "Caf\xE9"));
}

// --- Чтение памяти -------------------------------------------------------------------

void TestRegionReaderAndPointerPath() {
  FakeMemory mem;
  const std::uintptr_t a = mem.Alloc(8);
  mem.Put<std::uint32_t>(a, 0xDEADBEEF);
  const auto reader = mem.Reader();
  CHECK(reader->Read<std::uint32_t>(a) == 0xDEADBEEFu);
  CHECK(reader->ReadUnsigned(a, 2) == 0xBEEFu);
  CHECK(!reader->ReadUnsigned(a, 9));
  CHECK(!reader->Read<std::uint32_t>(mem.end() - 2));  // выходит за конец
  CHECK(!reader->Read<std::uint8_t>(mem.base() - 1));

  // Цепочка: global -> object; object + 0x10 -> inner; результат inner + 0x8.
  const std::uintptr_t global = mem.Alloc(8);
  const std::uintptr_t object = mem.Alloc(0x20);
  const std::uintptr_t inner = mem.Alloc(0x20);
  for (const std::size_t width : {std::size_t{8}, std::size_t{4}}) {
    mem.PutPointer(global, object, width);
    mem.PutPointer(object + 0x10, inner, width);
    CHECK(PointerPath(global).Resolve(*reader, width) == global);
    CHECK(PointerPath(global, {0}).Resolve(*reader, width) == object);
    CHECK(PointerPath(global, {0x10, 0x8}).Resolve(*reader, width) == inner + 0x8);
  }
  mem.PutPointer(object + 0x10, 0, 8);
  CHECK(!PointerPath(global, {0x10, 0x8}).Resolve(*reader, 8));  // нулевой указатель в цепочке
  CHECK(!PointerPath(mem.end(), {0}).Resolve(*reader, 8));       // нечитаемый адрес
}

#if defined(MEMREFL_HAS_PROCESS_READER)

// Две страницы, вторая освобождена: [readable page][unmapped].
struct GuardedPage {
  std::uint8_t* page = nullptr;
  std::size_t size = 0;

  GuardedPage() {
#if defined(_WIN32)
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    size = info.dwPageSize;
    page = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, 2 * size, MEM_RESERVE, PAGE_NOACCESS));
    CHECK(page != nullptr);
    CHECK(VirtualAlloc(page, size, MEM_COMMIT, PAGE_READWRITE) != nullptr);
#else
    size = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
    void* p = mmap(nullptr, 2 * size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(p != MAP_FAILED);
    page = static_cast<std::uint8_t*>(p);
    CHECK(munmap(page + size, size) == 0);
#endif
  }
  ~GuardedPage() {
#if defined(_WIN32)
    VirtualFree(page, 0, MEM_RELEASE);
#else
    munmap(page, size);
#endif
  }
  std::uintptr_t end() const { return reinterpret_cast<std::uintptr_t>(page + size); }
};

// Wine читает собственный процесс без учёта защиты страниц (PAGE_NOACCESS и
// зарезервированные страницы читаются успешно), а настоящая Windows по
// документации ReadProcessMemory завершается ошибкой, если чтение задевает
// недоступную область. Проверки на границе страницы под Wine пропускаются.
bool PageProtectionEnforced() {
#if defined(_WIN32)
  const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  return ntdll == nullptr || GetProcAddress(ntdll, "wine_get_version") == nullptr;
#else
  return true;
#endif
}

void TestProcessReader() {
  const ProcessMemoryReader self = ProcessMemoryReader::Self();
  static const std::uint64_t value = 0x1122334455667788ull;
  CHECK(self.Read<std::uint64_t>(reinterpret_cast<std::uintptr_t>(&value)) == value);
  CHECK(!self.Read<std::uint64_t>(0x10));  // невалидный адрес — false, а не падение

  GuardedPage guard;
  // Строка, кончающаяся у самой границы доступной памяти: чтение блоками
  // по страницам не задевает следующую страницу.
  const char edge[] = "edge-of-page";
  std::memcpy(guard.page + guard.size - sizeof(edge), edge, sizeof(edge));
  CHECK(ReadString(self, guard.end() - sizeof(edge), StringFormat{}) == std::string("edge-of-page"));
  CHECK(self.Read<std::uint32_t>(guard.end() - 4).has_value());
  if (PageProtectionEnforced()) {
    CHECK(!self.Read<std::uint64_t>(guard.end() - 4));  // половина значения на недоступной странице
    // Без завершающего нуля до конца страницы — nullopt, без падения.
    std::memset(guard.page + guard.size - 16, 'x', 16);
    CHECK(!ReadString(self, guard.end() - 16, StringFormat{}));
  } else {
    std::puts("note: page-protection checks skipped (Wine does not enforce them for ReadProcessMemory)");
  }

  // Таблица в собственном процессе с мусорным указателем.
  static const char* const names[] = {"alpha", "beta", nullptr, reinterpret_cast<const char*>(0x10),
                                      "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82"};
  ArrayTableSpec spec;
  spec.array = reinterpret_cast<std::uintptr_t>(names);
  spec.count = 5;
  const GenericDataResolver resolver(std::make_shared<ProcessMemoryReader>(self), spec);
  CHECK(resolver.Resolve(0) == std::string("alpha"));
  CHECK(resolver.Resolve(1) == std::string("beta"));
  CHECK(!resolver.Resolve(2));  // пустой слот
  CHECK(!resolver.Resolve(3));  // мусорный указатель
  CHECK(resolver.Resolve(4) == std::string("\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82"));
  CHECK(!resolver.Resolve(5));  // за пределами count
}

#endif  // MEMREFL_HAS_PROCESS_READER

// --- Строки --------------------------------------------------------------------------

void TestReadString() {
  FakeMemory mem;
  const auto reader = mem.Reader();

  // Завершающий ноль: UTF-8, UTF-16, Latin-1.
  StringFormat utf8;
  CHECK(ReadString(*reader, mem.Utf8("plain"), utf8) == std::string("plain"));
  CHECK(ReadString(*reader, mem.Utf8(""), utf8) == std::string(""));
  CHECK(ReadString(*reader, mem.Utf8("bad\xC0name"), utf8) == "bad" + kFffd + "name");
  StringFormat utf16;
  utf16.encoding = Encoding::Utf16LE;
  CHECK(ReadString(*reader, mem.Utf16(u"\x0428\x0438\x0440\x043E\x043A\x0438\x0439"), utf16) ==
        "\xD0\xA8\xD0\xB8\xD1\x80\xD0\xBE\xD0\xBA\xD0\xB8\xD0\xB9");  // "Широкий"
  StringFormat latin1;
  latin1.encoding = Encoding::Latin1;
  CHECK(ReadString(*reader, mem.Utf8("Caf\xE9"), latin1) == "Caf\xC3\xA9");

  // Поле длины перед символами: {uint16 length; char chars[]}.
  const std::uintptr_t prefixed = mem.Alloc(16);
  mem.Put<std::uint16_t>(prefixed, 5);
  mem.Write(prefixed + 2, "hello world", 11);  // длина ограничивает, ноль не нужен
  StringFormat with_length;
  with_length.length_size = 2;
  with_length.chars_offset = 2;
  CHECK(ReadString(*reader, prefixed, with_length) == std::string("hello"));

  // Указатель на символы + длина с учётом \0: {char16_t* data; int32 num; int32 max}.
  const std::uintptr_t dyn = mem.Alloc(16);
  mem.PutPointer(dyn, mem.Utf16(u"Dynamic"), 8);
  mem.Put<std::int32_t>(dyn + 8, 8);
  StringFormat dynamic;
  dynamic.encoding = Encoding::Utf16LE;
  dynamic.chars_by_pointer = true;
  dynamic.pointer_size = 8;
  dynamic.length_offset = 8;
  dynamic.length_size = 4;
  CHECK(ReadString(*reader, dyn, dynamic) == std::string("Dynamic"));
  // Пустая строка {nullptr, 0} — "", а {nullptr, 3} — ошибка.
  const std::uintptr_t empty = mem.Alloc(16);
  CHECK(ReadString(*reader, empty, dynamic) == std::string(""));
  mem.Put<std::int32_t>(empty + 8, 3);
  CHECK(!ReadString(*reader, empty, dynamic));

  // Длина упакована с флагами: uint16 header, бит 0 — UTF-16, длина = header >> 6.
  StringFormat packed;
  packed.encoding = Encoding::Latin1;
  packed.length_size = 2;
  packed.length_shift = 6;
  packed.utf16_flag_mask = 1;
  packed.chars_offset = 2;
  const std::uintptr_t narrow = mem.Alloc(16, 2);
  mem.Put<std::uint16_t>(narrow, 4 << 6);
  mem.Write(narrow + 2, "Caf\xE9", 4);
  CHECK(ReadString(*reader, narrow, packed) == "Caf\xC3\xA9");
  const std::uintptr_t wide = mem.Alloc(16, 2);
  mem.Put<std::uint16_t>(wide, (2 << 6) | 1);
  mem.Put<std::uint16_t>(wide + 2, 0x041E);  // О
  mem.Put<std::uint16_t>(wide + 4, 0x043A);  // к
  CHECK(ReadString(*reader, wide, packed) == "\xD0\x9E\xD0\xBA");

  // Защита от мусора.
  StringFormat short_limit;
  short_limit.max_chars = 4;
  CHECK(ReadString(*reader, mem.Utf8("abcd"), short_limit) == std::string("abcd"));
  CHECK(!ReadString(*reader, mem.Utf8("abcde"), short_limit));
  mem.Put<std::uint16_t>(prefixed, 5000);
  CHECK(!ReadString(*reader, prefixed, with_length));  // длина > max_chars
  CHECK(!ReadString(*reader, 0, utf8));
  CHECK(!ReadString(*reader, mem.end() + 100, utf8));

  // Строка в самом конце снимка: с нулём — читается, без нуля — nullopt.
  FakeMemory tail(32);
  const std::uintptr_t t = tail.Alloc(32, 1);
  tail.Write(t, "0123456789abcdefghijklmnopqrstu", 32);  // 31 символ + '\0' последним байтом
  CHECK(ReadString(*tail.Reader(), t + 20, utf8) == std::string("klmnopqrstu"));
  tail.Put<char>(t + 31, 'v');
  CHECK(!ReadString(*tail.Reader(), t + 20, utf8));
}

// --- Массивы ---------------------------------------------------------------------------

void TestArrayTable() {
  for (const std::size_t width : {std::size_t{8}, std::size_t{4}}) {
    FakeMemory mem;
    const std::uintptr_t array = mem.Alloc(4 * width);
    mem.PutPointer(array + 0 * width, mem.Utf8("None"), width);
    mem.PutPointer(array + 1 * width, mem.Utf8("Transform"), width);
    // array[2] = nullptr — пустой слот
    mem.PutPointer(array + 3 * width, mem.Utf8("\xD0\x9A\xD0\xB0\xD0\xBC\xD0\xB5\xD1\x80\xD0\xB0"), width);  // "Камера"

    ArrayTableSpec spec;
    spec.array = array;
    spec.pointer_size = width;
    spec.count = 4;
    const ArrayNameTable table(mem.Reader(), spec);
    CHECK(table.Lookup({0, 0}) == std::string("None"));
    CHECK(table.Lookup({1, 1}) == std::string("Transform"));
    CHECK(!table.Lookup({2, 2}));
    CHECK(table.Lookup({3, 3}) == std::string("\xD0\x9A\xD0\xB0\xD0\xBC\xD0\xB5\xD1\x80\xD0\xB0"));
    CHECK(!table.Lookup({4, 4}));
    CHECK(!table.Lookup({~0ull, 0}));

    std::map<std::uint64_t, std::string> all;
    table.ForEach([&](std::uint64_t key, const std::string& name) { return all.emplace(key, name).second; });
    CHECK(all.size() == 3 && all.at(1) == "Transform" && all.count(2) == 0);
  }
}

void TestArrayTableLiveLayouts() {
  FakeMemory mem;
  const auto reader = mem.Reader();

  // Массив, указатель на который лежит в глобальной структуре {T* data; uint32 count},
  // а сам массив перевыделяется по мере роста.
  const std::uintptr_t registry = mem.Alloc(16);
  const std::uintptr_t old_array = mem.Alloc(16);
  mem.PutPointer(old_array, mem.Utf8("first"), 8);
  mem.PutPointer(registry, old_array, 8);
  mem.Put<std::uint32_t>(registry + 8, 1);

  ArrayTableSpec spec;
  spec.array = PointerPath(registry, {0});
  spec.pointer_size = 8;
  spec.count_field = PointerPath(registry + 8);
  const ArrayNameTable table(reader, spec);
  CHECK(table.Count() == 1);
  CHECK(table.Lookup({0, 0}) == std::string("first"));
  CHECK(!table.Lookup({1, 1}));

  const std::uintptr_t new_array = mem.Alloc(32);
  mem.PutPointer(new_array, mem.Utf8("first"), 8);
  mem.PutPointer(new_array + 8, mem.Utf8("second"), 8);
  mem.PutPointer(registry, new_array, 8);  // "перевыделение"
  mem.Put<std::uint32_t>(registry + 8, 2);
  CHECK(table.Count() == 2);
  CHECK(table.Lookup({1, 1}) == std::string("second"));

  // Поблочный массив: блоки по 2 элемента, третьего блока нет.
  const std::uintptr_t chunks = mem.Alloc(3 * 8);
  for (int c = 0; c < 2; ++c) {
    const std::uintptr_t chunk = mem.Alloc(2 * 8);
    mem.PutPointer(chunks + c * 8, chunk, 8);
    for (int i = 0; i < 2; ++i) {
      mem.PutPointer(chunk + i * 8, mem.Utf8("chunk" + std::to_string(c) + "_" + std::to_string(i)), 8);
    }
  }
  ArrayTableSpec chunked;
  chunked.array = chunks;
  chunked.pointer_size = 8;
  chunked.chunk_capacity = 2;
  chunked.count = 6;
  const ArrayNameTable chunk_table(reader, chunked);
  CHECK(chunk_table.Lookup({0, 0}) == std::string("chunk0_0"));
  CHECK(chunk_table.Lookup({3, 3}) == std::string("chunk1_1"));
  CHECK(!chunk_table.Lookup({4, 4}));  // блок не выделен

  // Массив структур {uint32 id; uint32 flags; const char* name} — указатель внутри элемента.
  const std::uintptr_t records = mem.Alloc(3 * 16);
  for (int i = 0; i < 3; ++i) mem.PutPointer(records + i * 16 + 8, mem.Utf8("rec" + std::to_string(i)), 8);
  ArrayTableSpec structs;
  structs.array = records;
  structs.pointer_size = 8;
  structs.element_size = 16;
  structs.element_offset = 8;
  structs.count = 3;
  CHECK(ArrayNameTable(reader, structs).Lookup({2, 2}) == std::string("rec2"));

  // Записи прямо в массиве: {uint8 length; char chars[15]}; длина массива неизвестна —
  // перебор идёт до конца читаемой памяти.
  FakeMemory small(3 * 16);
  const std::uintptr_t inline_array = small.Alloc(3 * 16);
  const char* words[] = {"one", "two", "three"};
  for (int i = 0; i < 3; ++i) {
    small.Put<std::uint8_t>(inline_array + i * 16, static_cast<std::uint8_t>(std::strlen(words[i])));
    small.Write(inline_array + i * 16 + 1, words[i], std::strlen(words[i]));
  }
  ArrayTableSpec inline_spec;
  inline_spec.array = inline_array;
  inline_spec.element_is_pointer = false;
  inline_spec.element_size = 16;
  inline_spec.string.length_size = 1;
  inline_spec.string.chars_offset = 1;
  const ArrayNameTable inline_table(small.Reader(), inline_spec);
  CHECK(inline_table.Lookup({2, 2}) == std::string("three"));
  CHECK(!inline_table.Lookup({3, 3}));
  std::vector<std::string> listed;
  inline_table.ForEach([&](std::uint64_t, const std::string& name) {
    listed.push_back(name);
    return true;
  });
  CHECK((listed == std::vector<std::string>{"one", "two", "three"}));

  CHECK(Throws([&] {
    ArrayTableSpec bad;
    bad.element_is_pointer = false;
    ArrayNameTable t(reader, bad);
  }));
  CHECK(Throws([&] { ArrayNameTable t(nullptr, ArrayTableSpec{}); }));
}

// --- Хеш-таблицы -----------------------------------------------------------------------

// Узел цепочки: {uint32 key; uint32 pad; node* next; const char* name}.
struct ChainedBuilder {
  FakeMemory& mem;
  std::uintptr_t buckets;
  std::size_t count;

  ChainedBuilder(FakeMemory& m, std::size_t bucket_count) : mem(m), buckets(m.Alloc(bucket_count * 8)), count(bucket_count) {}

  std::uintptr_t Insert(std::uint32_t key, std::size_t bucket, const std::string& name) {
    const std::uintptr_t node = mem.Alloc(24);
    mem.Put<std::uint32_t>(node, key);
    const auto head = mem.Reader()->ReadPointer(buckets + bucket * 8, 8);
    mem.PutPointer(node + 8, head.value_or(0), 8);  // вставка в начало цепочки
    mem.PutPointer(node + 16, mem.Utf8(name), 8);
    mem.PutPointer(buckets + bucket * 8, node, 8);
    return node;
  }

  HashTableSpec Spec() const {
    HashTableSpec spec;
    spec.buckets = buckets;
    spec.pointer_size = 8;
    spec.bucket_count = count;
    spec.key_offset = 0;
    spec.key_size = 4;
    spec.next_offset = 8;
    spec.value_offset = 16;
    return spec;
  }
};

void TestChainedHashTable() {
  FakeMemory mem;
  ChainedBuilder builder(mem, 4);
  builder.Insert(1, 1, "one");
  builder.Insert(5, 1, "five");  // та же корзина: 5 % 4 == 1
  const std::uintptr_t nine = builder.Insert(9, 1, "nine");
  builder.Insert(2, 2, "two");
  const HashNameTable table(mem.Reader(), builder.Spec());
  CHECK(table.Lookup({1, 1}) == std::string("one"));
  CHECK(table.Lookup({5, 5}) == std::string("five"));
  CHECK(table.Lookup({9, 9}) == std::string("nine"));
  CHECK(table.Lookup({2, 2}) == std::string("two"));
  CHECK(!table.Lookup({13, 13}));  // корзина та же, ключа нет
  CHECK(!table.Lookup({3, 3}));    // пустая корзина

  std::map<std::uint64_t, std::string> all;
  table.ForEach([&](std::uint64_t key, const std::string& name) { return all.emplace(key, name).second; });
  CHECK(all.size() == 4 && all.at(9) == "nine");

  // Маска вместо остатка: 4 — степень двойки, результат тот же; 3 — нет, это ошибка конфигурации.
  HashTableSpec masked = builder.Spec();
  masked.bucket_index = BucketIndex::Mask;
  CHECK(HashNameTable(mem.Reader(), masked).Lookup({5, 5}) == std::string("five"));
  masked.bucket_count = 3;
  CHECK(!HashNameTable(mem.Reader(), masked).Lookup({5, 5}));

  // Зацикленная цепочка (повреждённая память) не вешает поиск.
  mem.PutPointer(nine + 8, nine, 8);
  CHECK(!table.Lookup({13, 13}));
  CHECK(table.Lookup({9, 9}) == std::string("nine"));
}

void TestHashKeyedByNameHash() {
  // ID объекта — FNV-1a хеш его имени; таблица хранит хеш в узле и
  // выбирает корзину по нему же.
  FakeMemory mem;
  ChainedBuilder builder(mem, 8);
  const StringHashOptions hashing;  // FNV-1a 32, UTF-8
  const std::vector<std::string> names = {"Position", "Velocity", "Health", "\xD0\x98\xD0\xBC\xD1\x8F"};  // "Имя"
  for (const std::string& name : names) {
    const auto h = static_cast<std::uint32_t>(HashString(name, hashing));
    builder.Insert(h, h % 8, name);
  }
  const GenericDataResolver resolver(mem.Reader(), builder.Spec());  // KeyConverter::Direct()
  for (const std::string& name : names) CHECK(resolver.Resolve(HashString(name, hashing)) == name);
  CHECK(!resolver.Resolve(HashString("Missing", hashing)));

  // Таблица хеширует числовой ID (FNV-1a над 4 байтами) — KeyConverter::HashedId.
  FakeMemory mem2;
  ChainedBuilder hashed(mem2, 16);
  for (std::uint32_t id : {10u, 20u, 30u, 1000000u}) {
    const auto bucket = HashBytes(HashAlgorithm::Fnv1a32, &id, 4) % 16;
    hashed.Insert(id, static_cast<std::size_t>(bucket), "object" + std::to_string(id));
  }
  const GenericDataResolver by_id(mem2.Reader(), hashed.Spec(), KeyConverter::HashedId(HashAlgorithm::Fnv1a32, 4));
  CHECK(by_id.Resolve(20) == std::string("object20"));
  CHECK(by_id.Resolve(1000000) == std::string("object1000000"));
  CHECK(!by_id.Resolve(40));
  // С прямым конвертером корзина была бы другой.
  const GenericDataResolver wrong(mem2.Reader(), hashed.Spec());
  CHECK(!wrong.Resolve(1000000));
}

void TestLinearProbing() {
  // Слоты {uint32 key; uint32 pad; const char* name}, 8 слотов, 0 — пусто,
  // 0xFFFFFFFF — удалено.
  FakeMemory mem;
  const std::uintptr_t slots = mem.Alloc(8 * 16);
  auto put = [&](std::size_t slot, std::uint32_t key, const char* name) {
    mem.Put<std::uint32_t>(slots + slot * 16, key);
    mem.PutPointer(slots + slot * 16 + 8, name ? mem.Utf8(name) : 0, 8);
  };
  put(7, 7, "seven");     // корзина 7
  put(0, 15, "fifteen");  // корзина 7, перенос в начало массива
  put(1, 0xFFFFFFFF, nullptr);
  put(2, 23, "twenty-three");  // корзина 7, после "надгробия"
  put(3, 3, "three");          // своя корзина 3 занята? нет: 3 % 8 == 3 — первый же слот

  HashTableSpec spec;
  spec.kind = HashTableKind::LinearProbing;
  spec.buckets = slots;
  spec.pointer_size = 8;
  spec.bucket_count = 8;
  spec.slot_size = 16;
  spec.key_size = 4;
  spec.value_offset = 8;
  spec.empty_key = 0;
  spec.deleted_key = 0xFFFFFFFF;
  const HashNameTable table(mem.Reader(), spec);
  CHECK(table.Lookup({7, 7}) == std::string("seven"));
  CHECK(table.Lookup({15, 15}) == std::string("fifteen"));
  CHECK(table.Lookup({23, 23}) == std::string("twenty-three"));
  CHECK(table.Lookup({3, 3}) == std::string("three"));
  CHECK(!table.Lookup({31, 31}));  // доходит до пустого слота 4
  CHECK(!table.Lookup({0xFFFFFFFF, 7}));

  std::vector<std::string> listed;
  table.ForEach([&](std::uint64_t, const std::string& name) {
    listed.push_back(name);
    return true;
  });
  CHECK(listed.size() == 4);

  // Заполненная таблица без пустых слотов: поиск отсутствующего ключа завершается.
  for (std::size_t i = 4; i < 7; ++i) put(i, static_cast<std::uint32_t>(100 + i), "filler");
  put(1, 101, "filler");
  CHECK(!table.Lookup({31, 31}));

  spec.slot_size = 0;
  CHECK(Throws([&] { HashNameTable t(mem.Reader(), spec); }));
}

// --- Словарь, конвертеры, resolver ----------------------------------------------------------

void TestDictionary() {
  DictionaryNameTable dict;
  CHECK(dict.Add("Position") && dict.Add("Velocity") && dict.Add("Position"));  // повтор того же имени — не коллизия
  CHECK(dict.size() == 2);
  CHECK(dict.Lookup({dict.KeyOf("Velocity"), 0}) == std::string("Velocity"));
  CHECK(!dict.Add(dict.KeyOf("Position"), "Impostor"));
  CHECK(dict.collisions().size() == 1);
  CHECK(dict.collisions()[0].kept == "Position" && dict.collisions()[0].rejected == "Impostor");
  CHECK(dict.Add(42, "bad\xFFutf8") && dict.Lookup({42, 0}) == "bad" + kFffd + "utf8");

  StringHashOptions crc_lower;
  crc_lower.algorithm = HashAlgorithm::Crc32;
  crc_lower.ascii_lowercase = true;
  DictionarySpec spec;
  spec.hashing = crc_lower;
  spec.names = {"PlayerController", "GameMode"};
  spec.entries = {{7, "Seven"}};
  const GenericDataResolver resolver(nullptr, spec);
  CHECK(resolver.Resolve(HashString("playercontroller", crc_lower)) == std::string("PlayerController"));
  CHECK(resolver.Resolve(7) == std::string("Seven"));
  CHECK(!resolver.Resolve(8));
}

void TestKeyConverters() {
  CHECK(KeyConverter()(5)->value == 5);
  const KeyConverter low32 = KeyConverter::BitField(0, 0xFFFFFFFF);
  CHECK(low32(0x0000000700000010ull)->value == 0x10);
  const KeyConverter flagged = KeyConverter::BitField(1, ~0ull);
  CHECK(flagged(11)->value == 5 && flagged(11)->hash == 5);
  const KeyConverter one_based = KeyConverter::BitField(0, ~0ull, -1);
  CHECK(one_based(1)->value == 0);
  CHECK(one_based(0)->value == ~0ull);  // "нет имени" -> заведомо за пределами массива
  CHECK(KeyConverter::BitField(64, ~0ull)(123)->value == 0);

  const std::uint32_t id = 0xABCD;
  const auto hashed = KeyConverter::HashedId(HashAlgorithm::Crc32, 4)(id);
  CHECK(hashed->value == id && hashed->hash == HashBytes(HashAlgorithm::Crc32, &id, 4));

  const KeyConverter chain = KeyConverter::BitField(8, 0xFF).Then(KeyConverter::HashedId(HashAlgorithm::Fnv1a32, 1));
  const unsigned char byte = 0xAB;
  CHECK(chain(0xAB00)->value == 0xAB && chain(0xAB00)->hash == HashBytes(HashAlgorithm::Fnv1a32, &byte, 1));

  const KeyConverter even_only = KeyConverter::Custom([](std::uint64_t v) -> std::optional<LookupKey> {
    if (v % 2) return std::nullopt;
    return LookupKey{v / 2, v / 2};
  });
  CHECK(!even_only(3) && even_only(4)->value == 2);
  CHECK(!KeyConverter::BitField(0, ~0ull).Then(even_only)(3));

  CHECK(Throws([] { KeyConverter::HashedId(HashAlgorithm::Crc32, 9); }));
  CHECK(Throws([] { KeyConverter::Custom(nullptr); }));
}

void TestResolverCacheStatsAndLabels() {
  FakeMemory mem;
  const std::uintptr_t array = mem.Alloc(3 * 8);
  const std::uintptr_t mutable_name = mem.Utf8("old");
  mem.PutPointer(array, mutable_name, 8);
  mem.PutPointer(array + 8, mem.Utf8("stable"), 8);
  ArrayTableSpec spec;
  spec.array = array;
  spec.pointer_size = 8;
  spec.count = 3;

  // ID упакован: индекс в битах 4.., младшие 4 бита — флаги.
  GenericDataResolver resolver(mem.Reader(), spec, KeyConverter::BitField(4, ~0ull));
  CHECK(resolver.Resolve(0x01) == std::string("old"));  // индекс 0, флаг 1
  mem.Write(mutable_name, "new", 3);
  CHECK(resolver.Resolve(0x01) == std::string("old"));  // из кеша
  CHECK(resolver.Resolve(0x02) == std::string("new"));  // другой ID — читается заново
  resolver.ClearCache();
  CHECK(resolver.Resolve(0x01) == std::string("new"));

  // Промахи не кешируются: имя, появившееся позже, находится.
  CHECK(!resolver.Resolve(0x20));
  mem.PutPointer(array + 16, mem.Utf8("late"), 8);
  CHECK(resolver.Resolve(0x20) == std::string("late"));

  CHECK(resolver.Label(0x10) == "stable");
  CHECK(resolver.Label(0x300) == "<unknown:0x300>");
  CHECK((resolver.Labels({0x10, 0x999}) == std::vector<std::string>{"stable", "<unknown:0x999>"}));

  const ResolverStats stats = resolver.stats();
  CHECK(stats.lookups == stats.cache_hits + stats.resolved + stats.unresolved);
  CHECK(stats.cache_hits == 2 && stats.unresolved == 3 && stats.rejected == 0);

  // Свои настройки: кешировать промахи, своя подпись, маленький кеш.
  ResolverOptions options;
  options.cache_misses = true;
  options.max_cache_entries = 2;
  options.fallback_label = [](std::uint64_t id) { return "obj#" + std::to_string(id); };
  GenericDataResolver strict(mem.Reader(), spec, KeyConverter::Direct(), options);
  mem.PutPointer(array + 16, 0, 8);
  CHECK(!strict.Resolve(2));
  mem.PutPointer(array + 16, mem.Utf8("late"), 8);
  CHECK(!strict.Resolve(2));  // промах закеширован
  CHECK(strict.Label(7) == "obj#7");
  CHECK(strict.Resolve(1) == std::string("stable"));  // кеш переполнился и очистился
  CHECK(strict.Resolve(2) == std::string("late"));

  // Конвертер отклоняет ID — это промах с отметкой rejected.
  const GenericDataResolver picky(
      mem.Reader(), spec, KeyConverter::Custom([](std::uint64_t) -> std::optional<LookupKey> { return std::nullopt; }));
  CHECK(!picky.Resolve(1) && picky.stats().rejected == 1);

  // Resolver перемещаем (например, в контейнер).
  std::vector<GenericDataResolver> resolvers;
  resolvers.push_back(std::move(resolver));
  CHECK(resolvers[0].Resolve(0x10) == std::string("stable"));
}

void TestResolverConcurrency() {
  FakeMemory mem(1 << 20);
  constexpr int kNames = 500;
  const std::uintptr_t array = mem.Alloc(kNames * 8);
  for (int i = 0; i < kNames; ++i) mem.PutPointer(array + i * 8, mem.Utf8("name_" + std::to_string(i)), 8);
  ArrayTableSpec spec;
  spec.array = array;
  spec.pointer_size = 8;
  spec.count = kNames;
  ResolverOptions options;
  options.max_cache_entries = 200;  // заодно проверяется очистка кеша под нагрузкой
  const GenericDataResolver resolver(mem.Reader(), spec, KeyConverter::Direct(), options);

  std::atomic<bool> ok{true};
  std::vector<std::thread> threads;
  for (int t = 0; t < 4; ++t) {
    threads.emplace_back([&, t] {
      for (int round = 0; round < 20; ++round) {
        for (int i = 0; i < kNames + 10; ++i) {
          const int id = (i * 7 + t * 13) % (kNames + 10);
          const auto name = resolver.Resolve(static_cast<std::uint64_t>(id));
          const bool expected_found = id < kNames;
          if (name.has_value() != expected_found || (name && *name != "name_" + std::to_string(id))) ok = false;
        }
      }
    });
  }
  for (auto& thread : threads) thread.join();
  CHECK(ok);
  const ResolverStats stats = resolver.stats();
  CHECK(stats.lookups == 4u * 20 * (kNames + 10));
  CHECK(stats.lookups == stats.cache_hits + stats.resolved + stats.unresolved);
}

void TestFallbackAndExport() {
  FakeMemory mem;
  const std::uintptr_t array = mem.Alloc(2 * 8);
  mem.PutPointer(array, mem.Utf8("live0"), 8);
  mem.PutPointer(array + 8, mem.Utf8("live1"), 8);
  ArrayTableSpec spec;
  spec.array = array;
  spec.pointer_size = 8;
  spec.count = 2;
  const auto live = MakeNameTable(mem.Reader(), spec);

  auto known = std::make_shared<DictionaryNameTable>();
  known->Add(1, "shadowed");  // ключ 1 уже есть в живой таблице
  known->Add(5, "known5");
  const GenericDataResolver resolver(std::make_shared<FallbackNameTable>(
      std::vector<std::shared_ptr<const NameTable>>{live, known}));
  CHECK(resolver.Resolve(1) == std::string("live1"));
  CHECK(resolver.Resolve(5) == std::string("known5"));
  std::map<std::uint64_t, std::string> all;
  resolver.ForEachName([&](std::uint64_t key, const std::string& name) { return all.emplace(key, name).second; });
  CHECK(all.size() == 3 && all.at(1) == "live1");

  // Снимок живой таблицы переживает изменение (или исчезновение) памяти.
  const GenericDataResolver live_resolver(live);
  const GenericDataResolver frozen(live_resolver.ExportDictionary());
  mem.PutPointer(array, 0, 8);
  CHECK(!live_resolver.Resolve(0));
  CHECK(frozen.Resolve(0) == std::string("live0"));
  CHECK(frozen.Resolve(1) == std::string("live1"));

  CHECK(Throws([] { GenericDataResolver r(std::shared_ptr<const NameTable>{}); }));
  CHECK(Throws([] { FallbackNameTable t({nullptr}); }));
}

}  // namespace

int main() {
  TestUtf8();
  TestUtf16AndLatin1();
  TestHashes();
  TestRegionReaderAndPointerPath();
#if defined(MEMREFL_HAS_PROCESS_READER)
  TestProcessReader();
#endif
  TestReadString();
  TestArrayTable();
  TestArrayTableLiveLayouts();
  TestChainedHashTable();
  TestHashKeyedByNameHash();
  TestLinearProbing();
  TestDictionary();
  TestKeyConverters();
  TestResolverCacheStatsAndLabels();
  TestResolverConcurrency();
  TestFallbackAndExport();
  std::puts("all data resolver tests passed");
  return 0;
}

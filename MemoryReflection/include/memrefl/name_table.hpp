// memrefl — модуль 2: таблицы имён.
//
// Таблица имён отвечает на вопрос "какая строка соответствует ключу". Ключ
// (LookupKey) получается из числового ID через KeyConverter (data_resolver.hpp).
// Реализации:
//
//   ArrayNameTable      массив в памяти цели: ключ = индекс. Плоский или
//                       поблочный (массив указателей на блоки), элементы —
//                       указатели на записи или сами записи.
//   HashNameTable       хеш-таблица в памяти цели: цепочки (bucket -> node ->
//                       next) или открытая адресация (линейное пробирование).
//   DictionaryNameTable локальный словарь: ключ = хеш известного имени. Для
//                       случаев, когда ID — хеш строки, а самой таблицы в
//                       памяти нет (или она недоступна).
//   FallbackNameTable   цепочка таблиц: первая нашедшая имя побеждает.
//
// Таблица в памяти находит адрес "записи" строки, а StringFormat описывает,
// как прочитать строку из записи: кодировка, где символы (в записи или по
// указателю), как задана длина (поле длины или завершающий ноль). Для
// нестандартных форматов можно передать свой StringDecoder.
//
// Все чтения идут через MemoryReader и проверяются, поэтому мусорный ID или
// повреждённая таблица дают nullopt, а не падение. Число элементов, длина
// цепочек и длина строк ограничены сверху на случай мусорных данных и циклов.

#pragma once

#include "memrefl/memory_reader.hpp"
#include "memrefl/text.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace memrefl {

// --- Ключ ---------------------------------------------------------------------

struct LookupKey {
  std::uint64_t value = 0;  // индекс в массиве; ключ для сравнения в хеш-таблице и словаре
  std::uint64_t hash = 0;   // по нему хеш-таблица выбирает корзину
};

// --- Строки в памяти ---------------------------------------------------------------

struct StringFormat {
  Encoding encoding = Encoding::Utf8;

  // Где лежат символы: прямо в записи по смещению chars_offset или по
  // указателю, который лежит в записи по смещению chars_offset.
  std::size_t chars_offset = 0;
  bool chars_by_pointer = false;
  std::size_t pointer_size = sizeof(void*);

  // Длина в символах. length_size == 0 — строка до завершающего нуля;
  // иначе — поле длины length_size байт по смещению length_offset. Если длина
  // упакована вместе с флагами: length = (raw >> length_shift) & length_mask.
  // Завершающие нули внутри длины отбрасываются (длина "с учётом \0").
  std::size_t length_offset = 0;
  std::size_t length_size = 0;
  unsigned length_shift = 0;
  std::uint64_t length_mask = ~std::uint64_t{0};

  // Если в поле длины установлен этот бит — строка в UTF-16LE, иначе в
  // encoding. Для таблиц, где узкие и широкие строки хранятся вперемешку.
  std::uint64_t utf16_flag_mask = 0;

  // Строка длиннее — считается мусором (nullopt).
  std::size_t max_chars = 1024;
};

// Свой способ прочитать строку из записи по адресу record.
using StringDecoder = std::function<std::optional<std::string>(const MemoryReader&, std::uintptr_t record)>;

namespace detail {

// Читает единицы кодирования до нулевой. Блоками, не пересекающими границу
// 4 КБ-страницы: страница читается либо целиком, либо никак, и строка в
// конце последней доступной страницы читается без ошибки. Если блок не
// прочитался (например, конец снимка не выровнен по странице), дочитывает по
// одной единице.
inline bool ReadTerminated(const MemoryReader& reader, std::uintptr_t address, std::size_t unit,
                           std::size_t max_chars, std::string& out) {
  constexpr std::size_t kPage = 4096;
  constexpr std::size_t kChunk = 256;
  const std::size_t max_bytes = max_chars * unit;
  unsigned char buffer[kChunk];
  std::uintptr_t cursor = address;
  for (;;) {
    std::size_t want = (std::min)(kChunk, kPage - static_cast<std::size_t>(cursor % kPage));
    want -= want % unit;
    if (want == 0 || !reader.ReadBytes(cursor, buffer, want)) {
      want = unit;
      if (!reader.ReadBytes(cursor, buffer, unit)) return false;
    }
    for (std::size_t i = 0; i < want; i += unit) {
      if (std::all_of(buffer + i, buffer + i + unit, [](unsigned char b) { return b == 0; })) return true;
      out.append(reinterpret_cast<const char*>(buffer + i), unit);
      if (out.size() > max_bytes) return false;
    }
    cursor += want;
  }
}

}  // namespace detail

// Читает строку из записи по адресу record и возвращает её в UTF-8.
inline std::optional<std::string> ReadString(const MemoryReader& reader, std::uintptr_t record,
                                             const StringFormat& format) {
  if (record == 0) return std::nullopt;
  Encoding encoding = format.encoding;
  std::optional<std::size_t> length;
  if (format.length_size != 0) {
    const std::optional<std::uint64_t> raw = reader.ReadUnsigned(record + format.length_offset, format.length_size);
    if (!raw) return std::nullopt;
    if (format.utf16_flag_mask != 0 && (*raw & format.utf16_flag_mask) != 0) encoding = Encoding::Utf16LE;
    const std::uint64_t chars = format.length_shift >= 64 ? 0 : (*raw >> format.length_shift) & format.length_mask;
    if (chars > format.max_chars) return std::nullopt;
    length = static_cast<std::size_t>(chars);
  }

  std::uintptr_t chars_address = record + format.chars_offset;
  if (format.chars_by_pointer) {
    const std::optional<std::uintptr_t> pointer = reader.ReadPointer(chars_address, format.pointer_size);
    if (!pointer) return std::nullopt;
    // Пустые строки часто хранятся как {nullptr, 0}.
    if (*pointer == 0) return length == std::size_t{0} ? std::optional<std::string>("") : std::nullopt;
    chars_address = *pointer;
  }

  const std::size_t unit = CodeUnitSize(encoding);
  std::string bytes;
  if (length) {
    bytes.resize(*length * unit);
    if (!reader.ReadBytes(chars_address, bytes.data(), bytes.size())) return std::nullopt;
    while (bytes.size() >= unit && std::all_of(bytes.end() - static_cast<std::ptrdiff_t>(unit), bytes.end(),
                                               [](char b) { return b == '\0'; })) {
      bytes.resize(bytes.size() - unit);
    }
  } else if (!detail::ReadTerminated(reader, chars_address, unit, format.max_chars, bytes)) {
    return std::nullopt;
  }
  return ToUtf8(bytes.data(), bytes.size(), encoding);
}

// --- Интерфейс таблицы -------------------------------------------------------------

// callback(key, name) для каждой записи; false прекращает перебор.
using NameVisitor = std::function<bool(std::uint64_t key, const std::string& name)>;

class NameTable {
 public:
  virtual ~NameTable() = default;

  // Имя по ключу; nullopt — записи нет или её не удалось прочитать.
  virtual std::optional<std::string> Lookup(const LookupKey& key) const = 0;

  // Перебор всех записей (выгрузка словаря целиком). Порядок зависит от таблицы.
  // По умолчанию таблица перебор не поддерживает.
  virtual void ForEach(const NameVisitor& visitor) const { (void)visitor; }
};

namespace detail {

inline StringDecoder MakeDecoder(StringDecoder custom, const StringFormat& format) {
  if (custom) return custom;
  return [format](const MemoryReader& reader, std::uintptr_t record) { return ReadString(reader, record, format); };
}

inline std::uint64_t KeyBits(std::uint64_t value, std::size_t size) noexcept {
  return size >= 8 ? value : value & ((std::uint64_t{1} << (8 * size)) - 1);
}

}  // namespace detail

// --- Массив ----------------------------------------------------------------------

struct ArrayTableSpec {
  // Начало массива элементов, а для поблочного массива — начало массива
  // указателей на блоки. PointerPath перечитывается при каждом обращении.
  PointerPath array;
  std::size_t pointer_size = sizeof(void*);  // ширина указателей цели

  // Элемент: указатель на запись строки (element_is_pointer) или сама запись.
  // element_size — шаг элементов (0 = pointer_size); element_offset — где
  // внутри элемента лежит указатель/запись (например, массив {id, name*}).
  bool element_is_pointer = true;
  std::size_t element_size = 0;
  std::size_t element_offset = 0;

  // > 0: поблочный массив — блок i / chunk_capacity, элемент i % chunk_capacity.
  // Так хранят таблицы, которые растут без перевыделения.
  std::size_t chunk_capacity = 0;

  // Число элементов: известное заранее (count) или из поля в памяти цели
  // (count_field, перечитывается при каждом обращении). Если не задано ни то,
  // ни другое, граница — max_count, а перебор идёт до первого нечитаемого
  // элемента.
  std::size_t count = 0;
  std::optional<PointerPath> count_field;
  std::size_t count_size = 4;
  std::size_t max_count = std::size_t{1} << 24;

  StringFormat string;
  StringDecoder decoder;  // если задан — используется вместо string
};

class ArrayNameTable final : public NameTable {
 public:
  ArrayNameTable(std::shared_ptr<const MemoryReader> reader, ArrayTableSpec spec)
      : reader_(std::move(reader)), spec_(std::move(spec)), decode_(detail::MakeDecoder(spec_.decoder, spec_.string)) {
    if (!reader_) throw std::invalid_argument("ArrayNameTable: reader is null");
    if (spec_.element_size == 0) {
      if (!spec_.element_is_pointer) throw std::invalid_argument("ArrayNameTable: element_size is required for inline records");
      spec_.element_size = spec_.pointer_size;
    }
  }

  std::optional<std::string> Lookup(const LookupKey& key) const override {
    if (key.value >= Count()) return std::nullopt;
    const std::optional<std::uintptr_t> element = ElementAddress(key.value);
    if (!element) return std::nullopt;
    const std::optional<std::uintptr_t> record = RecordAddress(*element);
    if (!record) return std::nullopt;
    return decode_(*reader_, *record);
  }

  void ForEach(const NameVisitor& visitor) const override {
    const bool count_known = spec_.count != 0 || spec_.count_field.has_value();
    const std::size_t count = Count();
    for (std::size_t i = 0; i < count; ++i) {
      const std::optional<std::uintptr_t> element = ElementAddress(i);
      std::uint8_t probe;
      const bool readable = element && reader_->ReadBytes(*element, &probe, 1);
      if (!readable) {
        if (count_known) continue;
        return;  // конец массива неизвестной длины
      }
      const std::optional<std::uintptr_t> record = RecordAddress(*element);
      if (!record) continue;  // пустой слот
      if (const std::optional<std::string> name = decode_(*reader_, *record)) {
        if (!visitor(i, *name)) return;
      }
    }
  }

  // Текущая граница индексов.
  std::size_t Count() const {
    if (spec_.count != 0) return (std::min)(spec_.count, spec_.max_count);
    if (spec_.count_field) {
      const std::optional<std::uintptr_t> field = spec_.count_field->Resolve(*reader_, spec_.pointer_size);
      const std::optional<std::uint64_t> value = field ? reader_->ReadUnsigned(*field, spec_.count_size) : std::nullopt;
      if (!value) return 0;
      return static_cast<std::size_t>(std::min<std::uint64_t>(*value, spec_.max_count));
    }
    return spec_.max_count;
  }

 private:
  std::optional<std::uintptr_t> ElementAddress(std::uint64_t index) const {
    const std::optional<std::uintptr_t> array = spec_.array.Resolve(*reader_, spec_.pointer_size);
    if (!array) return std::nullopt;
    if (spec_.chunk_capacity == 0) return *array + static_cast<std::uintptr_t>(index) * spec_.element_size;
    const std::uint64_t chunk_index = index / spec_.chunk_capacity;
    const std::optional<std::uintptr_t> chunk =
        reader_->ReadPointer(*array + static_cast<std::uintptr_t>(chunk_index) * spec_.pointer_size, spec_.pointer_size);
    if (!chunk || *chunk == 0) return std::nullopt;
    return *chunk + static_cast<std::uintptr_t>(index % spec_.chunk_capacity) * spec_.element_size;
  }

  std::optional<std::uintptr_t> RecordAddress(std::uintptr_t element) const {
    if (!spec_.element_is_pointer) return element + spec_.element_offset;
    const std::optional<std::uintptr_t> record = reader_->ReadPointer(element + spec_.element_offset, spec_.pointer_size);
    if (!record || *record == 0) return std::nullopt;
    return record;
  }

  std::shared_ptr<const MemoryReader> reader_;
  ArrayTableSpec spec_;
  StringDecoder decode_;
};

// --- Хеш-таблица -------------------------------------------------------------------

enum class HashTableKind {
  Chained,        // корзина -> узел -> next -> ... (std::unordered_map-подобные)
  LinearProbing,  // массив слотов; коллизия -> следующий слот
};

enum class BucketIndex {
  Modulo,  // hash % bucket_count
  Mask,    // hash & (bucket_count - 1); bucket_count — степень двойки
};

struct HashTableSpec {
  HashTableKind kind = HashTableKind::Chained;

  // Chained: массив указателей на первый узел корзины.
  // LinearProbing: массив слотов по slot_size байт.
  PointerPath buckets;
  std::size_t pointer_size = sizeof(void*);

  // Число корзин/слотов: известное или из поля в памяти (перечитывается).
  std::size_t bucket_count = 0;
  std::optional<PointerPath> bucket_count_field;
  std::size_t bucket_count_size = 4;
  std::size_t max_bucket_count = std::size_t{1} << 26;
  BucketIndex bucket_index = BucketIndex::Modulo;

  // Узел (Chained) или слот (LinearProbing): ключ, который сравнивается с
  // LookupKey::value, и запись строки (указатель на неё или она сама).
  std::size_t key_offset = 0;
  std::size_t key_size = 4;
  std::size_t value_offset = 0;
  bool value_is_pointer = true;

  std::size_t next_offset = 0;              // Chained: указатель на следующий узел
  std::size_t slot_size = 0;                // LinearProbing: размер слота
  std::uint64_t empty_key = 0;              // LinearProbing: ключ пустого слота
  std::optional<std::uint64_t> deleted_key;  // LinearProbing: ключ удалённого слота ("надгробие")

  std::size_t max_chain = 4096;  // защита от зацикленных цепочек и бесконечного пробирования

  StringFormat string;
  StringDecoder decoder;
};

class HashNameTable final : public NameTable {
 public:
  HashNameTable(std::shared_ptr<const MemoryReader> reader, HashTableSpec spec)
      : reader_(std::move(reader)), spec_(std::move(spec)), decode_(detail::MakeDecoder(spec_.decoder, spec_.string)) {
    if (!reader_) throw std::invalid_argument("HashNameTable: reader is null");
    if (spec_.kind == HashTableKind::LinearProbing && spec_.slot_size == 0) {
      throw std::invalid_argument("HashNameTable: slot_size is required for linear probing");
    }
  }

  std::optional<std::string> Lookup(const LookupKey& key) const override {
    const std::size_t count = BucketCount();
    const std::optional<std::uintptr_t> buckets = spec_.buckets.Resolve(*reader_, spec_.pointer_size);
    if (count == 0 || !buckets) return std::nullopt;
    if (spec_.bucket_index == BucketIndex::Mask && (count & (count - 1)) != 0) return std::nullopt;
    const std::size_t bucket = static_cast<std::size_t>(
        spec_.bucket_index == BucketIndex::Mask ? key.hash & (count - 1) : key.hash % count);
    const std::uint64_t wanted = detail::KeyBits(key.value, spec_.key_size);

    if (spec_.kind == HashTableKind::Chained) {
      std::optional<std::uintptr_t> node = reader_->ReadPointer(*buckets + bucket * spec_.pointer_size, spec_.pointer_size);
      for (std::size_t step = 0; node && *node != 0 && step < spec_.max_chain; ++step) {
        const std::optional<std::uint64_t> stored = reader_->ReadUnsigned(*node + spec_.key_offset, spec_.key_size);
        if (!stored) return std::nullopt;
        if (*stored == wanted) return Value(*node);
        node = reader_->ReadPointer(*node + spec_.next_offset, spec_.pointer_size);
      }
      return std::nullopt;
    }

    const std::uint64_t empty = detail::KeyBits(spec_.empty_key, spec_.key_size);
    const std::size_t probes = (std::min)(count, spec_.max_chain);
    for (std::size_t probe = 0; probe < probes; ++probe) {
      const std::uintptr_t slot = *buckets + ((bucket + probe) % count) * spec_.slot_size;
      const std::optional<std::uint64_t> stored = reader_->ReadUnsigned(slot + spec_.key_offset, spec_.key_size);
      if (!stored || *stored == empty) return std::nullopt;
      if (*stored == wanted && !IsDeleted(*stored)) return Value(slot);
    }
    return std::nullopt;
  }

  void ForEach(const NameVisitor& visitor) const override {
    const std::size_t count = BucketCount();
    const std::optional<std::uintptr_t> buckets = spec_.buckets.Resolve(*reader_, spec_.pointer_size);
    if (count == 0 || !buckets) return;
    auto visit = [&](std::uintptr_t entry, std::uint64_t key) {
      const std::optional<std::string> name = Value(entry);
      return !name || visitor(key, *name);
    };
    for (std::size_t b = 0; b < count; ++b) {
      if (spec_.kind == HashTableKind::Chained) {
        std::optional<std::uintptr_t> node = reader_->ReadPointer(*buckets + b * spec_.pointer_size, spec_.pointer_size);
        for (std::size_t step = 0; node && *node != 0 && step < spec_.max_chain; ++step) {
          const std::optional<std::uint64_t> key = reader_->ReadUnsigned(*node + spec_.key_offset, spec_.key_size);
          if (!key) break;
          if (!visit(*node, *key)) return;
          node = reader_->ReadPointer(*node + spec_.next_offset, spec_.pointer_size);
        }
      } else {
        const std::uintptr_t slot = *buckets + b * spec_.slot_size;
        const std::optional<std::uint64_t> key = reader_->ReadUnsigned(slot + spec_.key_offset, spec_.key_size);
        if (!key || *key == detail::KeyBits(spec_.empty_key, spec_.key_size) || IsDeleted(*key)) continue;
        if (!visit(slot, *key)) return;
      }
    }
  }

  std::size_t BucketCount() const {
    std::uint64_t count = spec_.bucket_count;
    if (count == 0 && spec_.bucket_count_field) {
      const std::optional<std::uintptr_t> field = spec_.bucket_count_field->Resolve(*reader_, spec_.pointer_size);
      count = field ? reader_->ReadUnsigned(*field, spec_.bucket_count_size).value_or(0) : 0;
    }
    return count <= spec_.max_bucket_count ? static_cast<std::size_t>(count) : 0;  // мусор — не таблица
  }

 private:
  bool IsDeleted(std::uint64_t stored) const {
    return spec_.deleted_key && stored == detail::KeyBits(*spec_.deleted_key, spec_.key_size);
  }

  std::optional<std::string> Value(std::uintptr_t entry) const {
    std::uintptr_t record = entry + spec_.value_offset;
    if (spec_.value_is_pointer) {
      const std::optional<std::uintptr_t> pointer = reader_->ReadPointer(record, spec_.pointer_size);
      if (!pointer || *pointer == 0) return std::nullopt;
      record = *pointer;
    }
    return decode_(*reader_, record);
  }

  std::shared_ptr<const MemoryReader> reader_;
  HashTableSpec spec_;
  StringDecoder decode_;
};

// --- Словарь известных имён -----------------------------------------------------------

class DictionaryNameTable final : public NameTable {
 public:
  struct Collision {
    std::uint64_t key;
    std::string kept;      // имя, которое осталось в словаре (добавлено первым)
    std::string rejected;  // имя с тем же ключом, которое не добавлено
  };

  explicit DictionaryNameTable(StringHashOptions hashing = {}) : hashing_(hashing) {}

  // Добавляет имя под ключом = его хешем. false — ключ уже занят другим именем
  // (коллизия записывается в collisions(), в словаре остаётся первое имя).
  bool Add(std::string_view name) { return Add(HashString(name, hashing_), name); }

  // Явная пара "ключ -> имя" (например, из выгрузки или конфига).
  bool Add(std::uint64_t key, std::string_view name) {
    std::string utf8 = SanitizeUtf8(name);
    const auto [it, inserted] = names_.emplace(key, utf8);
    if (inserted || it->second == utf8) return true;
    collisions_.push_back({key, it->second, std::move(utf8)});
    return false;
  }

  std::uint64_t KeyOf(std::string_view name) const { return HashString(name, hashing_); }
  std::size_t size() const noexcept { return names_.size(); }
  const std::vector<Collision>& collisions() const noexcept { return collisions_; }

  std::optional<std::string> Lookup(const LookupKey& key) const override {
    const auto it = names_.find(key.value);
    if (it == names_.end()) return std::nullopt;
    return it->second;
  }

  void ForEach(const NameVisitor& visitor) const override {
    for (const auto& [key, name] : names_) {
      if (!visitor(key, name)) return;
    }
  }

 private:
  StringHashOptions hashing_;
  std::unordered_map<std::uint64_t, std::string> names_;
  std::vector<Collision> collisions_;
};

// --- Цепочка таблиц ----------------------------------------------------------------

// Например: сначала живая таблица процесса, затем словарь известных имён.
class FallbackNameTable final : public NameTable {
 public:
  explicit FallbackNameTable(std::vector<std::shared_ptr<const NameTable>> tables) : tables_(std::move(tables)) {
    for (const auto& table : tables_) {
      if (!table) throw std::invalid_argument("FallbackNameTable: table is null");
    }
  }

  std::optional<std::string> Lookup(const LookupKey& key) const override {
    for (const auto& table : tables_) {
      if (std::optional<std::string> name = table->Lookup(key)) return name;
    }
    return std::nullopt;
  }

  // Ключ, уже выданный более приоритетной таблицей, повторно не выдаётся.
  void ForEach(const NameVisitor& visitor) const override {
    std::unordered_set<std::uint64_t> seen;
    bool stopped = false;
    for (const auto& table : tables_) {
      table->ForEach([&](std::uint64_t key, const std::string& name) {
        if (!seen.insert(key).second) return true;
        stopped = !visitor(key, name);
        return !stopped;
      });
      if (stopped) return;
    }
  }

 private:
  std::vector<std::shared_ptr<const NameTable>> tables_;
};

}  // namespace memrefl

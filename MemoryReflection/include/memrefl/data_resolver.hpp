// memrefl — модуль 2: Generic Data Resolver (ID объекта -> читаемое имя).
//
//   ID ──KeyConverter──▶ LookupKey{value, hash} ──NameTable──▶ запись ──StringFormat──▶ UTF-8
//
// * KeyConverter переводит "сырой" ID из памяти в ключ таблицы: прямой индекс,
//   битовое поле (индекс упакован с флагами), хеш числового ID для выбора
//   корзины или любая своя функция. Конвертеры комбинируются через Then().
// * Тип маппинга выбирается спецификацией MappingSpec:
//     ArrayTableSpec  — массив (плоский или поблочный) в памяти цели;
//     HashTableSpec   — хеш-таблица в памяти цели (цепочки / открытая адресация);
//     DictionarySpec  — локальный словарь известных имён (ID = хеш имени).
//   Либо можно передать готовую NameTable (свою реализацию, FallbackNameTable...).
// * GenericDataResolver объединяет конвертер и таблицу, кеширует результаты,
//   ведёт статистику и выдаёт подписи для неизвестных ID.
//
// Resolve() можно вызывать из нескольких потоков одновременно.

#pragma once

#include "memrefl/memory_reader.hpp"
#include "memrefl/name_table.hpp"
#include "memrefl/text.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace memrefl {

// --- Конвертер ID -> ключ ---------------------------------------------------------

class KeyConverter {
 public:
  // nullopt — ID заведомо не имеет имени (например, неверные флаги).
  using Function = std::function<std::optional<LookupKey>(std::uint64_t id)>;

  // По умолчанию — Direct().
  KeyConverter() : KeyConverter(Direct()) {}

  // ID — это и есть ключ: индекс массива или хеш имени. value = hash = id.
  static KeyConverter Direct() {
    return KeyConverter([](std::uint64_t id) { return std::optional<LookupKey>(LookupKey{id, id}); });
  }

  // Битовое поле: value = hash = ((id >> shift) & mask) + bias.
  //   BitField(0, 0xFFFFFFFF)   — индекс в младших 32 битах, выше — флаги/номер;
  //   BitField(1, ~0ull)        — младший бит — флаг;
  //   BitField(0, ~0ull, -1)    — нумерация с единицы (0 означает "нет имени").
  static KeyConverter BitField(unsigned shift, std::uint64_t mask, std::int64_t bias = 0) {
    return KeyConverter([=](std::uint64_t id) {
      const std::uint64_t field = shift >= 64 ? 0 : (id >> shift) & mask;
      const std::uint64_t value = field + static_cast<std::uint64_t>(bias);
      return std::optional<LookupKey>(LookupKey{value, value});
    });
  }

  // value = id, а hash = хеш младших id_bytes байт ID (little-endian) — для
  // хеш-таблиц, которые хешируют числовой ключ перед выбором корзины.
  static KeyConverter HashedId(HashAlgorithm algorithm, std::size_t id_bytes = 4) {
    if (id_bytes == 0 || id_bytes > 8) throw std::invalid_argument("KeyConverter::HashedId: id_bytes must be 1..8");
    return KeyConverter([=](std::uint64_t id) {
      unsigned char bytes[8];
      for (std::size_t i = 0; i < id_bytes; ++i) bytes[i] = static_cast<unsigned char>(id >> (8 * i));
      return std::optional<LookupKey>(LookupKey{id, HashBytes(algorithm, bytes, id_bytes)});
    });
  }

  static KeyConverter Custom(Function function) {
    if (!function) throw std::invalid_argument("KeyConverter::Custom: function is empty");
    return KeyConverter(std::move(function));
  }

  // Композиция: сначала this, затем next получает полученный value как ID.
  KeyConverter Then(KeyConverter next) const {
    return KeyConverter([first = function_, second = std::move(next.function_)](std::uint64_t id) {
      const std::optional<LookupKey> key = first(id);
      return key ? second(key->value) : std::nullopt;
    });
  }

  std::optional<LookupKey> operator()(std::uint64_t id) const { return function_(id); }

 private:
  explicit KeyConverter(Function function) : function_(std::move(function)) {}

  Function function_;
};

// --- Выбор типа маппинга ---------------------------------------------------------------

struct DictionarySpec {
  StringHashOptions hashing;                                  // как программа хеширует имена
  std::vector<std::string> names;                             // ключ = HashString(name, hashing)
  std::vector<std::pair<std::uint64_t, std::string>> entries;  // явные пары "ключ -> имя"
};

using MappingSpec = std::variant<ArrayTableSpec, HashTableSpec, DictionarySpec>;

// Создаёт таблицу нужного типа. reader нужен таблицам в памяти цели и не
// используется словарём (может быть nullptr).
inline std::shared_ptr<const NameTable> MakeNameTable(std::shared_ptr<const MemoryReader> reader, MappingSpec spec) {
  return std::visit(
      [&](auto&& s) -> std::shared_ptr<const NameTable> {
        using Spec = std::decay_t<decltype(s)>;
        if constexpr (std::is_same_v<Spec, ArrayTableSpec>) {
          return std::make_shared<ArrayNameTable>(std::move(reader), std::move(s));
        } else if constexpr (std::is_same_v<Spec, HashTableSpec>) {
          return std::make_shared<HashNameTable>(std::move(reader), std::move(s));
        } else {
          auto dictionary = std::make_shared<DictionaryNameTable>(s.hashing);
          for (const std::string& name : s.names) dictionary->Add(name);
          for (const auto& [key, name] : s.entries) dictionary->Add(key, name);
          return dictionary;
        }
      },
      std::move(spec));
}

// --- Resolver ---------------------------------------------------------------------

struct ResolverOptions {
  bool cache_hits = true;
  // В живом процессе имя может появиться позже, поэтому промахи по умолчанию
  // не кешируются. Для неизменных снимков их кеширование экономит чтения.
  bool cache_misses = false;
  // При переполнении кеш очищается целиком.
  std::size_t max_cache_entries = std::size_t{1} << 20;
  // Подпись для ID без имени; по умолчанию "<unknown:0x1A2B>".
  std::function<std::string(std::uint64_t id)> fallback_label;
};

struct ResolverStats {
  std::uint64_t lookups = 0;     // вызовов Resolve (= cache_hits + resolved + unresolved)
  std::uint64_t cache_hits = 0;
  std::uint64_t resolved = 0;    // найдено в таблице
  std::uint64_t unresolved = 0;  // не найдено (в том числе отклонено конвертером)
  std::uint64_t rejected = 0;    // отклонено конвертером
};

class GenericDataResolver {
 public:
  GenericDataResolver(std::shared_ptr<const NameTable> table, KeyConverter converter = {}, ResolverOptions options = {})
      : table_(std::move(table)),
        converter_(std::move(converter)),
        options_(std::move(options)),
        state_(std::make_unique<State>()) {
    if (!table_) throw std::invalid_argument("GenericDataResolver: table is null");
  }

  GenericDataResolver(std::shared_ptr<const MemoryReader> reader, MappingSpec spec, KeyConverter converter = {},
                      ResolverOptions options = {})
      : GenericDataResolver(MakeNameTable(std::move(reader), std::move(spec)), std::move(converter),
                            std::move(options)) {}

  // Имя объекта; nullopt — у ID нет имени или его не удалось прочитать.
  std::optional<std::string> Resolve(std::uint64_t id) const {
    State& state = *state_;
    state.lookups.fetch_add(1, std::memory_order_relaxed);
    const bool use_cache = options_.cache_hits || options_.cache_misses;
    if (use_cache) {
      std::shared_lock<std::shared_mutex> lock(state.mutex);
      const auto it = state.cache.find(id);
      if (it != state.cache.end()) {
        state.cache_hits.fetch_add(1, std::memory_order_relaxed);
        return it->second;
      }
    }

    // Чтение памяти — без блокировки: два потока могут прочитать одно имя
    // одновременно, но никто не ждёт чужого медленного чтения.
    std::optional<std::string> name;
    if (const std::optional<LookupKey> key = converter_(id)) {
      name = table_->Lookup(*key);
    } else {
      state.rejected.fetch_add(1, std::memory_order_relaxed);
    }
    (name ? state.resolved : state.unresolved).fetch_add(1, std::memory_order_relaxed);

    if (name ? options_.cache_hits : options_.cache_misses) {
      std::unique_lock<std::shared_mutex> lock(state.mutex);
      if (state.cache.size() >= options_.max_cache_entries) state.cache.clear();
      state.cache.emplace(id, name);
    }
    return name;
  }

  // Имя или подпись для неизвестного ID — всегда непустая строка в UTF-8.
  std::string Label(std::uint64_t id) const {
    if (std::optional<std::string> name = Resolve(id)) return std::move(*name);
    if (options_.fallback_label) return options_.fallback_label(id);
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "<unknown:0x%llX>", static_cast<unsigned long long>(id));
    return buffer;
  }

  std::vector<std::string> Labels(const std::vector<std::uint64_t>& ids) const {
    std::vector<std::string> labels;
    labels.reserve(ids.size());
    for (const std::uint64_t id : ids) labels.push_back(Label(id));
    return labels;
  }

  // Все записи таблицы. Ключи — ключи таблицы (после конвертера), а не исходные
  // ID: конвертер не обязан быть обратимым.
  void ForEachName(const NameVisitor& visitor) const { table_->ForEach(visitor); }

  // Снимок таблицы в локальный словарь "ключ таблицы -> имя". Resolver над ним
  // с тем же конвертером даёт те же имена, когда процесс уже завершён.
  std::shared_ptr<DictionaryNameTable> ExportDictionary() const {
    auto dictionary = std::make_shared<DictionaryNameTable>();
    table_->ForEach([&](std::uint64_t key, const std::string& name) {
      dictionary->Add(key, name);
      return true;
    });
    return dictionary;
  }

  void ClearCache() {
    std::unique_lock<std::shared_mutex> lock(state_->mutex);
    state_->cache.clear();
  }

  ResolverStats stats() const {
    const State& s = *state_;
    ResolverStats out;
    out.lookups = s.lookups.load(std::memory_order_relaxed);
    out.cache_hits = s.cache_hits.load(std::memory_order_relaxed);
    out.resolved = s.resolved.load(std::memory_order_relaxed);
    out.unresolved = s.unresolved.load(std::memory_order_relaxed);
    out.rejected = s.rejected.load(std::memory_order_relaxed);
    return out;
  }

  const NameTable& table() const noexcept { return *table_; }
  const KeyConverter& converter() const noexcept { return converter_; }

 private:
  // Отдельно от resolver'а, чтобы тот оставался перемещаемым (мьютекс — нет).
  struct State {
    std::shared_mutex mutex;
    std::unordered_map<std::uint64_t, std::optional<std::string>> cache;
    std::atomic<std::uint64_t> lookups{0};
    std::atomic<std::uint64_t> cache_hits{0};
    std::atomic<std::uint64_t> resolved{0};
    std::atomic<std::uint64_t> unresolved{0};
    std::atomic<std::uint64_t> rejected{0};
  };

  std::shared_ptr<const NameTable> table_;
  KeyConverter converter_;
  ResolverOptions options_;
  std::unique_ptr<State> state_;
};

}  // namespace memrefl

// memrefl — модуль 3: схемы типов и значения свойств.
//
// Схема (TypeSchema) описывает раскладку объекта: имя свойства -> смещение и
// тип. Схемы берутся из двух источников:
//   * статически — описаны вручную или загружены из конфига (SchemaRegistry);
//     поддерживается одиночное наследование через имя родителя;
//   * динамически — из метаданных рефлексии самой программы
//     (ReflectionLoader, reflection_loader.hpp): имена свойств там хранятся
//     как ID и разрешаются через GenericDataResolver (модуль 2).
//
// Значение свойства — PropertyValue: bool, целое (со знаком / без), double,
// строка UTF-8 или Address. Ошибки возвращаются как Result<T> с кодом и
// понятным сообщением, а не исключением: в инструменте анализа "нет значения"
// — обычная ситуация (объект удалён, указатель пуст, ID без имени).

#pragma once

#include "memrefl/name_table.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace memrefl {

// --- Типы свойств --------------------------------------------------------------

enum class PropertyType : std::uint8_t {
  Bool,  // байт; с bool_mask — бит в байте (битовые поля)
  Int8,
  Int16,
  Int32,
  Int64,
  UInt8,
  UInt16,
  UInt32,
  UInt64,
  Float,
  Double,
  Pointer,        // произвольный указатель -> Address
  String,         // строка по StringFormat (модуль 2)
  Name,           // числовой ID имени -> строка через GenericDataResolver
  Struct,         // вложенная структура по значению
  StructPointer,  // указатель на объект/структуру
  Array,          // массив, см. ArrayLayout
  Unknown,        // тип не распознан (динамическая схема): свойство видно, но не читается
};

inline const char* PropertyTypeName(PropertyType type) noexcept {
  switch (type) {
    case PropertyType::Bool: return "Bool";
    case PropertyType::Int8: return "Int8";
    case PropertyType::Int16: return "Int16";
    case PropertyType::Int32: return "Int32";
    case PropertyType::Int64: return "Int64";
    case PropertyType::UInt8: return "UInt8";
    case PropertyType::UInt16: return "UInt16";
    case PropertyType::UInt32: return "UInt32";
    case PropertyType::UInt64: return "UInt64";
    case PropertyType::Float: return "Float";
    case PropertyType::Double: return "Double";
    case PropertyType::Pointer: return "Pointer";
    case PropertyType::String: return "String";
    case PropertyType::Name: return "Name";
    case PropertyType::Struct: return "Struct";
    case PropertyType::StructPointer: return "StructPointer";
    case PropertyType::Array: return "Array";
    case PropertyType::Unknown: return "Unknown";
  }
  return "?";
}

// Размер значения в объекте для типов фиксированного размера; 0 — размер
// задаётся описанием (Name — name_id_size, String/Struct/Array — раскладкой).
inline std::size_t ScalarSize(PropertyType type, std::size_t pointer_size) noexcept {
  switch (type) {
    case PropertyType::Bool:
    case PropertyType::Int8:
    case PropertyType::UInt8: return 1;
    case PropertyType::Int16:
    case PropertyType::UInt16: return 2;
    case PropertyType::Int32:
    case PropertyType::UInt32:
    case PropertyType::Float: return 4;
    case PropertyType::Int64:
    case PropertyType::UInt64:
    case PropertyType::Double: return 8;
    case PropertyType::Pointer:
    case PropertyType::StructPointer: return pointer_size;
    default: return 0;
  }
}

// --- Описание свойства -----------------------------------------------------------

// Массив: элементы прямо в объекте (inline_count штук) или динамический
// контейнер {T* data; ...; count} — data по смещению свойства, count по
// count_offset от начала контейнера.
struct ArrayLayout {
  PropertyType element = PropertyType::Int32;  // Struct/StructPointer — см. struct_type, String — см. string
  std::size_t stride = 0;                      // шаг элементов; 0 — размер скалярного элемента
  bool dynamic = false;
  std::size_t inline_count = 0;
  std::size_t count_offset = sizeof(void*);
  std::size_t count_size = 4;
  std::size_t max_count = std::size_t{1} << 20;  // защита от мусорного count
};

struct PropertyDesc {
  std::string name;
  PropertyType type = PropertyType::Int32;
  std::size_t offset = 0;  // от начала объекта

  std::uint8_t bool_mask = 0xFF;  // Bool: какие биты байта означают true
  StringFormat string;            // String (и строковые элементы массива)
  std::string name_resolver;      // Name: ключ resolver'а в PropertyFetcher ("" — основной)
  std::size_t name_id_size = 4;   // Name: ширина ID

  // Struct/StructPointer и элементы-структуры массива: имя статической схемы
  // или адрес объекта-типа в памяти цели (динамическая рефлексия). Для
  // StructPointer без типа он определяется по самому объекту.
  std::string struct_type;
  std::uintptr_t struct_type_object = 0;

  ArrayLayout array;
};

// Короткие конструкторы описаний.
inline PropertyDesc ScalarProperty(std::string name, PropertyType type, std::size_t offset) {
  PropertyDesc d;
  d.name = std::move(name);
  d.type = type;
  d.offset = offset;
  return d;
}

inline PropertyDesc BoolProperty(std::string name, std::size_t offset, std::uint8_t mask = 0xFF) {
  PropertyDesc d = ScalarProperty(std::move(name), PropertyType::Bool, offset);
  d.bool_mask = mask;
  return d;
}

inline PropertyDesc StringProperty(std::string name, std::size_t offset, StringFormat format = {}) {
  PropertyDesc d = ScalarProperty(std::move(name), PropertyType::String, offset);
  d.string = format;
  return d;
}

inline PropertyDesc NameProperty(std::string name, std::size_t offset, std::size_t id_size = 4,
                                 std::string resolver = {}) {
  PropertyDesc d = ScalarProperty(std::move(name), PropertyType::Name, offset);
  d.name_id_size = id_size;
  d.name_resolver = std::move(resolver);
  return d;
}

inline PropertyDesc StructProperty(std::string name, std::size_t offset, std::string type) {
  PropertyDesc d = ScalarProperty(std::move(name), PropertyType::Struct, offset);
  d.struct_type = std::move(type);
  return d;
}

// Указатель на объект; без типа — тип определяется по объекту (рефлексия).
inline PropertyDesc ObjectProperty(std::string name, std::size_t offset, std::string type = {}) {
  PropertyDesc d = ScalarProperty(std::move(name), PropertyType::StructPointer, offset);
  d.struct_type = std::move(type);
  return d;
}

inline PropertyDesc InlineArrayProperty(std::string name, std::size_t offset, PropertyType element,
                                        std::size_t count, std::size_t stride = 0) {
  PropertyDesc d = ScalarProperty(std::move(name), PropertyType::Array, offset);
  d.array.element = element;
  d.array.inline_count = count;
  d.array.stride = stride;
  return d;
}

// Контейнер {T* data; ...; count} по смещению offset.
inline PropertyDesc DynamicArrayProperty(std::string name, std::size_t offset, PropertyType element,
                                         std::size_t count_offset, std::size_t count_size = 4,
                                         std::size_t stride = 0) {
  PropertyDesc d = ScalarProperty(std::move(name), PropertyType::Array, offset);
  d.array.element = element;
  d.array.dynamic = true;
  d.array.count_offset = count_offset;
  d.array.count_size = count_size;
  d.array.stride = stride;
  return d;
}

// --- Схемы -----------------------------------------------------------------------

class TypeSchema {
 public:
  explicit TypeSchema(std::string name, std::string parent = {}) : name_(std::move(name)), parent_(std::move(parent)) {}

  // Бросает std::invalid_argument при повторном имени или некорректном описании.
  TypeSchema& Add(PropertyDesc property) {
    if (property.name.empty()) throw std::invalid_argument("TypeSchema " + name_ + ": property name is empty");
    if (index_.count(property.name)) {
      throw std::invalid_argument("TypeSchema " + name_ + ": duplicate property " + property.name);
    }
    index_.emplace(property.name, properties_.size());
    properties_.push_back(std::move(property));
    return *this;
  }

  TypeSchema& Add(std::string name, PropertyType type, std::size_t offset) {
    return Add(ScalarProperty(std::move(name), type, offset));
  }

  // Только собственные свойства (без родителя).
  const PropertyDesc* Find(std::string_view name) const {
    const auto it = index_.find(std::string(name));
    return it == index_.end() ? nullptr : &properties_[it->second];
  }

  const std::string& name() const noexcept { return name_; }
  const std::string& parent() const noexcept { return parent_; }
  const std::vector<PropertyDesc>& properties() const noexcept { return properties_; }

 private:
  std::string name_;
  std::string parent_;
  std::vector<PropertyDesc> properties_;
  std::unordered_map<std::string, std::size_t> index_;
};

// Набор статических схем. Заполняется до использования и дальше только читается.
class SchemaRegistry {
 public:
  static constexpr std::size_t kMaxInheritanceDepth = 64;  // защита от циклов в описаниях

  SchemaRegistry& Add(TypeSchema schema) {
    const std::string name = schema.name();
    if (!schemas_.emplace(name, std::move(schema)).second) {
      throw std::invalid_argument("SchemaRegistry: duplicate type " + name);
    }
    return *this;
  }

  const TypeSchema* Find(std::string_view type) const {
    const auto it = schemas_.find(std::string(type));
    return it == schemas_.end() ? nullptr : &it->second;
  }

  // Цепочка от типа к самому базовому родителю (без повторов).
  std::vector<const TypeSchema*> Lineage(std::string_view type) const {
    std::vector<const TypeSchema*> chain;
    for (const TypeSchema* s = Find(type); s != nullptr && chain.size() < kMaxInheritanceDepth;
         s = s->parent().empty() ? nullptr : Find(s->parent())) {
      if (std::find(chain.begin(), chain.end(), s) != chain.end()) break;  // цикл
      chain.push_back(s);
    }
    return chain;
  }

  std::size_t size() const noexcept { return schemas_.size(); }

 private:
  std::unordered_map<std::string, TypeSchema> schemas_;
};

// --- Значения и ошибки -------------------------------------------------------------

struct Address {
  std::uintptr_t value = 0;
  friend bool operator==(Address a, Address b) noexcept { return a.value == b.value; }
  friend bool operator!=(Address a, Address b) noexcept { return a.value != b.value; }
};

using PropertyValue = std::variant<bool, std::int64_t, std::uint64_t, double, std::string, Address>;

// Текст для отчётов: true/false, число, строка как есть, адрес в hex.
inline std::string FormatValue(const PropertyValue& value) {
  char buffer[64];
  switch (value.index()) {
    case 0: return std::get<bool>(value) ? "true" : "false";
    case 1: return std::to_string(std::get<std::int64_t>(value));
    case 2: return std::to_string(std::get<std::uint64_t>(value));
    case 3: std::snprintf(buffer, sizeof(buffer), "%.9g", std::get<double>(value)); return buffer;
    case 4: return std::get<std::string>(value);
    default:
      std::snprintf(buffer, sizeof(buffer), "0x%llX", static_cast<unsigned long long>(std::get<Address>(value).value));
      return buffer;
  }
}

enum class FetchErrorCode {
  InvalidPath,       // синтаксис пути или выбор поля у скаляра
  UnknownType,       // нет схемы для типа
  UnknownProperty,   // у типа нет такого свойства
  NullPointer,       // по пути встретился нулевой указатель
  UnreadableMemory,  // память недоступна или данные (строка, метаданные) не читаются
  IndexOutOfRange,
  UnsupportedType,   // тип не читается (Unknown, массив массивов, нет resolver'а...)
  TypeMismatch,      // Get<T> с несовместимым T
  ValueOutOfRange,   // значение не помещается в T или мусорный размер
  NameUnresolved,    // ID имени не найден в таблице имён
};

inline const char* FetchErrorCodeName(FetchErrorCode code) noexcept {
  switch (code) {
    case FetchErrorCode::InvalidPath: return "InvalidPath";
    case FetchErrorCode::UnknownType: return "UnknownType";
    case FetchErrorCode::UnknownProperty: return "UnknownProperty";
    case FetchErrorCode::NullPointer: return "NullPointer";
    case FetchErrorCode::UnreadableMemory: return "UnreadableMemory";
    case FetchErrorCode::IndexOutOfRange: return "IndexOutOfRange";
    case FetchErrorCode::UnsupportedType: return "UnsupportedType";
    case FetchErrorCode::TypeMismatch: return "TypeMismatch";
    case FetchErrorCode::ValueOutOfRange: return "ValueOutOfRange";
    case FetchErrorCode::NameUnresolved: return "NameUnresolved";
  }
  return "?";
}

struct FetchError {
  FetchErrorCode code;
  std::string message;
};

// value() при ошибке бросает это исключение с текстом ошибки.
class BadResultAccess : public std::logic_error {
 public:
  explicit BadResultAccess(const FetchError& error)
      : std::logic_error(std::string(FetchErrorCodeName(error.code)) + ": " + error.message), error_(error) {}
  const FetchError& error() const noexcept { return error_; }

 private:
  FetchError error_;
};

// Значение или ошибка (аналог std::expected из C++23).
template <class T>
class Result {
 public:
  Result(T value) : data_(std::in_place_index<0>, std::move(value)) {}            // NOLINT: неявное
  Result(FetchError error) : data_(std::in_place_index<1>, std::move(error)) {}  // NOLINT: неявное

  bool ok() const noexcept { return data_.index() == 0; }
  explicit operator bool() const noexcept { return ok(); }

  const T& value() const& {
    if (!ok()) throw BadResultAccess(std::get<1>(data_));
    return std::get<0>(data_);
  }
  // У временного Result значение отдаётся по значению, а не ссылкой: так
  // "for (auto& e : fetcher.Dump(obj).value())" не обращается к уничтоженному объекту.
  T value() && {
    if (!ok()) throw BadResultAccess(std::get<1>(data_));
    return std::get<0>(std::move(data_));
  }
  const T& operator*() const& { return value(); }
  T operator*() && { return std::move(*this).value(); }
  const T* operator->() const { return &value(); }

  T value_or(T fallback) const& { return ok() ? std::get<0>(data_) : std::move(fallback); }

  // Только для ошибки (иначе std::logic_error).
  const FetchError& error() const {
    if (ok()) throw std::logic_error("Result holds a value, not an error");
    return std::get<1>(data_);
  }

 private:
  std::variant<T, FetchError> data_;
};

}  // namespace memrefl

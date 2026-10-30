// memrefl — модуль 3: доступ к свойствам объектов по строковому пути.
//
//   fetcher.Get<int>(player, "Health")
//   fetcher.Get<std::string>(player, "Inventory.Items[1].Name")
//
// Как обрабатывается путь:
//   1. Тип объекта: имя статической схемы из ObjectRef или — если оно не
//      задано — тип из памяти через ReflectionLoader (ID имён типов и
//      свойств разрешаются GenericDataResolver'ом, модуль 2).
//   2. Каждый сегмент пути ищется в схеме (с учётом родителей) и
//      превращается в шаги PointerPath: смещение поля, переход по указателю
//      (StructPointer, данные динамического массива). Индекс массива
//      проверяется по его длине в памяти.
//   3. PointerPath разрешается и значение читается с проверкой границ:
//      целые расширяются со знаком, ID имён разрешаются в строки, строки
//      приводятся к UTF-8. Get<T> дополнительно проверяет, что значение
//      помещается в T.
// Корень объекта — PointerPath: адрес или цепочка от глобальной переменной,
// найденной сигнатурой (модуль 1), поэтому путь остаётся верным, даже если
// программа пересоздала объект.
//
// Ошибки не бросаются, а возвращаются как Result<T> (код + сообщение с путём).
// Locate() отдаёт готовый PointerPath: его можно сохранить и перечитывать
// значение через Read() без повторного разбора пути.
//
// Настройка (SetNameResolver, SetReflection) — до использования; после неё
// константные методы можно вызывать из нескольких потоков.

#pragma once

#include "memrefl/data_resolver.hpp"
#include "memrefl/memory_reader.hpp"
#include "memrefl/property_schema.hpp"
#include "memrefl/reflection_loader.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace memrefl {

// Объект, к свойствам которого обращаются.
struct ObjectRef {
  PointerPath root;  // адрес объекта или цепочка указателей к нему
  std::string type;  // имя статической схемы; пусто — тип определяется по памяти (рефлексия)

  ObjectRef() = default;
  ObjectRef(std::uintptr_t address, std::string type_name = {}) : root(address), type(std::move(type_name)) {}
  ObjectRef(PointerPath path, std::string type_name = {}) : root(std::move(path)), type(std::move(type_name)) {}
};

// Где лежит значение и как его читать.
struct PropertyLocation {
  PointerPath path;        // от корня объекта до значения; разрешается заново при каждом Read()
  PropertyDesc property;   // описание значения (для элемента массива — описание элемента)
  std::string owner_type;  // тип, в схеме которого найдено свойство
};

struct PropertyEntry {
  std::string name;
  PropertyType type;
  std::size_t offset;
  std::string owner_type;
  Result<PropertyValue> value;
};

struct FetcherOptions {
  std::size_t pointer_size = sizeof(void*);  // ширина указателей цели
};

namespace detail {

struct PathSegment {
  std::string name;
  std::optional<std::size_t> index;
};

// "A.B[3].C": имена — любые символы, кроме '.', '[' и ']'; индекс — десятичный.
inline Result<std::vector<PathSegment>> ParsePropertyPath(std::string_view path) {
  auto fail = [&](std::size_t at, const char* what) {
    return FetchError{FetchErrorCode::InvalidPath,
                      "bad property path \"" + std::string(path) + "\" at " + std::to_string(at) + ": " + what};
  };
  std::vector<PathSegment> segments;
  std::size_t i = 0;
  for (;;) {
    const std::size_t start = i;
    while (i < path.size() && path[i] != '.' && path[i] != '[' && path[i] != ']') ++i;
    if (i == start) return fail(i, "expected property name");
    PathSegment segment{std::string(path.substr(start, i - start)), std::nullopt};
    if (i < path.size() && path[i] == '[') {
      const std::size_t digits = ++i;
      std::size_t index = 0;
      for (; i < path.size() && path[i] >= '0' && path[i] <= '9'; ++i) {
        if (index > ((std::numeric_limits<std::size_t>::max)() - 9) / 10) return fail(digits, "index is too large");
        index = index * 10 + static_cast<std::size_t>(path[i] - '0');
      }
      if (i == digits) return fail(i, "expected array index");
      if (i >= path.size() || path[i] != ']') return fail(i, "expected ']'");
      ++i;
      segment.index = index;
    }
    segments.push_back(std::move(segment));
    if (i == path.size()) return segments;
    if (path[i] != '.') return fail(i, "expected '.'");
    ++i;
  }
}

// Расстояние Левенштейна — для подсказки "did you mean".
inline std::size_t EditDistance(std::string_view a, std::string_view b) {
  std::vector<std::size_t> row(b.size() + 1);
  for (std::size_t j = 0; j <= b.size(); ++j) row[j] = j;
  for (std::size_t i = 1; i <= a.size(); ++i) {
    std::size_t diagonal = row[0];
    row[0] = i;
    for (std::size_t j = 1; j <= b.size(); ++j) {
      const std::size_t above = row[j];
      row[j] = (std::min)({row[j] + 1, row[j - 1] + 1, diagonal + (a[i - 1] == b[j - 1] ? 0 : 1)});
      diagonal = above;
    }
  }
  return row[b.size()];
}

inline std::string Hex(std::uintptr_t value) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "0x%llX", static_cast<unsigned long long>(value));
  return buffer;
}

inline const char* ValueKind(const PropertyValue& value) {
  static constexpr const char* kKinds[] = {"bool", "signed integer", "unsigned integer", "floating point",
                                           "string", "address"};
  return kKinds[value.index()];
}

template <class T>
inline constexpr bool kDependentFalse = false;

}  // namespace detail

// Преобразование значения к T с проверкой: целые — только если помещаются в
// T, вещественные принимают и целые, остальное — строго по виду значения.
template <class T>
Result<T> ConvertValue(const PropertyValue& value, std::string_view what = "value") {
  auto mismatch = [&] {
    return FetchError{FetchErrorCode::TypeMismatch,
                      std::string(what) + " is " + detail::ValueKind(value) + ", requested type does not match"};
  };
  auto out_of_range = [&] {
    return FetchError{FetchErrorCode::ValueOutOfRange,
                      std::string(what) + " = " + FormatValue(value) + " does not fit into the requested type"};
  };
  if constexpr (std::is_same_v<T, PropertyValue>) {
    return value;
  } else if constexpr (std::is_same_v<T, bool>) {
    if (const bool* b = std::get_if<bool>(&value)) return *b;
    return mismatch();
  } else if constexpr (std::is_integral_v<T>) {
    using Limits = std::numeric_limits<T>;
    if (const std::int64_t* s = std::get_if<std::int64_t>(&value)) {
      const bool fits = std::is_signed_v<T> ? (*s >= static_cast<std::int64_t>((Limits::min)()) &&
                                               *s <= static_cast<std::int64_t>((Limits::max)()))
                                            : (*s >= 0 && static_cast<std::uint64_t>(*s) <=
                                                              static_cast<std::uint64_t>((Limits::max)()));
      if (!fits) return out_of_range();
      return static_cast<T>(*s);
    }
    if (const std::uint64_t* u = std::get_if<std::uint64_t>(&value)) {
      if (*u > static_cast<std::uint64_t>((Limits::max)())) return out_of_range();
      return static_cast<T>(*u);
    }
    return mismatch();
  } else if constexpr (std::is_floating_point_v<T>) {
    if (const double* d = std::get_if<double>(&value)) return static_cast<T>(*d);
    if (const std::int64_t* s = std::get_if<std::int64_t>(&value)) return static_cast<T>(*s);
    if (const std::uint64_t* u = std::get_if<std::uint64_t>(&value)) return static_cast<T>(*u);
    return mismatch();
  } else if constexpr (std::is_same_v<T, std::string>) {
    if (const std::string* s = std::get_if<std::string>(&value)) return *s;
    return mismatch();
  } else if constexpr (std::is_same_v<T, Address>) {
    if (const Address* a = std::get_if<Address>(&value)) return *a;
    return mismatch();
  } else {
    static_assert(detail::kDependentFalse<T>, "Get<T>: T must be bool, an integer, a floating point type, "
                                              "std::string, Address or PropertyValue");
  }
}

class PropertyFetcher {
 public:
  PropertyFetcher(std::shared_ptr<const MemoryReader> reader, std::shared_ptr<const SchemaRegistry> schemas = nullptr,
                  FetcherOptions options = {})
      : reader_(std::move(reader)), schemas_(std::move(schemas)), options_(options) {
    if (!reader_) throw std::invalid_argument("PropertyFetcher: reader is null");
  }

  // Resolver для свойств типа Name; key — PropertyDesc::name_resolver.
  PropertyFetcher& SetNameResolver(std::shared_ptr<const GenericDataResolver> resolver, std::string key = {}) {
    if (!resolver) throw std::invalid_argument("PropertyFetcher: name resolver is null");
    name_resolvers_[std::move(key)] = std::move(resolver);
    return *this;
  }

  // Динамические типы из метаданных программы.
  PropertyFetcher& SetReflection(std::shared_ptr<const ReflectionLoader> loader) {
    reflection_ = std::move(loader);
    return *this;
  }

  // --- Доступ к значениям ------------------------------------------------------------

  Result<PropertyValue> Fetch(const ObjectRef& object, std::string_view path) const {
    const Result<PropertyLocation> location = Locate(object, path);
    if (!location) return location.error();
    return Read(*location);
  }

  template <class T>
  Result<T> Get(const ObjectRef& object, std::string_view path) const {
    const Result<PropertyValue> value = Fetch(object, path);
    if (!value) return value.error();
    return ConvertValue<T>(*value, path);
  }

  // Путь к значению без чтения самого значения. Результат можно хранить и
  // перечитывать через Read() — например, опрашивать значение раз в кадр.
  // Индексы динамических массивов проверяются на момент вызова Locate().
  Result<PropertyLocation> Locate(const ObjectRef& object, std::string_view path) const {
    const Result<std::vector<detail::PathSegment>> segments = detail::ParsePropertyPath(path);
    if (!segments) return segments.error();

    PointerPath navigation = object.root;
    Result<TypeView> type = RootType(object);
    if (!type) return type.error();
    TypeView view = std::move(type).value();

    std::string walked;  // пройденная часть пути — для сообщений
    for (std::size_t i = 0; i < segments->size(); ++i) {
      const detail::PathSegment& segment = (*segments)[i];
      std::string owner;
      const PropertyDesc* found = view.Find(segment.name, &owner);
      if (found == nullptr) return UnknownProperty(view, segment.name);

      PropertyDesc leaf = *found;
      walked += (walked.empty() ? "" : ".") + segment.name;
      navigation.Offset(static_cast<std::ptrdiff_t>(leaf.offset));
      if (segment.index) {
        Result<PropertyDesc> element = SelectElement(leaf, *segment.index, navigation, walked);
        if (!element) return element.error();
        leaf = std::move(element).value();
        walked += "[" + std::to_string(*segment.index) + "]";
      }
      if (i + 1 == segments->size()) {
        leaf.name = walked;
        return PropertyLocation{std::move(navigation), std::move(leaf), std::move(owner)};
      }

      // Переход внутрь вложенного объекта.
      if (leaf.type == PropertyType::Struct) {
        type = NestedType(leaf, navigation, false);
      } else if (leaf.type == PropertyType::StructPointer) {
        navigation.Dereference();
        type = NestedType(leaf, navigation, true);
      } else {
        return FetchError{FetchErrorCode::InvalidPath,
                          "'" + walked + "' is " + PropertyTypeName(leaf.type) + " and has no properties"};
      }
      if (!type) return type.error();
      view = std::move(type).value();
    }
    return FetchError{FetchErrorCode::InvalidPath, "empty property path"};  // недостижимо: путь не пуст
  }

  Result<PropertyValue> Read(const PropertyLocation& location) const {
    const Result<std::uintptr_t> address = Navigate(location.path, location.property.name);
    if (!address) return address.error();
    return ReadValue(*address, location.property);
  }

  // Число элементов массива.
  Result<std::size_t> Count(const ObjectRef& object, std::string_view array_path) const {
    const Result<PropertyLocation> location = Locate(object, array_path);
    if (!location) return location.error();
    const PropertyDesc& array = location->property;
    if (array.type != PropertyType::Array) {
      return FetchError{FetchErrorCode::TypeMismatch, "'" + array.name + "' is not an array"};
    }
    if (!array.array.dynamic) return array.array.inline_count;
    const Result<std::uintptr_t> container = Navigate(location->path, array.name);
    if (!container) return container.error();
    return ReadCount(*container, array);
  }

  // Имя типа объекта (из ObjectRef или из метаданных).
  Result<std::string> TypeName(const ObjectRef& object) const {
    const Result<TypeView> view = RootType(object);
    if (!view) return view.error();
    return view->name;
  }

  // Все свойства объекта со значениями (родительские — первыми). Ошибка
  // чтения отдельного свойства не прерывает выгрузку.
  Result<std::vector<PropertyEntry>> Dump(const ObjectRef& object) const {
    const Result<std::uintptr_t> address = Navigate(object.root, "object");
    if (!address) return address.error();
    const Result<TypeView> view = RootType(object);
    if (!view) return view.error();
    std::vector<PropertyEntry> entries;
    for (const auto& [property, owner] : view->All()) {
      entries.push_back({property->name, property->type, property->offset, owner,
                         ReadValue(*address + property->offset, *property)});
    }
    return entries;
  }

 private:
  // Схема с цепочкой родителей (статическая) или плоская (динамическая).
  struct TypeView {
    std::string name;
    std::vector<const TypeSchema*> lineage;   // от типа к базовому
    std::shared_ptr<const TypeSchema> holder;  // держит динамическую схему

    const PropertyDesc* Find(std::string_view property, std::string* owner) const {
      for (const TypeSchema* schema : lineage) {
        if (const PropertyDesc* found = schema->Find(property)) {
          *owner = schema->name();
          return found;
        }
      }
      return nullptr;
    }

    // Родительские свойства первыми; переопределённое потомком — на месте родительского.
    std::vector<std::pair<const PropertyDesc*, std::string>> All() const {
      std::vector<std::pair<const PropertyDesc*, std::string>> all;
      std::unordered_map<std::string, std::size_t> index;
      for (auto it = lineage.rbegin(); it != lineage.rend(); ++it) {
        for (const PropertyDesc& property : (*it)->properties()) {
          const auto [slot, inserted] = index.emplace(property.name, all.size());
          if (inserted) {
            all.emplace_back(&property, (*it)->name());
          } else {
            all[slot->second] = {&property, (*it)->name()};
          }
        }
      }
      return all;
    }
  };

  Result<TypeView> StaticType(const std::string& name) const {
    if (!schemas_ || schemas_->Find(name) == nullptr) {
      return FetchError{FetchErrorCode::UnknownType, "no schema for type '" + name + "'"};
    }
    return TypeView{name, schemas_->Lineage(name), nullptr};
  }

  static TypeView DynamicType(std::shared_ptr<const TypeSchema> schema) {
    TypeView view{schema->name(), {schema.get()}, nullptr};
    view.holder = std::move(schema);
    return view;
  }

  Result<TypeView> TypeOfObjectAt(std::uintptr_t object) const {
    if (!reflection_) {
      return FetchError{FetchErrorCode::UnknownType,
                        "object type is not specified and reflection is not configured (SetReflection)"};
    }
    const Result<std::shared_ptr<const TypeSchema>> schema = reflection_->LoadForObject(object);
    if (!schema) return schema.error();
    return DynamicType(*schema);
  }

  Result<TypeView> RootType(const ObjectRef& object) const {
    if (!object.type.empty()) return StaticType(object.type);
    const Result<std::uintptr_t> address = Navigate(object.root, "object");
    if (!address) return address.error();
    return TypeOfObjectAt(*address);
  }

  // Тип вложенной структуры или объекта по указателю. Для указателя без
  // статического типа предпочитается фактический тип объекта (наследник).
  Result<TypeView> NestedType(const PropertyDesc& property, const PointerPath& navigation, bool by_pointer) const {
    if (!property.struct_type.empty()) return StaticType(property.struct_type);
    if (by_pointer && reflection_) {
      const Result<std::uintptr_t> object = Navigate(navigation, property.name);
      if (!object) return object.error();
      const Result<TypeView> actual = TypeOfObjectAt(*object);
      if (actual || property.struct_type_object == 0) return actual;
    }
    if (property.struct_type_object != 0 && reflection_) {
      const Result<std::shared_ptr<const TypeSchema>> schema = reflection_->Load(property.struct_type_object);
      if (!schema) return schema.error();
      return DynamicType(*schema);
    }
    return FetchError{FetchErrorCode::UnknownType, "type of '" + property.name + "' is unknown"};
  }

  // Шаги к элементу массива; возвращает описание элемента.
  Result<PropertyDesc> SelectElement(const PropertyDesc& array, std::size_t index, PointerPath& navigation,
                                     const std::string& walked) const {
    if (array.type != PropertyType::Array) {
      return FetchError{FetchErrorCode::InvalidPath, "'" + walked + "' is " + PropertyTypeName(array.type) +
                                                         ", not an array"};
    }
    const ArrayLayout& layout = array.array;
    if (layout.element == PropertyType::Array || layout.element == PropertyType::Unknown) {
      return FetchError{FetchErrorCode::UnsupportedType,
                        "'" + walked + "': arrays of " + PropertyTypeName(layout.element) + " are not supported"};
    }
    const std::size_t stride = layout.stride != 0 ? layout.stride
                               : layout.element == PropertyType::Name
                                   ? array.name_id_size
                                   : ScalarSize(layout.element, options_.pointer_size);
    if (stride == 0) {
      return FetchError{FetchErrorCode::UnsupportedType, "'" + walked + "': stride is required for " +
                                                             PropertyTypeName(layout.element) + " elements"};
    }

    std::size_t count = layout.inline_count;
    if (layout.dynamic) {
      const Result<std::uintptr_t> container = Navigate(navigation, walked);
      if (!container) return container.error();
      const Result<std::size_t> dynamic_count = ReadCount(*container, array);
      if (!dynamic_count) return dynamic_count.error();
      count = *dynamic_count;
    }
    if (index >= count) {
      return FetchError{FetchErrorCode::IndexOutOfRange, "'" + walked + "[" + std::to_string(index) +
                                                             "]': index out of range, count is " + std::to_string(count)};
    }
    if (layout.dynamic) navigation.Dereference();  // data — первое поле контейнера
    navigation.Offset(static_cast<std::ptrdiff_t>(index * stride));

    PropertyDesc element = array;  // формат строки, struct_type, resolver — общие с массивом
    element.type = layout.element;
    element.offset = 0;
    return element;
  }

  Result<std::size_t> ReadCount(std::uintptr_t container, const PropertyDesc& array) const {
    const std::optional<std::uint64_t> count =
        reader_->ReadUnsigned(container + array.array.count_offset, array.array.count_size);
    if (!count) {
      return FetchError{FetchErrorCode::UnreadableMemory, "cannot read count of '" + array.name + "' at " +
                                                              detail::Hex(container + array.array.count_offset)};
    }
    if (*count > array.array.max_count) {
      return FetchError{FetchErrorCode::ValueOutOfRange, "count of '" + array.name + "' is " +
                                                             std::to_string(*count) + ", above max_count"};
    }
    return static_cast<std::size_t>(*count);
  }

  Result<std::uintptr_t> Navigate(const PointerPath& path, const std::string& what) const {
    const PointerPath::Trace trace = path.ResolveTraced(*reader_, options_.pointer_size);
    if (trace.address) return *trace.address;
    return FetchError{trace.null_pointer ? FetchErrorCode::NullPointer : FetchErrorCode::UnreadableMemory,
                      "'" + what + "': " + (trace.null_pointer ? "null pointer" : "unreadable memory") +
                          " at step " + std::to_string(trace.failed_step + 1) + " of " + path.ToString()};
  }

  FetchError UnknownProperty(const TypeView& view, const std::string& name) const {
    std::string message = "type '" + view.name + "' has no property '" + name + "'";
    std::size_t best = (std::numeric_limits<std::size_t>::max)();
    std::string suggestion;
    for (const auto& entry : view.All()) {
      const std::size_t distance = detail::EditDistance(name, entry.first->name);
      if (distance < best) {
        best = distance;
        suggestion = entry.first->name;
      }
    }
    if (!suggestion.empty() && best <= std::max<std::size_t>(1, name.size() / 3)) {
      message += "; did you mean '" + suggestion + "'?";
    }
    return FetchError{FetchErrorCode::UnknownProperty, std::move(message)};
  }

  Result<PropertyValue> ReadValue(std::uintptr_t address, const PropertyDesc& property) const {
    auto unreadable = [&] {
      return FetchError{FetchErrorCode::UnreadableMemory, "cannot read '" + property.name + "' (" +
                                                              PropertyTypeName(property.type) + ") at " +
                                                              detail::Hex(address)};
    };
    auto pointer_value = [&]() -> Result<PropertyValue> {
      const std::optional<std::uintptr_t> pointer = reader_->ReadPointer(address, options_.pointer_size);
      if (!pointer) return unreadable();
      return PropertyValue(std::in_place_type<Address>, Address{*pointer});
    };

    switch (property.type) {
      case PropertyType::Bool: {
        const std::optional<std::uint8_t> byte = reader_->Read<std::uint8_t>(address);
        if (!byte) return unreadable();
        return PropertyValue(std::in_place_type<bool>, (*byte & property.bool_mask) != 0);
      }
      case PropertyType::Int8:
      case PropertyType::Int16:
      case PropertyType::Int32:
      case PropertyType::Int64: {
        const std::size_t size = ScalarSize(property.type, options_.pointer_size);
        const std::optional<std::uint64_t> raw = reader_->ReadUnsigned(address, size);
        if (!raw) return unreadable();
        std::uint64_t bits = *raw;
        if (size < 8 && ((bits >> (8 * size - 1)) & 1) != 0) bits |= ~std::uint64_t{0} << (8 * size);  // знак
        return PropertyValue(std::in_place_type<std::int64_t>, static_cast<std::int64_t>(bits));
      }
      case PropertyType::UInt8:
      case PropertyType::UInt16:
      case PropertyType::UInt32:
      case PropertyType::UInt64: {
        const std::optional<std::uint64_t> raw =
            reader_->ReadUnsigned(address, ScalarSize(property.type, options_.pointer_size));
        if (!raw) return unreadable();
        return PropertyValue(std::in_place_type<std::uint64_t>, *raw);
      }
      case PropertyType::Float: {
        const std::optional<float> value = reader_->Read<float>(address);
        if (!value) return unreadable();
        return PropertyValue(std::in_place_type<double>, static_cast<double>(*value));
      }
      case PropertyType::Double: {
        const std::optional<double> value = reader_->Read<double>(address);
        if (!value) return unreadable();
        return PropertyValue(std::in_place_type<double>, *value);
      }
      case PropertyType::Pointer:
      case PropertyType::StructPointer:
        return pointer_value();
      case PropertyType::Struct:
        return PropertyValue(std::in_place_type<Address>, Address{address});
      case PropertyType::Array:
        if (property.array.dynamic) return pointer_value();  // адрес данных
        return PropertyValue(std::in_place_type<Address>, Address{address});
      case PropertyType::String: {
        std::optional<std::string> text = ReadString(*reader_, address, property.string);
        if (!text) return unreadable();
        return PropertyValue(std::in_place_type<std::string>, std::move(*text));
      }
      case PropertyType::Name: {
        const auto resolver = name_resolvers_.find(property.name_resolver);
        if (resolver == name_resolvers_.end()) {
          return FetchError{FetchErrorCode::UnsupportedType, "'" + property.name + "': no name resolver '" +
                                                                 property.name_resolver + "' (SetNameResolver)"};
        }
        const std::optional<std::uint64_t> id = reader_->ReadUnsigned(address, property.name_id_size);
        if (!id) return unreadable();
        std::optional<std::string> name = resolver->second->Resolve(*id);
        if (!name) {
          return FetchError{FetchErrorCode::NameUnresolved,
                            "'" + property.name + "': name id " + resolver->second->Label(*id) + " is not in the table"};
        }
        return PropertyValue(std::in_place_type<std::string>, std::move(*name));
      }
      case PropertyType::Unknown:
        break;
    }
    return FetchError{FetchErrorCode::UnsupportedType,
                      "'" + property.name + "' has an unrecognized type and cannot be read"};
  }

  std::shared_ptr<const MemoryReader> reader_;
  std::shared_ptr<const SchemaRegistry> schemas_;
  FetcherOptions options_;
  std::unordered_map<std::string, std::shared_ptr<const GenericDataResolver>> name_resolvers_;
  std::shared_ptr<const ReflectionLoader> reflection_;
};

}  // namespace memrefl

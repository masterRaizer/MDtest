// memrefl — модуль 3: схемы из метаданных рефлексии самой программы.
//
// Многие C++-программы (движки, фреймворки с метаобъектами, кодогенерация для
// сериализации) хранят в памяти описания своих типов:
//
//   объект ──type──▶ тип { name_id, super*, properties }
//                                         │
//                         дескриптор { name_id, offset, tag, inner_type*, next* } ─▶ ...
//
// ReflectionLoader читает такие описания по раскладке ReflectionLayout и
// строит TypeSchema:
//   * ID имён типов и свойств разрешаются через GenericDataResolver (модуль 2);
//   * свойства родителей (super) включаются в схему, родительские — первыми;
//     свойство потомка с тем же именем заменяет родительское;
//   * тег дескриптора переводится в описание через таблицу tags (или свой
//     classify), неизвестный тег даёт свойство типа Unknown — оно видно в
//     выгрузке, но не читается.
// Адреса типов или реестра типов обычно находят сигнатурами (модуль 1).
//
// Схемы кешируются по адресу типа; Load() можно вызывать из нескольких потоков.

#pragma once

#include "memrefl/data_resolver.hpp"
#include "memrefl/memory_reader.hpp"
#include "memrefl/property_schema.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace memrefl {

enum class PropertyListKind {
  LinkedList,    // тип хранит указатель на первый дескриптор, дескриптор — на следующий
  PointerArray,  // тип хранит {Descriptor** data; count}
};

struct ReflectionLayout {
  std::size_t pointer_size = sizeof(void*);

  // Объект -> его тип (указатель на объект-тип по этому смещению).
  std::size_t object_type_offset = 0;

  // Объект-тип.
  std::size_t type_name_offset = 0;  // ID имени типа
  std::size_t type_name_size = 4;
  std::optional<std::size_t> super_offset;  // указатель на родительский тип
  PropertyListKind list_kind = PropertyListKind::LinkedList;
  std::size_t list_offset = 0;  // первый дескриптор или массив указателей на дескрипторы
  std::size_t list_count_offset = 0;  // PointerArray: число дескрипторов (в объекте-типе)
  std::size_t list_count_size = 4;

  // Дескриптор свойства.
  std::size_t next_offset = 0;  // LinkedList: следующий дескриптор
  std::size_t property_name_offset = 0;
  std::size_t property_name_size = 4;
  std::size_t property_offset_offset = 0;  // где в дескрипторе смещение поля в объекте
  std::size_t property_offset_size = 4;
  std::size_t property_tag_offset = 0;
  std::size_t property_tag_size = 4;
  std::optional<std::size_t> property_inner_type_offset;  // тип структуры/объекта/элементов

  // Тег -> прототип описания (тип, формат строки, раскладка массива...). Имя,
  // смещение и struct_type_object заполняются из дескриптора.
  std::unordered_map<std::uint64_t, PropertyDesc> tags;
  // Вместо tags, если тип определяется сложнее (например, тег — указатель на
  // "класс свойства" со своим именем). nullopt — тип неизвестен.
  std::function<std::optional<PropertyDesc>(const MemoryReader&, std::uintptr_t descriptor)> classify;

  std::size_t max_properties = 4096;  // на один тип
  std::size_t max_depth = 64;         // длина цепочки родителей
};

class ReflectionLoader {
 public:
  ReflectionLoader(std::shared_ptr<const MemoryReader> reader, std::shared_ptr<const GenericDataResolver> names,
                   ReflectionLayout layout)
      : reader_(std::move(reader)), names_(std::move(names)), layout_(std::move(layout)) {
    if (!reader_) throw std::invalid_argument("ReflectionLoader: reader is null");
    if (!names_) throw std::invalid_argument("ReflectionLoader: name resolver is null");
  }

  // Адрес объекта-типа для объекта.
  Result<std::uintptr_t> TypeObjectOf(std::uintptr_t object) const {
    const std::optional<std::uintptr_t> type =
        reader_->ReadPointer(object + layout_.object_type_offset, layout_.pointer_size);
    if (!type) return Fail(FetchErrorCode::UnreadableMemory, "cannot read type pointer of object", object);
    if (*type == 0) return Fail(FetchErrorCode::NullPointer, "object has no type pointer", object);
    return *type;
  }

  // Схема типа: собственные и унаследованные свойства. Кешируется.
  Result<std::shared_ptr<const TypeSchema>> Load(std::uintptr_t type_object) const {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      const auto it = cache_.find(type_object);
      if (it != cache_.end()) return it->second;
    }
    // Метаданные читаются без блокировки; если два потока построили схему
    // одновременно, в кеше остаётся первая.
    Result<std::shared_ptr<const TypeSchema>> built = Build(type_object);
    if (!built) return built;
    std::lock_guard<std::mutex> lock(mutex_);
    return cache_.emplace(type_object, *built).first->second;
  }

  Result<std::shared_ptr<const TypeSchema>> LoadForObject(std::uintptr_t object) const {
    const Result<std::uintptr_t> type = TypeObjectOf(object);
    if (!type) return type.error();
    return Load(*type);
  }

  // Имя типа ("<unknown:0x..>", если ID не разрешился).
  std::string TypeName(std::uintptr_t type_object) const {
    const std::optional<std::uint64_t> id =
        reader_->ReadUnsigned(type_object + layout_.type_name_offset, layout_.type_name_size);
    if (!id) return "<unreadable type " + Hex(type_object) + ">";
    return names_->Label(*id);
  }

  void ClearCache() {
    std::lock_guard<std::mutex> lock(mutex_);
    cache_.clear();
  }

  const ReflectionLayout& layout() const noexcept { return layout_; }

 private:
  static std::string Hex(std::uintptr_t value) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "0x%llX", static_cast<unsigned long long>(value));
    return buffer;
  }

  static FetchError Fail(FetchErrorCode code, const std::string& what, std::uintptr_t where) {
    return FetchError{code, what + " at " + Hex(where)};
  }

  Result<std::shared_ptr<const TypeSchema>> Build(std::uintptr_t type_object) const {
    // Цепочка тип -> родитель -> ... (цикл в метаданных обрывает цепочку).
    std::vector<std::uintptr_t> chain{type_object};
    if (layout_.super_offset) {
      std::unordered_set<std::uintptr_t> seen{type_object};
      while (chain.size() < layout_.max_depth) {
        const std::optional<std::uintptr_t> super =
            reader_->ReadPointer(chain.back() + *layout_.super_offset, layout_.pointer_size);
        if (!super) return Fail(FetchErrorCode::UnreadableMemory, "cannot read super type pointer", chain.back());
        if (*super == 0 || !seen.insert(*super).second) break;
        chain.push_back(*super);
      }
    }

    // Свойства от самого базового типа к потомку.
    std::vector<PropertyDesc> properties;
    std::unordered_map<std::string, std::size_t> index;
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
      const Result<std::vector<std::uintptr_t>> descriptors = Descriptors(*it);
      if (!descriptors) return descriptors.error();
      for (const std::uintptr_t descriptor : *descriptors) {
        Result<PropertyDesc> property = Describe(descriptor);
        if (!property) return property.error();
        const auto [slot, inserted] = index.emplace(property->name, properties.size());
        if (inserted) {
          properties.push_back(std::move(property).value());
        } else {
          properties[slot->second] = std::move(property).value();  // потомок переопределил
        }
      }
    }

    TypeSchema schema(TypeName(type_object));
    for (PropertyDesc& property : properties) schema.Add(std::move(property));
    return std::shared_ptr<const TypeSchema>(std::make_shared<TypeSchema>(std::move(schema)));
  }

  Result<std::vector<std::uintptr_t>> Descriptors(std::uintptr_t type_object) const {
    std::vector<std::uintptr_t> out;
    const std::size_t ps = layout_.pointer_size;
    if (layout_.list_kind == PropertyListKind::LinkedList) {
      std::optional<std::uintptr_t> next = reader_->ReadPointer(type_object + layout_.list_offset, ps);
      std::unordered_set<std::uintptr_t> seen;
      while (next && *next != 0) {
        if (!seen.insert(*next).second) break;  // зацикленный список
        if (out.size() >= layout_.max_properties) {
          return Fail(FetchErrorCode::ValueOutOfRange, "too many properties (max_properties)", type_object);
        }
        out.push_back(*next);
        next = reader_->ReadPointer(*next + layout_.next_offset, ps);
      }
      if (!next) return Fail(FetchErrorCode::UnreadableMemory, "cannot read property list", type_object);
      return out;
    }

    const std::optional<std::uintptr_t> array = reader_->ReadPointer(type_object + layout_.list_offset, ps);
    const std::optional<std::uint64_t> count =
        reader_->ReadUnsigned(type_object + layout_.list_count_offset, layout_.list_count_size);
    if (!array || !count) return Fail(FetchErrorCode::UnreadableMemory, "cannot read property array", type_object);
    if (*count > layout_.max_properties) {
      return Fail(FetchErrorCode::ValueOutOfRange, "property count exceeds max_properties", type_object);
    }
    if (*count != 0 && *array == 0) return Fail(FetchErrorCode::NullPointer, "property array is null", type_object);
    for (std::uint64_t i = 0; i < *count; ++i) {
      const std::optional<std::uintptr_t> descriptor =
          reader_->ReadPointer(*array + static_cast<std::uintptr_t>(i) * ps, ps);
      if (!descriptor) return Fail(FetchErrorCode::UnreadableMemory, "cannot read property array", *array);
      if (*descriptor != 0) out.push_back(*descriptor);
    }
    return out;
  }

  Result<PropertyDesc> Describe(std::uintptr_t descriptor) const {
    PropertyDesc property;
    if (layout_.classify) {
      std::optional<PropertyDesc> known = layout_.classify(*reader_, descriptor);
      property = known ? std::move(*known) : PropertyDesc{};
      if (!known) property.type = PropertyType::Unknown;
    } else {
      const std::optional<std::uint64_t> tag =
          reader_->ReadUnsigned(descriptor + layout_.property_tag_offset, layout_.property_tag_size);
      if (!tag) return Fail(FetchErrorCode::UnreadableMemory, "cannot read property tag", descriptor);
      const auto it = layout_.tags.find(*tag);
      if (it != layout_.tags.end()) {
        property = it->second;
      } else {
        property.type = PropertyType::Unknown;
      }
    }

    const std::optional<std::uint64_t> name_id =
        reader_->ReadUnsigned(descriptor + layout_.property_name_offset, layout_.property_name_size);
    const std::optional<std::uint64_t> offset =
        reader_->ReadUnsigned(descriptor + layout_.property_offset_offset, layout_.property_offset_size);
    if (!name_id || !offset) return Fail(FetchErrorCode::UnreadableMemory, "cannot read property descriptor", descriptor);
    property.name = names_->Label(*name_id);
    property.offset = static_cast<std::size_t>(*offset);
    if (layout_.property_inner_type_offset && property.struct_type_object == 0) {
      property.struct_type_object =
          reader_->ReadPointer(descriptor + *layout_.property_inner_type_offset, layout_.pointer_size).value_or(0);
    }
    return property;
  }

  std::shared_ptr<const MemoryReader> reader_;
  std::shared_ptr<const GenericDataResolver> names_;
  ReflectionLayout layout_;
  mutable std::mutex mutex_;
  mutable std::unordered_map<std::uintptr_t, std::shared_ptr<const TypeSchema>> cache_;
};

}  // namespace memrefl

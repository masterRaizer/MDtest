// Проверка сборкой: заголовки memrefl не ломаются макросами, которые
// <windows.h> определяет под MSVC (min/max без NOMINMAX, small из rpcndr.h,
// near/far, interface...). Если этот файл собрался — проверка пройдена.
//
// На Windows подключается настоящий <windows.h> раньше memrefl, как в
// реальной программе (под MSVC NOMINMAX по умолчанию не определён; libstdc++
// в MinGW определяет его сам). На других ОС те же макросы определяются
// вручную — после стандартных заголовков, как это происходит в MSVC, где
// стандартная библиотека защищена от них.

#if defined(_WIN32)
#include <windows.h>
#else
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <link.h>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <sys/uio.h>
#include <type_traits>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#define min(a, b) (((a) < (b)) ? (a) : (b))
#define max(a, b) (((a) > (b)) ? (a) : (b))
#define small char
#define near
#define far
#define interface struct
#define IN
#define OUT
#define OPTIONAL
#endif

#include "memrefl/data_resolver.hpp"
#include "memrefl/memory_reader.hpp"
#include "memrefl/module_regions.hpp"
#include "memrefl/name_table.hpp"
#include "memrefl/pattern.hpp"
#include "memrefl/property_fetcher.hpp"
#include "memrefl/property_schema.hpp"
#include "memrefl/reflection_loader.hpp"
#include "memrefl/signature_scanner.hpp"
#include "memrefl/text.hpp"

// Шаблоны, которые разворачиваются только при использовании.
int main() {
  const memrefl::PropertyValue value = std::int64_t{42};
  return memrefl::ConvertValue<int>(value).value_or(0) == 42 ? 0 : 1;
}

// memrefl — кодировки и хеши строк.
//
// Внутреннее представление всех имён — корректный UTF-8 (std::string). На
// входе бывают:
//   * UTF-8 / ASCII  — проверяется; битые последовательности заменяются на
//                      U+FFFD (по правилу "максимальной части" из стандарта
//                      Unicode, как в браузерах и ICU);
//   * UTF-16LE       — wchar_t-строки Windows; суррогатные пары собираются,
//                      одиночные суррогаты заменяются на U+FFFD;
//   * Latin-1        — "ANSI"-строки, где байт = кодовая точка U+0000..U+00FF.
// Так в вывод аналитики (CSV, JSON, консоль) никогда не попадает невалидный
// UTF-8, даже если в памяти лежал мусор.
//
// Хеши (FNV-1, FNV-1a, CRC32) нужны, когда ID объекта — это хеш его имени:
// по списку известных имён можно построить обратный словарь (DictionaryNameTable).

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace memrefl {

enum class Encoding {
  Utf8,     // также ASCII
  Utf16LE,  // wchar_t на Windows, char16_t
  Latin1,   // ISO-8859-1: байт = кодовая точка
};

inline constexpr std::size_t CodeUnitSize(Encoding encoding) noexcept {
  return encoding == Encoding::Utf16LE ? 2 : 1;
}

inline constexpr char32_t kReplacementChar = 0xFFFD;

// --- Кодовые точки ---------------------------------------------------------------

// Дописывает кодовую точку в UTF-8. Суррогаты и значения > U+10FFFF
// заменяются на U+FFFD.
inline void AppendUtf8(std::string& out, char32_t cp) {
  if ((cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) cp = kReplacementChar;
  if (cp < 0x80) {
    out += static_cast<char>(cp);
  } else if (cp < 0x800) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
}

struct DecodedChar {
  char32_t code_point;  // U+FFFD для некорректной последовательности
  std::size_t length;   // сколько единиц кодирования съедено (>= 1)
  bool valid;
};

// Одна кодовая точка из UTF-8 (n >= 1). Допустимые диапазоны байтов — по
// таблице 3-7 стандарта Unicode: без overlong-форм, суррогатов и > U+10FFFF.
// Для ошибки length — длина "максимальной части": начало корректной
// последовательности, оборвавшейся на неподходящем байте.
inline DecodedChar DecodeUtf8(const unsigned char* p, std::size_t n) noexcept {
  const unsigned char lead = p[0];
  if (lead < 0x80) return {lead, 1, true};
  std::size_t length;
  char32_t cp;
  unsigned char lo = 0x80, hi = 0xBF;  // допустимый диапазон второго байта
  if (lead >= 0xC2 && lead <= 0xDF) {
    length = 2;
    cp = lead & 0x1F;
  } else if (lead >= 0xE0 && lead <= 0xEF) {
    length = 3;
    cp = lead & 0x0F;
    if (lead == 0xE0) lo = 0xA0;       // иначе overlong
    else if (lead == 0xED) hi = 0x9F;  // иначе суррогат
  } else if (lead >= 0xF0 && lead <= 0xF4) {
    length = 4;
    cp = lead & 0x07;
    if (lead == 0xF0) lo = 0x90;       // иначе overlong
    else if (lead == 0xF4) hi = 0x8F;  // иначе > U+10FFFF
  } else {
    return {kReplacementChar, 1, false};
  }
  for (std::size_t k = 1; k < length; ++k) {
    if (k >= n || p[k] < lo || p[k] > hi) return {kReplacementChar, k, false};
    cp = (cp << 6) | (p[k] & 0x3F);
    lo = 0x80;
    hi = 0xBF;
  }
  return {cp, length, true};
}

// Одна кодовая точка из UTF-16 (n >= 1).
inline DecodedChar DecodeUtf16(const char16_t* p, std::size_t n) noexcept {
  const char16_t unit = p[0];
  if (unit < 0xD800 || unit > 0xDFFF) return {unit, 1, true};
  if (unit <= 0xDBFF && n >= 2 && p[1] >= 0xDC00 && p[1] <= 0xDFFF) {
    return {0x10000 + ((static_cast<char32_t>(unit) - 0xD800) << 10) + (p[1] - 0xDC00), 2, true};
  }
  return {kReplacementChar, 1, false};  // одиночный суррогат
}

// --- Преобразования ------------------------------------------------------------

inline bool IsValidUtf8(std::string_view s) noexcept {
  const auto* p = reinterpret_cast<const unsigned char*>(s.data());
  for (std::size_t i = 0; i < s.size();) {
    const DecodedChar c = DecodeUtf8(p + i, s.size() - i);
    if (!c.valid) return false;
    i += c.length;
  }
  return true;
}

// Корректный UTF-8: битые последовательности заменены на U+FFFD.
inline std::string SanitizeUtf8(std::string_view s) {
  if (IsValidUtf8(s)) return std::string(s);
  std::string out;
  out.reserve(s.size() + 8);
  const auto* p = reinterpret_cast<const unsigned char*>(s.data());
  for (std::size_t i = 0; i < s.size();) {
    const DecodedChar c = DecodeUtf8(p + i, s.size() - i);
    AppendUtf8(out, c.code_point);
    i += c.length;
  }
  return out;
}

inline std::string Utf16ToUtf8(std::u16string_view s) {
  std::string out;
  out.reserve(s.size());
  for (std::size_t i = 0; i < s.size();) {
    const DecodedChar c = DecodeUtf16(s.data() + i, s.size() - i);
    AppendUtf8(out, c.code_point);
    i += c.length;
  }
  return out;
}

inline std::string Latin1ToUtf8(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (const char ch : s) AppendUtf8(out, static_cast<unsigned char>(ch));
  return out;
}

// Байты строки в заданной кодировке -> корректный UTF-8. Для UTF-16LE
// нечётный последний байт отбрасывается.
inline std::string ToUtf8(const void* data, std::size_t bytes, Encoding encoding) {
  const auto* p = static_cast<const char*>(data);
  switch (encoding) {
    case Encoding::Utf16LE: {
      std::u16string units(bytes / 2, u'\0');
      for (std::size_t i = 0; i < units.size(); ++i) {
        units[i] = static_cast<char16_t>(static_cast<unsigned char>(p[2 * i]) |
                                         static_cast<unsigned char>(p[2 * i + 1]) << 8);
      }
      return Utf16ToUtf8(units);
    }
    case Encoding::Latin1:
      return Latin1ToUtf8(std::string_view(p, bytes));
    case Encoding::Utf8:
    default:
      return SanitizeUtf8(std::string_view(p, bytes));
  }
}

inline std::u16string Utf8ToUtf16(std::string_view s) {
  std::u16string out;
  out.reserve(s.size());
  const auto* p = reinterpret_cast<const unsigned char*>(s.data());
  for (std::size_t i = 0; i < s.size();) {
    const DecodedChar c = DecodeUtf8(p + i, s.size() - i);
    if (c.code_point >= 0x10000) {
      out += static_cast<char16_t>(0xD800 + ((c.code_point - 0x10000) >> 10));
      out += static_cast<char16_t>(0xDC00 + ((c.code_point - 0x10000) & 0x3FF));
    } else {
      out += static_cast<char16_t>(c.code_point);
    }
    i += c.length;
  }
  return out;
}

// Для Windows API (wchar_t = UTF-16); на других платформах wchar_t = UTF-32.
inline std::wstring Utf8ToWide(std::string_view s) {
  std::wstring out;
  if constexpr (sizeof(wchar_t) == 2) {
    const std::u16string utf16 = Utf8ToUtf16(s);
    out.assign(utf16.begin(), utf16.end());
  } else {
    const auto* p = reinterpret_cast<const unsigned char*>(s.data());
    for (std::size_t i = 0; i < s.size();) {
      const DecodedChar c = DecodeUtf8(p + i, s.size() - i);
      out += static_cast<wchar_t>(c.code_point);
      i += c.length;
    }
  }
  return out;
}

// --- Хеши ------------------------------------------------------------------------

enum class HashAlgorithm {
  Fnv1_32,
  Fnv1a32,
  Fnv1a64,
  Crc32,  // IEEE 802.3 (zlib, PNG): полином 0xEDB88320, init/xorout 0xFFFFFFFF
};

namespace detail {

inline constexpr std::array<std::uint32_t, 256> kCrc32Table = [] {
  std::array<std::uint32_t, 256> table{};
  for (std::uint32_t i = 0; i < 256; ++i) {
    std::uint32_t c = i;
    for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    table[i] = c;
  }
  return table;
}();

}  // namespace detail

inline std::uint64_t HashBytes(HashAlgorithm algorithm, const void* data, std::size_t size) noexcept {
  const auto* p = static_cast<const unsigned char*>(data);
  switch (algorithm) {
    case HashAlgorithm::Fnv1_32: {
      std::uint32_t h = 0x811C9DC5u;
      for (std::size_t i = 0; i < size; ++i) h = (h * 0x01000193u) ^ p[i];
      return h;
    }
    case HashAlgorithm::Fnv1a32: {
      std::uint32_t h = 0x811C9DC5u;
      for (std::size_t i = 0; i < size; ++i) h = (h ^ p[i]) * 0x01000193u;
      return h;
    }
    case HashAlgorithm::Fnv1a64: {
      std::uint64_t h = 0xCBF29CE484222325ull;
      for (std::size_t i = 0; i < size; ++i) h = (h ^ p[i]) * 0x100000001B3ull;
      return h;
    }
    case HashAlgorithm::Crc32:
    default: {
      std::uint32_t c = 0xFFFFFFFFu;
      for (std::size_t i = 0; i < size; ++i) c = detail::kCrc32Table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
      return c ^ 0xFFFFFFFFu;
    }
  }
}

// Как программа хеширует имя: алгоритм, в какой кодировке берутся байты
// строки и приводится ли ASCII к нижнему регистру ("Player" == "player").
struct StringHashOptions {
  HashAlgorithm algorithm = HashAlgorithm::Fnv1a32;
  Encoding input = Encoding::Utf8;
  bool ascii_lowercase = false;
};

// Хеш имени, заданного в UTF-8, как его посчитала бы программа.
inline std::uint64_t HashString(std::string_view utf8, const StringHashOptions& options = {}) {
  std::string normalized = SanitizeUtf8(utf8);
  if (options.ascii_lowercase) {
    for (char& ch : normalized) {
      if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    }
  }
  std::string bytes;
  switch (options.input) {
    case Encoding::Utf16LE:
      for (const char16_t unit : Utf8ToUtf16(normalized)) {
        bytes += static_cast<char>(unit & 0xFF);
        bytes += static_cast<char>(unit >> 8);
      }
      break;
    case Encoding::Latin1: {
      const auto* p = reinterpret_cast<const unsigned char*>(normalized.data());
      for (std::size_t i = 0; i < normalized.size();) {
        const DecodedChar c = DecodeUtf8(p + i, normalized.size() - i);
        bytes += static_cast<char>(c.code_point <= 0xFF ? c.code_point : U'?');
        i += c.length;
      }
      break;
    }
    case Encoding::Utf8:
    default:
      bytes = std::move(normalized);
      break;
  }
  return HashBytes(options.algorithm, bytes.data(), bytes.size());
}

}  // namespace memrefl

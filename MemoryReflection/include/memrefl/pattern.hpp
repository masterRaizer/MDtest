// memrefl — модуль 1: сигнатура (паттерн байтов с маской).
//
// Сигнатура описывает участок машинного кода, который переживает обновления
// бинарника: адреса, смещения полей и относительные переходы в нём заменены
// масками, а неизменная "форма" инструкций оставлена как есть.
//
// Текстовый формат (как в IDA / x64dbg / Cheat Engine):
//   "48 89 F8 ?? 0F 1F"   байты в hex через пробел, регистр не важен
//   "?" или "??"          любой байт
//   "4?" / "?F"           полубайтовая маска: проверяется только одна тетрада
//   "4889F8??0F1F"        слитная запись (разбирается парами символов)
//
// Внутри каждый байт хранится парой (value, mask): байт памяти m совпадает,
// если (m & mask) == value. Обычный байт — mask 0xFF, "??" — mask 0x00.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace memrefl {

class Pattern {
 public:
  static constexpr std::size_t npos = static_cast<std::size_t>(-1);

  // Разбирает текстовую сигнатуру. Бросает std::invalid_argument с позицией
  // ошибки — удобно для сигнатур, зашитых в код (ошибка видна сразу).
  explicit Pattern(std::string_view text) {
    std::string error;
    if (!Parse(text, error)) throw std::invalid_argument(error);
  }

  // То же без исключений — для сигнатур из конфигов и пользовательского ввода.
  static std::optional<Pattern> TryParse(std::string_view text, std::string* error = nullptr) {
    Pattern pattern;
    std::string message;
    if (!pattern.Parse(text, message)) {
      if (error) *error = std::move(message);
      return std::nullopt;
    }
    return pattern;
  }

  // Классический "code-style" формат: bytes = "\x48\x8B\x05\x00\x00\x00\x00",
  // mask = "xxx????" ('x' — байт проверяется, '?' — любой). Длина паттерна
  // равна длине маски. Бросает std::invalid_argument при ошибке.
  static Pattern FromCodeStyle(const void* bytes, std::string_view mask) {
    Pattern pattern;
    const auto* src = static_cast<const std::uint8_t*>(bytes);
    for (std::size_t i = 0; i < mask.size(); ++i) {
      if (mask[i] == 'x' || mask[i] == 'X') {
        pattern.Push(src[i], 0xFF);
      } else if (mask[i] == '?') {
        pattern.Push(0, 0);
      } else {
        throw std::invalid_argument("bad code-style mask character '" + std::string(1, mask[i]) +
                                    "' at offset " + std::to_string(i) + ": expected 'x' or '?'");
      }
    }
    std::string error;
    if (!pattern.Finalize(error)) throw std::invalid_argument(error);
    return pattern;
  }

  std::size_t size() const noexcept { return values_.size(); }
  const std::uint8_t* values() const noexcept { return values_.data(); }
  const std::uint8_t* masks() const noexcept { return masks_.data(); }

  // Индексы двух самых редких полностью заданных байтов ("якорей"); npos,
  // если таких байтов меньше. Сканер сначала ищет позиции, где совпали якоря,
  // и только их проверяет целиком. Выбор якорей влияет на скорость, но не на
  // результат поиска.
  std::size_t anchor() const noexcept { return anchor_; }
  std::size_t second_anchor() const noexcept { return second_anchor_; }

  // Совпадает ли паттерн с памятью, начиная с p. Должно быть доступно size() байт.
  bool MatchesAt(const std::uint8_t* p) const noexcept {
    const std::size_t n = values_.size();
    for (std::size_t i = 0; i < n; ++i) {
      if ((p[i] & masks_[i]) != values_[i]) return false;
    }
    return true;
  }

  // Обратно в текст ("48 8B ?? 4?") — для логов и сообщений.
  std::string ToString() const {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(values_.size() * 3);
    for (std::size_t i = 0; i < values_.size(); ++i) {
      if (i) out += ' ';
      out += (masks_[i] & 0xF0) ? kHex[values_[i] >> 4] : '?';
      out += (masks_[i] & 0x0F) ? kHex[values_[i] & 0x0F] : '?';
    }
    return out;
  }

 private:
  Pattern() = default;

  void Push(std::uint8_t value, std::uint8_t mask) {
    values_.push_back(static_cast<std::uint8_t>(value & mask));
    masks_.push_back(mask);
  }

  // Грубая оценка частоты байта в x86/x86-64 коде: 2 — очень частый,
  // 1 — частый, 0 — остальные. Редкий якорь даёт меньше ложных кандидатов.
  static int Commonness(std::uint8_t b) noexcept {
    switch (b) {
      case 0x00: case 0xFF: case 0xCC: case 0x90:  // нули/-1 в операндах, выравнивание int3/nop
      case 0x48: case 0x8B: case 0x89: case 0x0F:  // REX.W, mov, префикс двухбайтовых опкодов
        return 2;
      case 0x40: case 0x41: case 0x44: case 0x45: case 0x49: case 0x4C: case 0x4D:  // прочие REX
      case 0x24: case 0x83: case 0x85: case 0x8D: case 0xC0: case 0xC3: case 0xC7:
      case 0xE8: case 0xE9: case 0xEB: case 0x74: case 0x75: case 0x33:
      case 0x01: case 0x08: case 0x10: case 0x18: case 0x20: case 0x28: case 0x30: case 0x38:
        return 1;
      default:
        return 0;
    }
  }

  // Символ тетрады: hex-цифра (mask 0xF) или '?' (mask 0).
  static bool ParseNibble(char c, std::uint8_t& value, std::uint8_t& mask) noexcept {
    mask = 0xF;
    if (c >= '0' && c <= '9') {
      value = static_cast<std::uint8_t>(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      value = static_cast<std::uint8_t>(c - 'a' + 10);
    } else if (c >= 'A' && c <= 'F') {
      value = static_cast<std::uint8_t>(c - 'A' + 10);
    } else if (c == '?') {
      value = 0;
      mask = 0;
    } else {
      return false;
    }
    return true;
  }

  static bool IsSpace(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
  }

  bool Parse(std::string_view text, std::string& error) {
    std::size_t i = 0;
    while (i < text.size()) {
      if (IsSpace(text[i])) {
        ++i;
        continue;
      }
      const std::size_t start = i;
      while (i < text.size() && !IsSpace(text[i])) ++i;
      const std::string_view token = text.substr(start, i - start);

      if (token == "?") {
        Push(0, 0);
        continue;
      }
      bool ok = token.size() % 2 == 0;
      for (std::size_t k = 0; ok && k < token.size(); k += 2) {
        std::uint8_t hi, hi_mask, lo, lo_mask;
        ok = ParseNibble(token[k], hi, hi_mask) && ParseNibble(token[k + 1], lo, lo_mask);
        if (ok) {
          Push(static_cast<std::uint8_t>(hi << 4 | lo), static_cast<std::uint8_t>(hi_mask << 4 | lo_mask));
        }
      }
      if (!ok) {
        error = "bad signature token \"" + std::string(token) + "\" at offset " +
                std::to_string(start) + ": expected hex byte pairs (8B), nibble masks (4?) or wildcards (?)";
        return false;
      }
    }
    return Finalize(error);
  }

  // Проверяет, что паттерн что-то ограничивает, и выбирает якоря.
  bool Finalize(std::string& error) {
    if (values_.empty()) {
      error = "empty signature";
      return false;
    }
    bool has_fixed_bits = false;
    for (std::size_t i = 0; i < values_.size(); ++i) {
      if (masks_[i] != 0) has_fixed_bits = true;
      if (masks_[i] != 0xFF) continue;
      // При равной частоте выигрывает более ранний байт (строгое "<").
      if (anchor_ == npos || Commonness(values_[i]) < Commonness(values_[anchor_])) {
        second_anchor_ = anchor_;
        anchor_ = i;
      } else if (second_anchor_ == npos ||
                 Commonness(values_[i]) < Commonness(values_[second_anchor_])) {
        second_anchor_ = i;
      }
    }
    if (!has_fixed_bits) {
      error = "signature has no fixed bits (only wildcards)";
      return false;
    }
    return true;
  }

  std::vector<std::uint8_t> values_;  // уже с наложенной маской: value & mask
  std::vector<std::uint8_t> masks_;
  std::size_t anchor_ = npos;
  std::size_t second_anchor_ = npos;
};

}  // namespace memrefl

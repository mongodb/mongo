#ifndef ABSL_STRINGS_INTERNAL_STR_FORMAT_PARSER_H_
#define ABSL_STRINGS_INTERNAL_STR_FORMAT_PARSER_H_

#include <limits.h>
#include <stddef.h>
#include <stdlib.h>

#include <cassert>
#include <initializer_list>
#include <iosfwd>
#include <iterator>
#include <memory>
#include <vector>

#include "absl/strings/internal/str_format/checker.h"
#include "absl/strings/internal/str_format/extension.h"

namespace absl {
namespace str_format_internal {

// The analyzed properties of a single specified conversion.
struct UnboundConversion {
  UnboundConversion()
      : flags() /* This is required to zero all the fields of flags. */ {
    flags.basic = true;
  }

  class InputValue {
   public:
    void set_value(int value) {
      assert(value >= 0);
      value_ = value;
    }
    int value() const { return value_; }

    // Marks the value as "from arg". aka the '*' format.
    // Requires `value >= 1`.
    // When set, is_from_arg() return true and get_from_arg() returns the
    // original value.
    // `value()`'s return value is unspecfied in this state.
    void set_from_arg(int value) {
      assert(value > 0);
      value_ = -value - 1;
    }
    bool is_from_arg() const { return value_ < -1; }
    int get_from_arg() const {
      assert(is_from_arg());
      return -value_ - 1;
    }

   private:
    int value_ = -1;
  };

  // No need to initialize. It will always be set in the parser.
  int arg_position;

  InputValue width;
  InputValue precision;

  Flags flags;
  LengthMod length_mod;
  ConversionChar conv;
};

// Consume conversion spec prefix (not including '%') of '*src' if valid.
// Examples of valid specs would be e.g.: "s", "d", "-12.6f".
// If valid, the front of src is advanced such that src becomes the
// part following the conversion spec, and the spec part is broken down and
// returned in 'conv'.
// If invalid, returns false and leaves 'src' unmodified.
// For example:
//   Given "d9", returns "d", and leaves src="9",
//   Given "!", returns "" and leaves src="!".
bool ConsumeUnboundConversion(string_view* src, UnboundConversion* conv,
                              int* next_arg);

// Parse the format string provided in 'src' and pass the identified items into
// 'consumer'.
// Text runs will be passed by calling
//   Consumer::Append(string_view);
// ConversionItems will be passed by calling
//   Consumer::ConvertOne(UnboundConversion, string_view);
// In the case of ConvertOne, the string_view that is passed is the
// portion of the format string corresponding to the conversion, not including
// the leading %. On success, it returns true. On failure, it stops and returns
// false.
template <typename Consumer>
bool ParseFormatString(string_view src, Consumer consumer) {
  int next_arg = 0;
  while (!src.empty()) {
    const char* percent =
        static_cast<const char*>(memchr(src.data(), '%', src.size()));
    if (!percent) {
      // We found the last substring.
      return consumer.Append(src);
    }
    // We found a percent, so push the text run then process the percent.
    size_t percent_loc = percent - src.data();
    if (!consumer.Append(string_view(src.data(), percent_loc))) return false;
    if (percent + 1 >= src.data() + src.size()) return false;

    UnboundConversion conv;

    switch (percent[1]) {
      case '%':
        if (!consumer.Append("%")) return false;
        src.remove_prefix(percent_loc + 2);
        continue;

#define PARSER_CASE(ch)                                     \
  case #ch[0]:                                              \
    src.remove_prefix(percent_loc + 2);                     \
    conv.conv = ConversionChar::FromId(ConversionChar::ch); \
    conv.arg_position = ++next_arg;                         \
    break;
        ABSL_CONVERSION_CHARS_EXPAND_(PARSER_CASE, );
#undef PARSER_CASE

      default:
        src.remove_prefix(percent_loc + 1);
        if (!ConsumeUnboundConversion(&src, &conv, &next_arg)) return false;
        break;
    }
    if (next_arg == 0) {
      // This indicates an error in the format std::string.
      // The only way to get next_arg == 0 is to have a positional argument
      // first which sets next_arg to -1 and then a non-positional argument
      // which does ++next_arg.
      // Checking here seems to be the cheapeast place to do it.
      return false;
    }
    if (!consumer.ConvertOne(
            conv, string_view(percent + 1, src.data() - (percent + 1)))) {
      return false;
    }
  }
  return true;
}

// Always returns true, or fails to compile in a constexpr context if s does not
// point to a constexpr char array.
constexpr bool EnsureConstexpr(string_view s) {
  return s.empty() || s[0] == s[0];
}

class ParsedFormatBase {
 public:
  explicit ParsedFormatBase(string_view format, bool allow_ignored,
                            std::initializer_list<Conv> convs);

  ParsedFormatBase(const ParsedFormatBase& other) { *this = other; }

  ParsedFormatBase(ParsedFormatBase&& other) { *this = std::move(other); }

  ParsedFormatBase& operator=(const ParsedFormatBase& other) {
    if (this == &other) return *this;
    has_error_ = other.has_error_;
    items_ = other.items_;
    size_t text_size = items_.empty() ? 0 : items_.back().text_end;
    data_.reset(new char[text_size]);
    memcpy(data_.get(), other.data_.get(), text_size);
    return *this;
  }

  ParsedFormatBase& operator=(ParsedFormatBase&& other) {
    if (this == &other) return *this;
    has_error_ = other.has_error_;
    data_ = std::move(other.data_);
    items_ = std::move(other.items_);
    // Reset the vector to make sure the invariants hold.
    other.items_.clear();
    return *this;
  }

  template <typename Consumer>
  bool ProcessFormat(Consumer consumer) const {
    const char* const base = data_.get();
    string_view text(base, 0);
    for (const auto& item : items_) {
      const char* const end = text.data() + text.size();
      text = string_view(end, (base + item.text_end) - end);
      if (item.is_conversion) {
        if (!consumer.ConvertOne(item.conv, text)) return false;
      } else {
        if (!consumer.Append(text)) return false;
      }
    }
    return !has_error_;
  }

  bool has_error() const { return has_error_; }

 private:
  // Returns whether the conversions match and if !allow_ignored it verifies
  // that all conversions are used by the format.
  bool MatchesConversions(bool allow_ignored,
                          std::initializer_list<Conv> convs) const;

  struct ParsedFormatConsumer;

  struct ConversionItem {
    bool is_conversion;
    // Points to the past-the-end location of this element in the data_ array.
    size_t text_end;
    UnboundConversion conv;
  };

  bool has_error_;
  std::unique_ptr<char[]> data_;
  std::vector<ConversionItem> items_;
};


// A value type representing a preparsed format.  These can be created, copied
// around, and reused to speed up formatting loops.
// The user must specify through the template arguments the conversion
// characters used in the format. This will be checked at compile time.
//
// This class uses Conv enum values to specify each argument.
// This allows for more flexibility as you can specify multiple possible
// conversion characters for each argument.
// ParsedFormat<char...> is a simplified alias for when the user only
// needs to specify a single conversion character for each argument.
//
// Example:
//   // Extended format supports multiple characters per argument:
//   using MyFormat = ExtendedParsedFormat<Conv::d | Conv::x>;
//   MyFormat GetFormat(bool use_hex) {
//     if (use_hex) return MyFormat("foo %x bar");
//     return MyFormat("foo %d bar");
//   }
//   // 'format' can be used with any value that supports 'd' and 'x',
//   // like `int`.
//   auto format = GetFormat(use_hex);
//   value = StringF(format, i);
//
// This class also supports runtime format checking with the ::New() and
// ::NewAllowIgnored() factory functions.
// This is the only API that allows the user to pass a runtime specified format
// string. These factory functions will return NULL if the format does not match
// the conversions requested by the user.
template <str_format_internal::Conv... C>
class ExtendedParsedFormat : public str_format_internal::ParsedFormatBase {
 public:
  explicit ExtendedParsedFormat(string_view format)
#if ABSL_INTERNAL_ENABLE_FORMAT_CHECKER
      __attribute__((
          enable_if(str_format_internal::EnsureConstexpr(format),
                    "Format std::string is not constexpr."),
          enable_if(str_format_internal::ValidFormatImpl<C...>(format),
                    "Format specified does not match the template arguments.")))
#endif  // ABSL_INTERNAL_ENABLE_FORMAT_CHECKER
      : ExtendedParsedFormat(format, false) {
  }

  // ExtendedParsedFormat factory function.
  // The user still has to specify the conversion characters, but they will not
  // be checked at compile time. Instead, it will be checked at runtime.
  // This delays the checking to runtime, but allows the user to pass
  // dynamically sourced formats.
  // It returns NULL if the format does not match the conversion characters.
  // The user is responsible for checking the return value before using it.
  //
  // The 'New' variant will check that all the specified arguments are being
  // consumed by the format and return NULL if any argument is being ignored.
  // The 'NewAllowIgnored' variant will not verify this and will allow formats
  // that ignore arguments.
  static std::unique_ptr<ExtendedParsedFormat> New(string_view format) {
    return New(format, false);
  }
  static std::unique_ptr<ExtendedParsedFormat> NewAllowIgnored(
      string_view format) {
    return New(format, true);
  }

 private:
  static std::unique_ptr<ExtendedParsedFormat> New(string_view format,
                                                   bool allow_ignored) {
    std::unique_ptr<ExtendedParsedFormat> conv(
        new ExtendedParsedFormat(format, allow_ignored));
    if (conv->has_error()) return nullptr;
    return conv;
  }

  ExtendedParsedFormat(string_view s, bool allow_ignored)
      : ParsedFormatBase(s, allow_ignored, {C...}) {}
};
}  // namespace str_format_internal
}  // namespace absl

#endif  // ABSL_STRINGS_INTERNAL_STR_FORMAT_PARSER_H_

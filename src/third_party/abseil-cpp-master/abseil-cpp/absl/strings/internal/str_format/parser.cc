#include "absl/strings/internal/str_format/parser.h"

#include <assert.h>
#include <string.h>
#include <wchar.h>
#include <cctype>
#include <cstdint>

#include <algorithm>
#include <initializer_list>
#include <limits>
#include <ostream>
#include <string>
#include <unordered_set>

namespace absl {
namespace str_format_internal {
namespace {

bool CheckFastPathSetting(const UnboundConversion& conv) {
  bool should_be_basic = !conv.flags.left &&      //
                         !conv.flags.show_pos &&  //
                         !conv.flags.sign_col &&  //
                         !conv.flags.alt &&       //
                         !conv.flags.zero &&      //
                         (conv.width.value() == -1) &&
                         (conv.precision.value() == -1);
  if (should_be_basic != conv.flags.basic) {
    fprintf(stderr,
            "basic=%d left=%d show_pos=%d sign_col=%d alt=%d zero=%d "
            "width=%d precision=%d\n",
            conv.flags.basic, conv.flags.left, conv.flags.show_pos,
            conv.flags.sign_col, conv.flags.alt, conv.flags.zero,
            conv.width.value(), conv.precision.value());
  }
  return should_be_basic == conv.flags.basic;
}

// Keep a single table for all the conversion chars and length modifiers.
// We invert the length modifiers to make them negative so that we can easily
// test for them.
// Everything else is `none`, which is a negative constant.
using CC = ConversionChar::Id;
using LM = LengthMod::Id;
static constexpr std::int8_t none = -128;
static constexpr std::int8_t kIds[] = {
    none,   none,   none,   none,  none,   none,  none,  none,   // 00-07
    none,   none,   none,   none,  none,   none,  none,  none,   // 08-0f
    none,   none,   none,   none,  none,   none,  none,  none,   // 10-17
    none,   none,   none,   none,  none,   none,  none,  none,   // 18-1f
    none,   none,   none,   none,  none,   none,  none,  none,   // 20-27
    none,   none,   none,   none,  none,   none,  none,  none,   // 28-2f
    none,   none,   none,   none,  none,   none,  none,  none,   // 30-37
    none,   none,   none,   none,  none,   none,  none,  none,   // 38-3f
    none,   CC::A,  none,   CC::C, none,   CC::E, CC::F, CC::G,  // @ABCDEFG
    none,   none,   none,   none,  ~LM::L, none,  none,  none,   // HIJKLMNO
    none,   none,   none,   CC::S, none,   none,  none,  none,   // PQRSTUVW
    CC::X,  none,   none,   none,  none,   none,  none,  none,   // XYZ[\]^_
    none,   CC::a,  none,   CC::c, CC::d,  CC::e, CC::f, CC::g,  // `abcdefg
    ~LM::h, CC::i,  ~LM::j, none,  ~LM::l, none,  CC::n, CC::o,  // hijklmno
    CC::p,  ~LM::q, none,   CC::s, ~LM::t, CC::u, none,  none,   // pqrstuvw
    CC::x,  none,   ~LM::z, none,  none,   none,  none,  none,   // xyz{|}~!
    none,   none,   none,   none,  none,   none,  none,  none,   // 80-87
    none,   none,   none,   none,  none,   none,  none,  none,   // 88-8f
    none,   none,   none,   none,  none,   none,  none,  none,   // 90-97
    none,   none,   none,   none,  none,   none,  none,  none,   // 98-9f
    none,   none,   none,   none,  none,   none,  none,  none,   // a0-a7
    none,   none,   none,   none,  none,   none,  none,  none,   // a8-af
    none,   none,   none,   none,  none,   none,  none,  none,   // b0-b7
    none,   none,   none,   none,  none,   none,  none,  none,   // b8-bf
    none,   none,   none,   none,  none,   none,  none,  none,   // c0-c7
    none,   none,   none,   none,  none,   none,  none,  none,   // c8-cf
    none,   none,   none,   none,  none,   none,  none,  none,   // d0-d7
    none,   none,   none,   none,  none,   none,  none,  none,   // d8-df
    none,   none,   none,   none,  none,   none,  none,  none,   // e0-e7
    none,   none,   none,   none,  none,   none,  none,  none,   // e8-ef
    none,   none,   none,   none,  none,   none,  none,  none,   // f0-f7
    none,   none,   none,   none,  none,   none,  none,  none,   // f8-ff
};

template <bool is_positional>
bool ConsumeConversion(string_view *src, UnboundConversion *conv,
                       int *next_arg) {
  const char *pos = src->data();
  const char *const end = pos + src->size();
  char c;
  // Read the next char into `c` and update `pos`. Returns false if there are
  // no more chars to read.
#define ABSL_FORMAT_PARSER_INTERNAL_GET_CHAR()        \
  do {                                                \
    if (ABSL_PREDICT_FALSE(pos == end)) return false; \
    c = *pos++;                                       \
  } while (0)

  const auto parse_digits = [&] {
    int digits = c - '0';
    // We do not want to overflow `digits` so we consume at most digits10
    // digits. If there are more digits the parsing will fail later on when the
    // digit doesn't match the expected characters.
    int num_digits = std::numeric_limits<int>::digits10;
    for (;;) {
      if (ABSL_PREDICT_FALSE(pos == end || !num_digits)) break;
      c = *pos++;
      if (!std::isdigit(c)) break;
      --num_digits;
      digits = 10 * digits + c - '0';
    }
    return digits;
  };

  if (is_positional) {
    ABSL_FORMAT_PARSER_INTERNAL_GET_CHAR();
    if (ABSL_PREDICT_FALSE(c < '1' || c > '9')) return false;
    conv->arg_position = parse_digits();
    assert(conv->arg_position > 0);
    if (ABSL_PREDICT_FALSE(c != '$')) return false;
  }

  ABSL_FORMAT_PARSER_INTERNAL_GET_CHAR();

  // We should start with the basic flag on.
  assert(conv->flags.basic);

  // Any non alpha character makes this conversion not basic.
  // This includes flags (-+ #0), width (1-9, *) or precision (.).
  // All conversion characters and length modifiers are alpha characters.
  if (c < 'A') {
    conv->flags.basic = false;

    for (; c <= '0';) {
      // FIXME: We might be able to speed this up reusing the kIds lookup table
      // from above.
      // It might require changing Flags to be a plain integer where we can |= a
      // value.
      switch (c) {
        case '-':
          conv->flags.left = true;
          break;
        case '+':
          conv->flags.show_pos = true;
          break;
        case ' ':
          conv->flags.sign_col = true;
          break;
        case '#':
          conv->flags.alt = true;
          break;
        case '0':
          conv->flags.zero = true;
          break;
        default:
          goto flags_done;
      }
      ABSL_FORMAT_PARSER_INTERNAL_GET_CHAR();
    }
flags_done:

    if (c <= '9') {
      if (c >= '0') {
        int maybe_width = parse_digits();
        if (!is_positional && c == '$') {
          if (ABSL_PREDICT_FALSE(*next_arg != 0)) return false;
          // Positional conversion.
          *next_arg = -1;
          conv->flags = Flags();
          conv->flags.basic = true;
          return ConsumeConversion<true>(src, conv, next_arg);
        }
        conv->width.set_value(maybe_width);
      } else if (c == '*') {
        ABSL_FORMAT_PARSER_INTERNAL_GET_CHAR();
        if (is_positional) {
          if (ABSL_PREDICT_FALSE(c < '1' || c > '9')) return false;
          conv->width.set_from_arg(parse_digits());
          if (ABSL_PREDICT_FALSE(c != '$')) return false;
          ABSL_FORMAT_PARSER_INTERNAL_GET_CHAR();
        } else {
          conv->width.set_from_arg(++*next_arg);
        }
      }
    }

    if (c == '.') {
      ABSL_FORMAT_PARSER_INTERNAL_GET_CHAR();
      if (std::isdigit(c)) {
        conv->precision.set_value(parse_digits());
      } else if (c == '*') {
        ABSL_FORMAT_PARSER_INTERNAL_GET_CHAR();
        if (is_positional) {
          if (ABSL_PREDICT_FALSE(c < '1' || c > '9')) return false;
          conv->precision.set_from_arg(parse_digits());
          if (c != '$') return false;
          ABSL_FORMAT_PARSER_INTERNAL_GET_CHAR();
        } else {
          conv->precision.set_from_arg(++*next_arg);
        }
      } else {
        conv->precision.set_value(0);
      }
    }
  }

  std::int8_t id = kIds[static_cast<unsigned char>(c)];

  if (id < 0) {
    if (ABSL_PREDICT_FALSE(id == none)) return false;

    // It is a length modifier.
    using str_format_internal::LengthMod;
    LengthMod length_mod = LengthMod::FromId(static_cast<LM>(~id));
    ABSL_FORMAT_PARSER_INTERNAL_GET_CHAR();
    if (c == 'h' && length_mod.id() == LengthMod::h) {
      conv->length_mod = LengthMod::FromId(LengthMod::hh);
      ABSL_FORMAT_PARSER_INTERNAL_GET_CHAR();
    } else if (c == 'l' && length_mod.id() == LengthMod::l) {
      conv->length_mod = LengthMod::FromId(LengthMod::ll);
      ABSL_FORMAT_PARSER_INTERNAL_GET_CHAR();
    } else {
      conv->length_mod = length_mod;
    }
    id = kIds[static_cast<unsigned char>(c)];
    if (ABSL_PREDICT_FALSE(id < 0)) return false;
  }

  assert(CheckFastPathSetting(*conv));
  (void)(&CheckFastPathSetting);

  conv->conv = ConversionChar::FromId(static_cast<CC>(id));
  if (!is_positional) conv->arg_position = ++*next_arg;
  *src = string_view(pos, end - pos);
  return true;
}

}  // namespace

bool ConsumeUnboundConversion(string_view *src, UnboundConversion *conv,
                              int *next_arg) {
  if (*next_arg < 0) return ConsumeConversion<true>(src, conv, next_arg);
  return ConsumeConversion<false>(src, conv, next_arg);
}

struct ParsedFormatBase::ParsedFormatConsumer {
  explicit ParsedFormatConsumer(ParsedFormatBase *parsedformat)
      : parsed(parsedformat), data_pos(parsedformat->data_.get()) {}

  bool Append(string_view s) {
    if (s.empty()) return true;

    size_t text_end = AppendText(s);

    if (!parsed->items_.empty() && !parsed->items_.back().is_conversion) {
      // Let's extend the existing text run.
      parsed->items_.back().text_end = text_end;
    } else {
      // Let's make a new text run.
      parsed->items_.push_back({false, text_end, {}});
    }
    return true;
  }

  bool ConvertOne(const UnboundConversion &conv, string_view s) {
    size_t text_end = AppendText(s);
    parsed->items_.push_back({true, text_end, conv});
    return true;
  }

  size_t AppendText(string_view s) {
    memcpy(data_pos, s.data(), s.size());
    data_pos += s.size();
    return static_cast<size_t>(data_pos - parsed->data_.get());
  }

  ParsedFormatBase *parsed;
  char* data_pos;
};

ParsedFormatBase::ParsedFormatBase(string_view format, bool allow_ignored,
                                   std::initializer_list<Conv> convs)
    : data_(format.empty() ? nullptr : new char[format.size()]) {
  has_error_ = !ParseFormatString(format, ParsedFormatConsumer(this)) ||
               !MatchesConversions(allow_ignored, convs);
}

bool ParsedFormatBase::MatchesConversions(
    bool allow_ignored, std::initializer_list<Conv> convs) const {
  std::unordered_set<int> used;
  auto add_if_valid_conv = [&](int pos, char c) {
      if (static_cast<size_t>(pos) > convs.size() ||
          !Contains(convs.begin()[pos - 1], c))
        return false;
      used.insert(pos);
      return true;
  };
  for (const ConversionItem &item : items_) {
    if (!item.is_conversion) continue;
    auto &conv = item.conv;
    if (conv.precision.is_from_arg() &&
        !add_if_valid_conv(conv.precision.get_from_arg(), '*'))
      return false;
    if (conv.width.is_from_arg() &&
        !add_if_valid_conv(conv.width.get_from_arg(), '*'))
      return false;
    if (!add_if_valid_conv(conv.arg_position, conv.conv.Char())) return false;
  }
  return used.size() == convs.size() || allow_ignored;
}

}  // namespace str_format_internal
}  // namespace absl

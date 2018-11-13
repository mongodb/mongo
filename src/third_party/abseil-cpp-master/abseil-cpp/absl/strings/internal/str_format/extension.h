//
// Copyright 2017 The Abseil Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
//
#ifndef ABSL_STRINGS_INTERNAL_STR_FORMAT_EXTENSION_H_
#define ABSL_STRINGS_INTERNAL_STR_FORMAT_EXTENSION_H_

#include <limits.h>
#include <cstddef>
#include <cstring>
#include <ostream>

#include "absl/base/port.h"
#include "absl/strings/internal/str_format/output.h"
#include "absl/strings/string_view.h"

class Cord;

namespace absl {

namespace str_format_internal {

class FormatRawSinkImpl {
 public:
  // Implicitly convert from any type that provides the hook function as
  // described above.
  template <typename T, decltype(str_format_internal::InvokeFlush(
                            std::declval<T*>(), string_view()))* = nullptr>
  FormatRawSinkImpl(T* raw)  // NOLINT
      : sink_(raw), write_(&FormatRawSinkImpl::Flush<T>) {}

  void Write(string_view s) { write_(sink_, s); }

  template <typename T>
  static FormatRawSinkImpl Extract(T s) {
    return s.sink_;
  }

 private:
  template <typename T>
  static void Flush(void* r, string_view s) {
    str_format_internal::InvokeFlush(static_cast<T*>(r), s);
  }

  void* sink_;
  void (*write_)(void*, string_view);
};

// An abstraction to which conversions write their string data.
class FormatSinkImpl {
 public:
  explicit FormatSinkImpl(FormatRawSinkImpl raw) : raw_(raw) {}

  ~FormatSinkImpl() { Flush(); }

  void Flush() {
    raw_.Write(string_view(buf_, pos_ - buf_));
    pos_ = buf_;
  }

  void Append(size_t n, char c) {
    if (n == 0) return;
    size_ += n;
    auto raw_append = [&](size_t count) {
      memset(pos_, c, count);
      pos_ += count;
    };
    while (n > Avail()) {
      n -= Avail();
      if (Avail() > 0) {
        raw_append(Avail());
      }
      Flush();
    }
    raw_append(n);
  }

  void Append(string_view v) {
    size_t n = v.size();
    if (n == 0) return;
    size_ += n;
    if (n >= Avail()) {
      Flush();
      raw_.Write(v);
      return;
    }
    memcpy(pos_, v.data(), n);
    pos_ += n;
  }

  size_t size() const { return size_; }

  // Put 'v' to 'sink' with specified width, precision, and left flag.
  bool PutPaddedString(string_view v, int w, int p, bool l);

  template <typename T>
  T Wrap() {
    return T(this);
  }

  template <typename T>
  static FormatSinkImpl* Extract(T* s) {
    return s->sink_;
  }

 private:
  size_t Avail() const { return buf_ + sizeof(buf_) - pos_; }

  FormatRawSinkImpl raw_;
  size_t size_ = 0;
  char* pos_ = buf_;
  char buf_[1024];
};

struct Flags {
  bool basic : 1;     // fastest conversion: no flags, width, or precision
  bool left : 1;      // "-"
  bool show_pos : 1;  // "+"
  bool sign_col : 1;  // " "
  bool alt : 1;       // "#"
  bool zero : 1;      // "0"
  std::string ToString() const;
  friend std::ostream& operator<<(std::ostream& os, const Flags& v) {
    return os << v.ToString();
  }
};

struct LengthMod {
 public:
  enum Id : uint8_t {
    h, hh, l, ll, L, j, z, t, q, none
  };
  static const size_t kNumValues = none + 1;

  LengthMod() : id_(none) {}

  // Index into the opaque array of LengthMod enums.
  // Requires: i < kNumValues
  static LengthMod FromIndex(size_t i) {
    return LengthMod(kSpecs[i].value);
  }

  static LengthMod FromId(Id id) { return LengthMod(id); }

  // The length modifier std::string associated with a specified LengthMod.
  string_view name() const {
    const Spec& spec = kSpecs[id_];
    return {spec.name, spec.name_length};
  }

  Id id() const { return id_; }

  friend bool operator==(const LengthMod& a, const LengthMod& b) {
    return a.id() == b.id();
  }
  friend bool operator!=(const LengthMod& a, const LengthMod& b) {
    return !(a == b);
  }
  friend std::ostream& operator<<(std::ostream& os, const LengthMod& v) {
    return os << v.name();
  }

 private:
  struct Spec {
    Id value;
    const char *name;
    size_t name_length;
  };
  static const Spec kSpecs[];

  explicit LengthMod(Id id) : id_(id) {}

  Id id_;
};

// clang-format off
#define ABSL_CONVERSION_CHARS_EXPAND_(X_VAL, X_SEP) \
  /* text */ \
  X_VAL(c) X_SEP X_VAL(C) X_SEP X_VAL(s) X_SEP X_VAL(S) X_SEP \
  /* ints */ \
  X_VAL(d) X_SEP X_VAL(i) X_SEP X_VAL(o) X_SEP \
  X_VAL(u) X_SEP X_VAL(x) X_SEP X_VAL(X) X_SEP \
  /* floats */ \
  X_VAL(f) X_SEP X_VAL(F) X_SEP X_VAL(e) X_SEP X_VAL(E) X_SEP \
  X_VAL(g) X_SEP X_VAL(G) X_SEP X_VAL(a) X_SEP X_VAL(A) X_SEP \
  /* misc */ \
  X_VAL(n) X_SEP X_VAL(p)
// clang-format on

struct ConversionChar {
 public:
  enum Id : uint8_t {
    c, C, s, S,              // text
    d, i, o, u, x, X,        // int
    f, F, e, E, g, G, a, A,  // float
    n, p,                    // misc
    none
  };
  static const size_t kNumValues = none + 1;

  ConversionChar() : id_(none) {}

 public:
  // Index into the opaque array of ConversionChar enums.
  // Requires: i < kNumValues
  static ConversionChar FromIndex(size_t i) {
    return ConversionChar(kSpecs[i].value);
  }

  static ConversionChar FromChar(char c) {
    ConversionChar::Id out_id = ConversionChar::none;
    switch (c) {
#define X_VAL(id)                \
  case #id[0]:                   \
    out_id = ConversionChar::id; \
    break;
      ABSL_CONVERSION_CHARS_EXPAND_(X_VAL, )
#undef X_VAL
      default:
        break;
    }
    return ConversionChar(out_id);
  }

  static ConversionChar FromId(Id id) { return ConversionChar(id); }
  Id id() const { return id_; }

  int radix() const {
    switch (id()) {
      case x: case X: case a: case A: case p: return 16;
      case o: return 8;
      default: return 10;
    }
  }

  bool upper() const {
    switch (id()) {
      case X: case F: case E: case G: case A: return true;
      default: return false;
    }
  }

  bool is_signed() const {
    switch (id()) {
      case d: case i: return true;
      default: return false;
    }
  }

  bool is_integral() const {
    switch (id()) {
      case d: case i: case u: case o: case x: case X:
        return true;
      default: return false;
    }
  }

  bool is_float() const {
    switch (id()) {
      case a: case e: case f: case g: case A: case E: case F: case G:
        return true;
      default: return false;
    }
  }

  bool IsValid() const { return id() != none; }

  // The associated char.
  char Char() const { return kSpecs[id_].name; }

  friend bool operator==(const ConversionChar& a, const ConversionChar& b) {
    return a.id() == b.id();
  }
  friend bool operator!=(const ConversionChar& a, const ConversionChar& b) {
    return !(a == b);
  }
  friend std::ostream& operator<<(std::ostream& os, const ConversionChar& v) {
    char c = v.Char();
    if (!c) c = '?';
    return os << c;
  }

 private:
  struct Spec {
    Id value;
    char name;
  };
  static const Spec kSpecs[];

  explicit ConversionChar(Id id) : id_(id) {}

  Id id_;
};

class ConversionSpec {
 public:
  Flags flags() const { return flags_; }
  LengthMod length_mod() const { return length_mod_; }
  ConversionChar conv() const {
    // Keep this field first in the struct . It generates better code when
    // accessing it when ConversionSpec is passed by value in registers.
    static_assert(offsetof(ConversionSpec, conv_) == 0, "");
    return conv_;
  }

  // Returns the specified width. If width is unspecfied, it returns a negative
  // value.
  int width() const { return width_; }
  // Returns the specified precision. If precision is unspecfied, it returns a
  // negative value.
  int precision() const { return precision_; }

  void set_flags(Flags f) { flags_ = f; }
  void set_length_mod(LengthMod lm) { length_mod_ = lm; }
  void set_conv(ConversionChar c) { conv_ = c; }
  void set_width(int w) { width_ = w; }
  void set_precision(int p) { precision_ = p; }
  void set_left(bool b) { flags_.left = b; }

 private:
  ConversionChar conv_;
  Flags flags_;
  LengthMod length_mod_;
  int width_;
  int precision_;
};

constexpr uint64_t ConversionCharToConvValue(char conv) {
  return
#define CONV_SET_CASE(c) \
  conv == #c[0] ? (uint64_t{1} << (1 + ConversionChar::Id::c)):
      ABSL_CONVERSION_CHARS_EXPAND_(CONV_SET_CASE, )
#undef CONV_SET_CASE
                  conv == '*'
          ? 1
          : 0;
}

enum class Conv : uint64_t {
#define CONV_SET_CASE(c) c = ConversionCharToConvValue(#c[0]),
  ABSL_CONVERSION_CHARS_EXPAND_(CONV_SET_CASE, )
#undef CONV_SET_CASE

  // Used for width/precision '*' specification.
  star = ConversionCharToConvValue('*'),

  // Some predefined values:
  integral = d | i | u | o | x | X,
  floating = a | e | f | g | A | E | F | G,
  numeric = integral | floating,
  string = s,  // absl:ignore(std::string)
  pointer = p
};

// Type safe OR operator.
// We need this for two reasons:
//  1. operator| on enums makes them decay to integers and the result is an
//     integer. We need the result to stay as an enum.
//  2. We use "enum class" which would not work even if we accepted the decay.
constexpr Conv operator|(Conv a, Conv b) {
  return Conv(static_cast<uint64_t>(a) | static_cast<uint64_t>(b));
}

// Get a conversion with a single character in it.
constexpr Conv ConversionCharToConv(char c) {
  return Conv(ConversionCharToConvValue(c));
}

// Checks whether `c` exists in `set`.
constexpr bool Contains(Conv set, char c) {
  return (static_cast<uint64_t>(set) & ConversionCharToConvValue(c)) != 0;
}

// Checks whether all the characters in `c` are contained in `set`
constexpr bool Contains(Conv set, Conv c) {
  return (static_cast<uint64_t>(set) & static_cast<uint64_t>(c)) ==
         static_cast<uint64_t>(c);
}

// Return type of the AbslFormatConvert() functions.
// The Conv template parameter is used to inform the framework of what
// conversion characters are supported by that AbslFormatConvert routine.
template <Conv C>
struct ConvertResult {
  static constexpr Conv kConv = C;
  bool value;
};
template <Conv C>
constexpr Conv ConvertResult<C>::kConv;

// Return capacity - used, clipped to a minimum of 0.
inline size_t Excess(size_t used, size_t capacity) {
  return used < capacity ? capacity - used : 0;
}

}  // namespace str_format_internal

}  // namespace absl

#endif  // ABSL_STRINGS_INTERNAL_STR_FORMAT_EXTENSION_H_

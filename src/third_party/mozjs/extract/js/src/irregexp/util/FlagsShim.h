// Copyright 2014 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_UTIL_FLAGS_H_
#define V8_UTIL_FLAGS_H_

// Origin:
// https://github.com/v8/v8/blob/1bafcc6b999b23ea1d394f5d267a08183e3c4e19/src/base/flags.h#L15-L90

namespace v8 {
namespace base {

// The Flags class provides a type-safe way of storing OR-combinations of enum
// values. The Flags<T, S> class is a template class, where T is an enum type,
// and S is the underlying storage type (usually int).
//
// The traditional C++ approach for storing OR-combinations of enum values is to
// use an int or unsigned int variable. The inconvenience with this approach is
// that there's no type checking at all; any enum value can be OR'd with any
// other enum value and passed on to a function that takes an int or unsigned
// int.
template <typename T, typename S = int>
class Flags final {
 public:
  using flag_type = T;
  using mask_type = S;

  constexpr Flags() : mask_(0) {}
  constexpr Flags(flag_type flag) : mask_(static_cast<S>(flag)) {}
  constexpr explicit Flags(mask_type mask) : mask_(static_cast<S>(mask)) {}

  constexpr bool operator==(flag_type flag) const {
    return mask_ == static_cast<S>(flag);
  }
  constexpr bool operator!=(flag_type flag) const {
    return mask_ != static_cast<S>(flag);
  }

  Flags& operator&=(const Flags& flags) {
    mask_ &= flags.mask_;
    return *this;
  }
  Flags& operator|=(const Flags& flags) {
    mask_ |= flags.mask_;
    return *this;
  }
  Flags& operator^=(const Flags& flags) {
    mask_ ^= flags.mask_;
    return *this;
  }

  constexpr Flags operator&(const Flags& flags) const {
    return Flags(mask_ & flags.mask_);
  }
  constexpr Flags operator|(const Flags& flags) const {
    return Flags(mask_ | flags.mask_);
  }
  constexpr Flags operator^(const Flags& flags) const {
    return Flags(mask_ ^ flags.mask_);
  }

  Flags& operator&=(flag_type flag) { return operator&=(Flags(flag)); }
  Flags& operator|=(flag_type flag) { return operator|=(Flags(flag)); }
  Flags& operator^=(flag_type flag) { return operator^=(Flags(flag)); }

  constexpr Flags operator&(flag_type flag) const {
    return operator&(Flags(flag));
  }
  constexpr Flags operator|(flag_type flag) const {
    return operator|(Flags(flag));
  }
  constexpr Flags operator^(flag_type flag) const {
    return operator^(Flags(flag));
  }

  constexpr Flags operator~() const { return Flags(~mask_); }

  constexpr operator mask_type() const { return mask_; }
  constexpr bool operator!() const { return !mask_; }

  Flags without(flag_type flag) { return *this & (~Flags(flag)); }

  friend size_t hash_value(const Flags& flags) { return flags.mask_; }

 private:
  mask_type mask_;
};

}  // namespace base
}  // namespace v8

#endif  // V8_UTIL_FLAG_H_

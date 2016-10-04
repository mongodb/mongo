// Copyright 2004 Google Inc.
// All Rights Reserved.
//

#ifndef BASE_INT128_H_
#define BASE_INT128_H_

#include <iostream>
using std::ostream;
using std::cout;
using std::endl;

#include "integral_types.h"
#include "logging.h"

// An unsigned 128-bit integer type. Thread-compatible.
class uint128 {
public:
  uint128();  // Sets to 0, but don't trust on this behavior.
  uint128(uint64 top, uint64 bottom);
#ifndef SWIG
  uint128(int bottom);
  uint128(uint32 bottom);   // Top 96 bits = 0
#endif
  uint128(uint64 bottom);   // hi_ = 0
  uint128(const uint128 &val);

  void Initialize(uint64 top, uint64 bottom);

  bool operator==(const uint128& b) const;
  bool operator!=(const uint128& b) const;
  uint128& operator=(const uint128& b);

  bool operator<(const uint128& b) const;
  bool operator>(const uint128& b) const;
  bool operator<=(const uint128& b) const;
  bool operator>=(const uint128& b) const;

  // Logical operators.
  uint128 operator~() const;
  uint128 operator|(const uint128& b) const;
  uint128 operator&(const uint128& b) const;
  uint128 operator^(const uint128& b) const;

  // Shift operators.
  uint128 operator<<(int amount) const;
  uint128 operator>>(int amount) const;

  // Arithmetic operators.
  // TODO: multiplication, division, etc.
  uint128 operator+(const uint128& b) const;
  uint128 operator-(const uint128& b) const;
  uint128 operator+=(const uint128& b);
  uint128 operator-=(const uint128& b);
  uint128 operator++(int);
  uint128 operator--(int);
  uint128 operator++();
  uint128 operator--();

  friend uint64 Uint128Low64(const uint128& v);
  friend uint64 Uint128High64(const uint128& v);

  friend ostream& operator<<(ostream& o, const uint128& b);

private:
  // Little-endian memory order optimizations can benefit from
  // having lo_ first, hi_ last.
  // See util/endian/endian.h and Load128/Store128 for storing a uint128.
  uint64        lo_;
  uint64        hi_;

  // Not implemented, just declared for catching automatic type conversions.
  uint128(uint8);
  uint128(uint16);
  uint128(float v);
  uint128(double v);
};

extern const uint128 kuint128max;

// allow uint128 to be logged
extern ostream& operator<<(ostream& o, const uint128& b);

// Methods to access low and high pieces of 128-bit value.
// Defined externally from uint128 to facilitate conversion
// to native 128-bit types when compilers support them.
inline uint64 Uint128Low64(const uint128& v) { return v.lo_; }
inline uint64 Uint128High64(const uint128& v) { return v.hi_; }

// TODO: perhaps it would be nice to have int128, a signed 128-bit type?

// --------------------------------------------------------------------------
//                      Implementation details follow
// --------------------------------------------------------------------------
inline bool uint128::operator==(const uint128& b) const {
  return (lo_ == b.lo_) && (hi_ == b.hi_);
}
inline bool uint128::operator!=(const uint128& b) const {
  return !(*this == b);
}
inline uint128& uint128::operator=(const uint128& b) {
  lo_ = b.lo_;
  hi_ = b.hi_;
  return *this;
}

inline uint128::uint128(): lo_(0), hi_(0) { }
inline uint128::uint128(uint64 top, uint64 bottom) : lo_(bottom), hi_(top) { }
inline uint128::uint128(const uint128 &v) : lo_(v.lo_), hi_(v.hi_) { }
inline uint128::uint128(uint64 bottom) : lo_(bottom), hi_(0) { }
#ifndef SWIG
inline uint128::uint128(uint32 bottom) : lo_(bottom), hi_(0) { }
inline uint128::uint128(int bottom) : lo_(bottom), hi_(0) {
  if (bottom < 0) {
    --hi_;
  }
}
#endif
inline void uint128::Initialize(uint64 top, uint64 bottom) {
  hi_ = top;
  lo_ = bottom;
}

// Comparison operators.

#define CMP128(op)                                              \
inline bool uint128::operator op(const uint128& b) const {      \
  return (hi_ == b.hi_) ? (lo_ op b.lo_) : (hi_ op b.hi_);      \
}

CMP128(<)
CMP128(>)
CMP128(>=)
CMP128(<=)

#undef CMP128

// Logical operators.

inline uint128 uint128::operator~() const {
  return uint128(~hi_, ~lo_);
}

#define LOGIC128(op)                                             \
inline uint128 uint128::operator op(const uint128& b) const {    \
  return uint128(hi_ op b.hi_, lo_ op b.lo_);                    \
}

LOGIC128(|)
LOGIC128(&)
LOGIC128(^)

#undef LOGIC128

// Shift operators.

inline uint128 uint128::operator<<(int amount) const {
  DCHECK_GE(amount, 0);

  // uint64 shifts of >= 64 are undefined, so we will need some special-casing.
  if (amount < 64) {
    if (amount == 0) {
      return *this;
    }
    uint64 new_hi = (hi_ << amount) | (lo_ >> (64 - amount));
    uint64 new_lo = lo_ << amount;
    return uint128(new_hi, new_lo);
  } else if (amount < 128) {
    return uint128(lo_ << (amount - 64), 0);
  } else {
    return uint128(0, 0);
  }
}

inline uint128 uint128::operator>>(int amount) const {
  DCHECK_GE(amount, 0);

  // uint64 shifts of >= 64 are undefined, so we will need some special-casing.
  if (amount < 64) {
    if (amount == 0) {
      return *this;
    }
    uint64 new_hi = hi_ >> amount;
    uint64 new_lo = (lo_ >> amount) | (hi_ << (64 - amount));
    return uint128(new_hi, new_lo);
  } else if (amount < 128) {
    return uint128(0, hi_ >> (amount - 64));
  } else {
    return uint128(0, 0);
  }
}

inline uint128 uint128::operator+(const uint128& b) const {
  return uint128(*this) += b;
}

inline uint128 uint128::operator-(const uint128& b) const {
  return uint128(*this) -= b;
}

inline uint128 uint128::operator+=(const uint128& b) {
  hi_ += b.hi_;
  lo_ += b.lo_;
  if (lo_ < b.lo_)
    ++hi_;
  return *this;
}

inline uint128 uint128::operator-=(const uint128& b) {
  hi_ -= b.hi_;
  if (b.lo_ > lo_)
    --hi_;
  lo_ -= b.lo_;
  return *this;
}

inline uint128 uint128::operator++(int) {
  uint128 tmp(*this);
  *this += 1;
  return tmp;
}

inline uint128 uint128::operator--(int) {
  uint128 tmp(*this);
  *this -= 1;
  return tmp;
}

inline uint128 uint128::operator++() {
  *this += 1;
  return *this;
}

inline uint128 uint128::operator--() {
  *this -= 1;
  return *this;
}

#endif  // BASE_INT128_H_

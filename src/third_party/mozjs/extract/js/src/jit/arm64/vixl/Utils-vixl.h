// Copyright 2015, VIXL authors
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//   * Redistributions of source code must retain the above copyright notice,
//     this list of conditions and the following disclaimer.
//   * Redistributions in binary form must reproduce the above copyright notice,
//     this list of conditions and the following disclaimer in the documentation
//     and/or other materials provided with the distribution.
//   * Neither the name of ARM Limited nor the names of its contributors may be
//     used to endorse or promote products derived from this software without
//     specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS CONTRIBUTORS "AS IS" AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
// OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#ifndef VIXL_UTILS_H
#define VIXL_UTILS_H

#include "mozilla/FloatingPoint.h"

#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

#include "jit/arm64/vixl/CompilerIntrinsics-vixl.h"
#include "jit/arm64/vixl/Globals-vixl.h"

namespace vixl {

// Macros for compile-time format checking.
#if GCC_VERSION_OR_NEWER(4, 4, 0)
#define PRINTF_CHECK(format_index, varargs_index) \
  __attribute__((format(gnu_printf, format_index, varargs_index)))
#else
#define PRINTF_CHECK(format_index, varargs_index)
#endif

#ifdef __GNUC__
#define VIXL_HAS_DEPRECATED_WITH_MSG
#elif defined(__clang__)
#ifdef __has_extension
#define VIXL_HAS_DEPRECATED_WITH_MSG
#endif
#endif

#ifdef VIXL_HAS_DEPRECATED_WITH_MSG
#define VIXL_DEPRECATED(replaced_by, declarator) \
  __attribute__((deprecated("Use \"" replaced_by "\" instead"))) declarator
#else
#define VIXL_DEPRECATED(replaced_by, declarator) declarator
#endif

#ifdef VIXL_DEBUG
#define VIXL_UNREACHABLE_OR_FALLTHROUGH() VIXL_UNREACHABLE()
#else
#define VIXL_UNREACHABLE_OR_FALLTHROUGH() VIXL_FALLTHROUGH()
#endif

template <typename T, size_t n>
size_t ArrayLength(const T (&)[n]) {
  return n;
}

// Check number width.
// TODO: Refactor these using templates.
inline bool IsIntN(unsigned n, uint32_t x) {
  VIXL_ASSERT((0 < n) && (n < 32));
  uint32_t limit = UINT32_C(1) << (n - 1);
  return x < limit;
}
inline bool IsIntN(unsigned n, int32_t x) {
  VIXL_ASSERT((0 < n) && (n < 32));
  int32_t limit = INT32_C(1) << (n - 1);
  return (-limit <= x) && (x < limit);
}
inline bool IsIntN(unsigned n, uint64_t x) {
  VIXL_ASSERT((0 < n) && (n < 64));
  uint64_t limit = UINT64_C(1) << (n - 1);
  return x < limit;
}
inline bool IsIntN(unsigned n, int64_t x) {
  VIXL_ASSERT((0 < n) && (n < 64));
  int64_t limit = INT64_C(1) << (n - 1);
  return (-limit <= x) && (x < limit);
}
VIXL_DEPRECATED("IsIntN", inline bool is_intn(unsigned n, int64_t x)) {
  return IsIntN(n, x);
}

inline bool IsUintN(unsigned n, uint32_t x) {
  VIXL_ASSERT((0 < n) && (n < 32));
  return !(x >> n);
}
inline bool IsUintN(unsigned n, int32_t x) {
  VIXL_ASSERT((0 < n) && (n < 32));
  // Convert to an unsigned integer to avoid implementation-defined behavior.
  return !(static_cast<uint32_t>(x) >> n);
}
inline bool IsUintN(unsigned n, uint64_t x) {
  VIXL_ASSERT((0 < n) && (n < 64));
  return !(x >> n);
}
inline bool IsUintN(unsigned n, int64_t x) {
  VIXL_ASSERT((0 < n) && (n < 64));
  // Convert to an unsigned integer to avoid implementation-defined behavior.
  return !(static_cast<uint64_t>(x) >> n);
}
VIXL_DEPRECATED("IsUintN", inline bool is_uintn(unsigned n, int64_t x)) {
  return IsUintN(n, x);
}

inline uint64_t TruncateToUintN(unsigned n, uint64_t x) {
  VIXL_ASSERT((0 < n) && (n < 64));
  return static_cast<uint64_t>(x) & ((UINT64_C(1) << n) - 1);
}
VIXL_DEPRECATED("TruncateToUintN",
                inline uint64_t truncate_to_intn(unsigned n, int64_t x)) {
  return TruncateToUintN(n, x);
}

// clang-format off
#define INT_1_TO_32_LIST(V)                                                    \
V(1)  V(2)  V(3)  V(4)  V(5)  V(6)  V(7)  V(8)                                 \
V(9)  V(10) V(11) V(12) V(13) V(14) V(15) V(16)                                \
V(17) V(18) V(19) V(20) V(21) V(22) V(23) V(24)                                \
V(25) V(26) V(27) V(28) V(29) V(30) V(31) V(32)

#define INT_33_TO_63_LIST(V)                                                   \
V(33) V(34) V(35) V(36) V(37) V(38) V(39) V(40)                                \
V(41) V(42) V(43) V(44) V(45) V(46) V(47) V(48)                                \
V(49) V(50) V(51) V(52) V(53) V(54) V(55) V(56)                                \
V(57) V(58) V(59) V(60) V(61) V(62) V(63)

#define INT_1_TO_63_LIST(V) INT_1_TO_32_LIST(V) INT_33_TO_63_LIST(V)

// clang-format on

#define DECLARE_IS_INT_N(N)                                       \
  inline bool IsInt##N(int64_t x) { return IsIntN(N, x); }        \
  VIXL_DEPRECATED("IsInt" #N, inline bool is_int##N(int64_t x)) { \
    return IsIntN(N, x);                                          \
  }

#define DECLARE_IS_UINT_N(N)                                        \
  inline bool IsUint##N(int64_t x) { return IsUintN(N, x); }        \
  VIXL_DEPRECATED("IsUint" #N, inline bool is_uint##N(int64_t x)) { \
    return IsUintN(N, x);                                           \
  }

#define DECLARE_TRUNCATE_TO_UINT_32(N)                             \
  inline uint32_t TruncateToUint##N(uint64_t x) {                  \
    return static_cast<uint32_t>(TruncateToUintN(N, x));           \
  }                                                                \
  VIXL_DEPRECATED("TruncateToUint" #N,                             \
                  inline uint32_t truncate_to_int##N(int64_t x)) { \
    return TruncateToUint##N(x);                                   \
  }

INT_1_TO_63_LIST(DECLARE_IS_INT_N)
INT_1_TO_63_LIST(DECLARE_IS_UINT_N)
INT_1_TO_32_LIST(DECLARE_TRUNCATE_TO_UINT_32)

#undef DECLARE_IS_INT_N
#undef DECLARE_IS_UINT_N
#undef DECLARE_TRUNCATE_TO_INT_N

// Bit field extraction.
inline uint64_t ExtractUnsignedBitfield64(int msb, int lsb, uint64_t x) {
  VIXL_ASSERT((static_cast<size_t>(msb) < sizeof(x) * 8) && (lsb >= 0) &&
              (msb >= lsb));
  if ((msb == 63) && (lsb == 0)) return x;
  return (x >> lsb) & ((static_cast<uint64_t>(1) << (1 + msb - lsb)) - 1);
}


inline uint32_t ExtractUnsignedBitfield32(int msb, int lsb, uint32_t x) {
  VIXL_ASSERT((static_cast<size_t>(msb) < sizeof(x) * 8) && (lsb >= 0) &&
              (msb >= lsb));
  return TruncateToUint32(ExtractUnsignedBitfield64(msb, lsb, x));
}


inline int64_t ExtractSignedBitfield64(int msb, int lsb, int64_t x) {
  VIXL_ASSERT((static_cast<size_t>(msb) < sizeof(x) * 8) && (lsb >= 0) &&
              (msb >= lsb));
  uint64_t temp = ExtractUnsignedBitfield64(msb, lsb, x);
  // If the highest extracted bit is set, sign extend.
  if ((temp >> (msb - lsb)) == 1) {
    temp |= ~UINT64_C(0) << (msb - lsb);
  }
  int64_t result;
  memcpy(&result, &temp, sizeof(result));
  return result;
}


inline int32_t ExtractSignedBitfield32(int msb, int lsb, int32_t x) {
  VIXL_ASSERT((static_cast<size_t>(msb) < sizeof(x) * 8) && (lsb >= 0) &&
              (msb >= lsb));
  uint32_t temp = TruncateToUint32(ExtractSignedBitfield64(msb, lsb, x));
  int32_t result;
  memcpy(&result, &temp, sizeof(result));
  return result;
}


inline uint64_t RotateRight(uint64_t value,
                            unsigned int rotate,
                            unsigned int width) {
  VIXL_ASSERT((width > 0) && (width <= 64));
  uint64_t width_mask = ~UINT64_C(0) >> (64 - width);
  rotate &= 63;
  if (rotate > 0) {
    value &= width_mask;
    value = (value << (width - rotate)) | (value >> rotate);
  }
  return value & width_mask;
}


// Wrapper class for passing FP16 values through the assembler.
// This is purely to aid with type checking/casting.
class Float16 {
 public:
  explicit Float16(double dvalue);
  Float16() : rawbits_(0x0) {}
  friend uint16_t Float16ToRawbits(Float16 value);
  friend Float16 RawbitsToFloat16(uint16_t bits);

 protected:
  uint16_t rawbits_;
};

// Floating point representation.
uint16_t Float16ToRawbits(Float16 value);


uint32_t FloatToRawbits(float value);
VIXL_DEPRECATED("FloatToRawbits",
                inline uint32_t float_to_rawbits(float value)) {
  return FloatToRawbits(value);
}

uint64_t DoubleToRawbits(double value);
VIXL_DEPRECATED("DoubleToRawbits",
                inline uint64_t double_to_rawbits(double value)) {
  return DoubleToRawbits(value);
}

Float16 RawbitsToFloat16(uint16_t bits);

float RawbitsToFloat(uint32_t bits);
VIXL_DEPRECATED("RawbitsToFloat",
                inline float rawbits_to_float(uint32_t bits)) {
  return RawbitsToFloat(bits);
}

double RawbitsToDouble(uint64_t bits);
VIXL_DEPRECATED("RawbitsToDouble",
                inline double rawbits_to_double(uint64_t bits)) {
  return RawbitsToDouble(bits);
}

namespace internal {

// Internal simulation class used solely by the simulator to
// provide an abstraction layer for any half-precision arithmetic.
class SimFloat16 : public Float16 {
 public:
  // TODO: We should investigate making this constructor explicit.
  // This is currently difficult to do due to a number of templated
  // functions in the simulator which rely on returning double values.
  SimFloat16(double dvalue) : Float16(dvalue) {}  // NOLINT(runtime/explicit)
  SimFloat16(Float16 f) {                         // NOLINT(runtime/explicit)
    this->rawbits_ = Float16ToRawbits(f);
  }
  SimFloat16() : Float16() {}
  SimFloat16 operator-() const;
  SimFloat16 operator+(SimFloat16 rhs) const;
  SimFloat16 operator-(SimFloat16 rhs) const;
  SimFloat16 operator*(SimFloat16 rhs) const;
  SimFloat16 operator/(SimFloat16 rhs) const;
  bool operator<(SimFloat16 rhs) const;
  bool operator>(SimFloat16 rhs) const;
  bool operator==(SimFloat16 rhs) const;
  bool operator!=(SimFloat16 rhs) const;
  // This is necessary for conversions peformed in (macro asm) Fmov.
  bool operator==(double rhs) const;
  operator double() const;
};
}  // namespace internal

uint32_t Float16Sign(internal::SimFloat16 value);

uint32_t Float16Exp(internal::SimFloat16 value);

uint32_t Float16Mantissa(internal::SimFloat16 value);

uint32_t FloatSign(float value);
VIXL_DEPRECATED("FloatSign", inline uint32_t float_sign(float value)) {
  return FloatSign(value);
}

uint32_t FloatExp(float value);
VIXL_DEPRECATED("FloatExp", inline uint32_t float_exp(float value)) {
  return FloatExp(value);
}

uint32_t FloatMantissa(float value);
VIXL_DEPRECATED("FloatMantissa", inline uint32_t float_mantissa(float value)) {
  return FloatMantissa(value);
}

uint32_t DoubleSign(double value);
VIXL_DEPRECATED("DoubleSign", inline uint32_t double_sign(double value)) {
  return DoubleSign(value);
}

uint32_t DoubleExp(double value);
VIXL_DEPRECATED("DoubleExp", inline uint32_t double_exp(double value)) {
  return DoubleExp(value);
}

uint64_t DoubleMantissa(double value);
VIXL_DEPRECATED("DoubleMantissa",
                inline uint64_t double_mantissa(double value)) {
  return DoubleMantissa(value);
}

internal::SimFloat16 Float16Pack(uint16_t sign,
                                 uint16_t exp,
                                 uint16_t mantissa);

float FloatPack(uint32_t sign, uint32_t exp, uint32_t mantissa);
VIXL_DEPRECATED("FloatPack",
                inline float float_pack(uint32_t sign,
                                        uint32_t exp,
                                        uint32_t mantissa)) {
  return FloatPack(sign, exp, mantissa);
}

double DoublePack(uint64_t sign, uint64_t exp, uint64_t mantissa);
VIXL_DEPRECATED("DoublePack",
                inline double double_pack(uint32_t sign,
                                          uint32_t exp,
                                          uint64_t mantissa)) {
  return DoublePack(sign, exp, mantissa);
}

// An fpclassify() function for 16-bit half-precision floats.
int Float16Classify(Float16 value);
VIXL_DEPRECATED("Float16Classify", inline int float16classify(uint16_t value)) {
  return Float16Classify(RawbitsToFloat16(value));
}

bool IsZero(Float16 value);

inline bool IsNaN(float value) { return std::isnan(value); }

inline bool IsNaN(double value) { return std::isnan(value); }

inline bool IsNaN(Float16 value) { return Float16Classify(value) == FP_NAN; }

inline bool IsInf(float value) { return std::isinf(value); }

inline bool IsInf(double value) { return std::isinf(value); }

inline bool IsInf(Float16 value) {
  return Float16Classify(value) == FP_INFINITE;
}


// NaN tests.
inline bool IsSignallingNaN(double num) {
  const uint64_t kFP64QuietNaNMask = UINT64_C(0x0008000000000000);
  uint64_t raw = DoubleToRawbits(num);
  if (IsNaN(num) && ((raw & kFP64QuietNaNMask) == 0)) {
    return true;
  }
  return false;
}


inline bool IsSignallingNaN(float num) {
  const uint32_t kFP32QuietNaNMask = 0x00400000;
  uint32_t raw = FloatToRawbits(num);
  if (IsNaN(num) && ((raw & kFP32QuietNaNMask) == 0)) {
    return true;
  }
  return false;
}


inline bool IsSignallingNaN(Float16 num) {
  const uint16_t kFP16QuietNaNMask = 0x0200;
  return IsNaN(num) && ((Float16ToRawbits(num) & kFP16QuietNaNMask) == 0);
}


template <typename T>
inline bool IsQuietNaN(T num) {
  return IsNaN(num) && !IsSignallingNaN(num);
}


// Convert the NaN in 'num' to a quiet NaN.
inline double ToQuietNaN(double num) {
  const uint64_t kFP64QuietNaNMask = UINT64_C(0x0008000000000000);
  VIXL_ASSERT(IsNaN(num));
  return RawbitsToDouble(DoubleToRawbits(num) | kFP64QuietNaNMask);
}


inline float ToQuietNaN(float num) {
  const uint32_t kFP32QuietNaNMask = 0x00400000;
  VIXL_ASSERT(IsNaN(num));
  return RawbitsToFloat(FloatToRawbits(num) | kFP32QuietNaNMask);
}


inline internal::SimFloat16 ToQuietNaN(internal::SimFloat16 num) {
  const uint16_t kFP16QuietNaNMask = 0x0200;
  VIXL_ASSERT(IsNaN(num));
  return internal::SimFloat16(
      RawbitsToFloat16(Float16ToRawbits(num) | kFP16QuietNaNMask));
}


// Fused multiply-add.
inline double FusedMultiplyAdd(double op1, double op2, double a) {
  return fma(op1, op2, a);
}


inline float FusedMultiplyAdd(float op1, float op2, float a) {
  return fmaf(op1, op2, a);
}


inline uint64_t LowestSetBit(uint64_t value) { return value & -value; }


template <typename T>
inline int HighestSetBitPosition(T value) {
  VIXL_ASSERT(value != 0);
  return (sizeof(value) * 8 - 1) - CountLeadingZeros(value);
}


template <typename V>
inline int WhichPowerOf2(V value) {
  VIXL_ASSERT(IsPowerOf2(value));
  return CountTrailingZeros(value);
}


unsigned CountClearHalfWords(uint64_t imm, unsigned reg_size);


int BitCount(uint64_t value);


template <typename T>
T ReverseBits(T value) {
  VIXL_ASSERT((sizeof(value) == 1) || (sizeof(value) == 2) ||
              (sizeof(value) == 4) || (sizeof(value) == 8));
  T result = 0;
  for (unsigned i = 0; i < (sizeof(value) * 8); i++) {
    result = (result << 1) | (value & 1);
    value >>= 1;
  }
  return result;
}


template <typename T>
inline T SignExtend(T val, int bitSize) {
  VIXL_ASSERT(bitSize > 0);
  T mask = (T(2) << (bitSize - 1)) - T(1);
  val &= mask;
  T sign_bits = -((val >> (bitSize - 1)) << bitSize);
  val |= sign_bits;
  return val;
}


template <typename T>
T ReverseBytes(T value, int block_bytes_log2) {
  VIXL_ASSERT((sizeof(value) == 4) || (sizeof(value) == 8));
  VIXL_ASSERT((1U << block_bytes_log2) <= sizeof(value));
  // Split the 64-bit value into an 8-bit array, where b[0] is the least
  // significant byte, and b[7] is the most significant.
  uint8_t bytes[8];
  uint64_t mask = UINT64_C(0xff00000000000000);
  for (int i = 7; i >= 0; i--) {
    bytes[i] = (static_cast<uint64_t>(value) & mask) >> (i * 8);
    mask >>= 8;
  }

  // Permutation tables for REV instructions.
  //  permute_table[0] is used by REV16_x, REV16_w
  //  permute_table[1] is used by REV32_x, REV_w
  //  permute_table[2] is used by REV_x
  VIXL_ASSERT((0 < block_bytes_log2) && (block_bytes_log2 < 4));
  static const uint8_t permute_table[3][8] = {{6, 7, 4, 5, 2, 3, 0, 1},
                                              {4, 5, 6, 7, 0, 1, 2, 3},
                                              {0, 1, 2, 3, 4, 5, 6, 7}};
  uint64_t temp = 0;
  for (int i = 0; i < 8; i++) {
    temp <<= 8;
    temp |= bytes[permute_table[block_bytes_log2 - 1][i]];
  }

  T result;
  VIXL_STATIC_ASSERT(sizeof(result) <= sizeof(temp));
  memcpy(&result, &temp, sizeof(result));
  return result;
}

template <unsigned MULTIPLE, typename T>
inline bool IsMultiple(T value) {
  VIXL_ASSERT(IsPowerOf2(MULTIPLE));
  return (value & (MULTIPLE - 1)) == 0;
}

template <typename T>
inline bool IsMultiple(T value, unsigned multiple) {
  VIXL_ASSERT(IsPowerOf2(multiple));
  return (value & (multiple - 1)) == 0;
}

template <typename T>
inline bool IsAligned(T pointer, int alignment) {
  VIXL_ASSERT(IsPowerOf2(alignment));
  return (pointer & (alignment - 1)) == 0;
}

// Pointer alignment
// TODO: rename/refactor to make it specific to instructions.
template <unsigned ALIGN, typename T>
inline bool IsAligned(T pointer) {
  VIXL_ASSERT(sizeof(pointer) == sizeof(intptr_t));  // NOLINT(runtime/sizeof)
  // Use C-style casts to get static_cast behaviour for integral types (T), and
  // reinterpret_cast behaviour for other types.
  return IsAligned((intptr_t)(pointer), ALIGN);
}

template <typename T>
bool IsWordAligned(T pointer) {
  return IsAligned<4>(pointer);
}

// Increment a pointer until it has the specified alignment. The alignment must
// be a power of two.
template <class T>
T AlignUp(T pointer,
          typename Unsigned<sizeof(T) * kBitsPerByte>::type alignment) {
  VIXL_ASSERT(IsPowerOf2(alignment));
  // Use C-style casts to get static_cast behaviour for integral types (T), and
  // reinterpret_cast behaviour for other types.

  typename Unsigned<sizeof(T)* kBitsPerByte>::type pointer_raw =
      (typename Unsigned<sizeof(T) * kBitsPerByte>::type)pointer;
  VIXL_STATIC_ASSERT(sizeof(pointer) <= sizeof(pointer_raw));

  size_t mask = alignment - 1;
  T result = (T)((pointer_raw + mask) & ~mask);
  VIXL_ASSERT(result >= pointer);

  return result;
}

// Decrement a pointer until it has the specified alignment. The alignment must
// be a power of two.
template <class T>
T AlignDown(T pointer,
            typename Unsigned<sizeof(T) * kBitsPerByte>::type alignment) {
  VIXL_ASSERT(IsPowerOf2(alignment));
  // Use C-style casts to get static_cast behaviour for integral types (T), and
  // reinterpret_cast behaviour for other types.

  typename Unsigned<sizeof(T)* kBitsPerByte>::type pointer_raw =
      (typename Unsigned<sizeof(T) * kBitsPerByte>::type)pointer;
  VIXL_STATIC_ASSERT(sizeof(pointer) <= sizeof(pointer_raw));

  size_t mask = alignment - 1;
  return (T)(pointer_raw & ~mask);
}


template <typename T>
inline T ExtractBit(T value, unsigned bit) {
  return (value >> bit) & T(1);
}

template <typename Ts, typename Td>
inline Td ExtractBits(Ts value, int least_significant_bit, Td mask) {
  return Td((value >> least_significant_bit) & Ts(mask));
}

template <typename Ts, typename Td>
inline void AssignBit(Td& dst,  // NOLINT(runtime/references)
                      int bit,
                      Ts value) {
  VIXL_ASSERT((value == Ts(0)) || (value == Ts(1)));
  VIXL_ASSERT(bit >= 0);
  VIXL_ASSERT(bit < static_cast<int>(sizeof(Td) * 8));
  Td mask(1);
  dst &= ~(mask << bit);
  dst |= Td(value) << bit;
}

template <typename Td, typename Ts>
inline void AssignBits(Td& dst,  // NOLINT(runtime/references)
                       int least_significant_bit,
                       Ts mask,
                       Ts value) {
  VIXL_ASSERT(least_significant_bit >= 0);
  VIXL_ASSERT(least_significant_bit < static_cast<int>(sizeof(Td) * 8));
  VIXL_ASSERT(((Td(mask) << least_significant_bit) >> least_significant_bit) ==
              Td(mask));
  VIXL_ASSERT((value & mask) == value);
  dst &= ~(Td(mask) << least_significant_bit);
  dst |= Td(value) << least_significant_bit;
}

class VFP {
 public:
  static uint32_t FP32ToImm8(float imm) {
    // bits: aBbb.bbbc.defg.h000.0000.0000.0000.0000
    uint32_t bits = FloatToRawbits(imm);
    // bit7: a000.0000
    uint32_t bit7 = ((bits >> 31) & 0x1) << 7;
    // bit6: 0b00.0000
    uint32_t bit6 = ((bits >> 29) & 0x1) << 6;
    // bit5_to_0: 00cd.efgh
    uint32_t bit5_to_0 = (bits >> 19) & 0x3f;
    return static_cast<uint32_t>(bit7 | bit6 | bit5_to_0);
  }
  static uint32_t FP64ToImm8(double imm) {
    // bits: aBbb.bbbb.bbcd.efgh.0000.0000.0000.0000
    //       0000.0000.0000.0000.0000.0000.0000.0000
    uint64_t bits = DoubleToRawbits(imm);
    // bit7: a000.0000
    uint64_t bit7 = ((bits >> 63) & 0x1) << 7;
    // bit6: 0b00.0000
    uint64_t bit6 = ((bits >> 61) & 0x1) << 6;
    // bit5_to_0: 00cd.efgh
    uint64_t bit5_to_0 = (bits >> 48) & 0x3f;

    return static_cast<uint32_t>(bit7 | bit6 | bit5_to_0);
  }
  static float Imm8ToFP32(uint32_t imm8) {
    //   Imm8: abcdefgh (8 bits)
    // Single: aBbb.bbbc.defg.h000.0000.0000.0000.0000 (32 bits)
    // where B is b ^ 1
    uint32_t bits = imm8;
    uint32_t bit7 = (bits >> 7) & 0x1;
    uint32_t bit6 = (bits >> 6) & 0x1;
    uint32_t bit5_to_0 = bits & 0x3f;
    uint32_t result = (bit7 << 31) | ((32 - bit6) << 25) | (bit5_to_0 << 19);

    return RawbitsToFloat(result);
  }
  static double Imm8ToFP64(uint32_t imm8) {
    //   Imm8: abcdefgh (8 bits)
    // Double: aBbb.bbbb.bbcd.efgh.0000.0000.0000.0000
    //         0000.0000.0000.0000.0000.0000.0000.0000 (64 bits)
    // where B is b ^ 1
    uint32_t bits = imm8;
    uint64_t bit7 = (bits >> 7) & 0x1;
    uint64_t bit6 = (bits >> 6) & 0x1;
    uint64_t bit5_to_0 = bits & 0x3f;
    uint64_t result = (bit7 << 63) | ((256 - bit6) << 54) | (bit5_to_0 << 48);
    return RawbitsToDouble(result);
  }
  static bool IsImmFP32(float imm) {
    // Valid values will have the form:
    // aBbb.bbbc.defg.h000.0000.0000.0000.0000
    uint32_t bits = FloatToRawbits(imm);
    // bits[19..0] are cleared.
    if ((bits & 0x7ffff) != 0) {
      return false;
    }


    // bits[29..25] are all set or all cleared.
    uint32_t b_pattern = (bits >> 16) & 0x3e00;
    if (b_pattern != 0 && b_pattern != 0x3e00) {
      return false;
    }
    // bit[30] and bit[29] are opposite.
    if (((bits ^ (bits << 1)) & 0x40000000) == 0) {
      return false;
    }
    return true;
  }
  static bool IsImmFP64(double imm) {
    // Valid values will have the form:
    // aBbb.bbbb.bbcd.efgh.0000.0000.0000.0000
    // 0000.0000.0000.0000.0000.0000.0000.0000
    uint64_t bits = DoubleToRawbits(imm);
    // bits[47..0] are cleared.
    if ((bits & 0x0000ffffffffffff) != 0) {
      return false;
    }
    // bits[61..54] are all set or all cleared.
    uint32_t b_pattern = (bits >> 48) & 0x3fc0;
    if ((b_pattern != 0) && (b_pattern != 0x3fc0)) {
      return false;
    }
    // bit[62] and bit[61] are opposite.
    if (((bits ^ (bits << 1)) & (UINT64_C(1) << 62)) == 0) {
      return false;
    }
    return true;
  }
};

class BitField {
  // ForEachBitHelper is a functor that will call
  // bool ForEachBitHelper::execute(ElementType id) const
  //   and expects a boolean in return whether to continue (if true)
  //   or stop (if false)
  // check_set will check if the bits are on (true) or off(false)
  template <typename ForEachBitHelper, bool check_set>
  bool ForEachBit(const ForEachBitHelper& helper) {
    for (int i = 0; static_cast<size_t>(i) < bitfield_.size(); i++) {
      if (bitfield_[i] == check_set)
        if (!helper.execute(i)) return false;
    }
    return true;
  }

 public:
  explicit BitField(unsigned size) : bitfield_(size, 0) {}

  void Set(int i) {
    VIXL_ASSERT((i >= 0) && (static_cast<size_t>(i) < bitfield_.size()));
    bitfield_[i] = true;
  }

  void Unset(int i) {
    VIXL_ASSERT((i >= 0) && (static_cast<size_t>(i) < bitfield_.size()));
    bitfield_[i] = true;
  }

  bool IsSet(int i) const { return bitfield_[i]; }

  // For each bit not set in the bitfield call the execute functor
  // execute.
  // ForEachBitSetHelper::execute returns true if the iteration through
  // the bits can continue, otherwise it will stop.
  // struct ForEachBitSetHelper {
  //   bool execute(int /*id*/) { return false; }
  // };
  template <typename ForEachBitNotSetHelper>
  bool ForEachBitNotSet(const ForEachBitNotSetHelper& helper) {
    return ForEachBit<ForEachBitNotSetHelper, false>(helper);
  }

  // For each bit set in the bitfield call the execute functor
  // execute.
  template <typename ForEachBitSetHelper>
  bool ForEachBitSet(const ForEachBitSetHelper& helper) {
    return ForEachBit<ForEachBitSetHelper, true>(helper);
  }

 private:
  std::vector<bool> bitfield_;
};

namespace internal {

typedef int64_t Int64;
class Uint64;
class Uint128;

class Uint32 {
  uint32_t data_;

 public:
  // Unlike uint32_t, Uint32 has a default constructor.
  Uint32() { data_ = 0; }
  explicit Uint32(uint32_t data) : data_(data) {}
  inline explicit Uint32(Uint64 data);
  uint32_t Get() const { return data_; }
  template <int N>
  int32_t GetSigned() const {
    return ExtractSignedBitfield32(N - 1, 0, data_);
  }
  int32_t GetSigned() const { return data_; }
  Uint32 operator~() const { return Uint32(~data_); }
  Uint32 operator-() const { return Uint32(-data_); }
  bool operator==(Uint32 value) const { return data_ == value.data_; }
  bool operator!=(Uint32 value) const { return data_ != value.data_; }
  bool operator>(Uint32 value) const { return data_ > value.data_; }
  Uint32 operator+(Uint32 value) const { return Uint32(data_ + value.data_); }
  Uint32 operator-(Uint32 value) const { return Uint32(data_ - value.data_); }
  Uint32 operator&(Uint32 value) const { return Uint32(data_ & value.data_); }
  Uint32 operator&=(Uint32 value) {
    data_ &= value.data_;
    return *this;
  }
  Uint32 operator^(Uint32 value) const { return Uint32(data_ ^ value.data_); }
  Uint32 operator^=(Uint32 value) {
    data_ ^= value.data_;
    return *this;
  }
  Uint32 operator|(Uint32 value) const { return Uint32(data_ | value.data_); }
  Uint32 operator|=(Uint32 value) {
    data_ |= value.data_;
    return *this;
  }
  // Unlike uint32_t, the shift functions can accept negative shift and
  // return 0 when the shift is too big.
  Uint32 operator>>(int shift) const {
    if (shift == 0) return *this;
    if (shift < 0) {
      int tmp = -shift;
      if (tmp >= 32) return Uint32(0);
      return Uint32(data_ << tmp);
    }
    int tmp = shift;
    if (tmp >= 32) return Uint32(0);
    return Uint32(data_ >> tmp);
  }
  Uint32 operator<<(int shift) const {
    if (shift == 0) return *this;
    if (shift < 0) {
      int tmp = -shift;
      if (tmp >= 32) return Uint32(0);
      return Uint32(data_ >> tmp);
    }
    int tmp = shift;
    if (tmp >= 32) return Uint32(0);
    return Uint32(data_ << tmp);
  }
};

class Uint64 {
  uint64_t data_;

 public:
  // Unlike uint64_t, Uint64 has a default constructor.
  Uint64() { data_ = 0; }
  explicit Uint64(uint64_t data) : data_(data) {}
  explicit Uint64(Uint32 data) : data_(data.Get()) {}
  inline explicit Uint64(Uint128 data);
  uint64_t Get() const { return data_; }
  int64_t GetSigned(int N) const {
    return ExtractSignedBitfield64(N - 1, 0, data_);
  }
  int64_t GetSigned() const { return data_; }
  Uint32 ToUint32() const {
    VIXL_ASSERT((data_ >> 32) == 0);
    return Uint32(static_cast<uint32_t>(data_));
  }
  Uint32 GetHigh32() const { return Uint32(data_ >> 32); }
  Uint32 GetLow32() const { return Uint32(data_ & 0xffffffff); }
  Uint64 operator~() const { return Uint64(~data_); }
  Uint64 operator-() const { return Uint64(-data_); }
  bool operator==(Uint64 value) const { return data_ == value.data_; }
  bool operator!=(Uint64 value) const { return data_ != value.data_; }
  Uint64 operator+(Uint64 value) const { return Uint64(data_ + value.data_); }
  Uint64 operator-(Uint64 value) const { return Uint64(data_ - value.data_); }
  Uint64 operator&(Uint64 value) const { return Uint64(data_ & value.data_); }
  Uint64 operator&=(Uint64 value) {
    data_ &= value.data_;
    return *this;
  }
  Uint64 operator^(Uint64 value) const { return Uint64(data_ ^ value.data_); }
  Uint64 operator^=(Uint64 value) {
    data_ ^= value.data_;
    return *this;
  }
  Uint64 operator|(Uint64 value) const { return Uint64(data_ | value.data_); }
  Uint64 operator|=(Uint64 value) {
    data_ |= value.data_;
    return *this;
  }
  // Unlike uint64_t, the shift functions can accept negative shift and
  // return 0 when the shift is too big.
  Uint64 operator>>(int shift) const {
    if (shift == 0) return *this;
    if (shift < 0) {
      int tmp = -shift;
      if (tmp >= 64) return Uint64(0);
      return Uint64(data_ << tmp);
    }
    int tmp = shift;
    if (tmp >= 64) return Uint64(0);
    return Uint64(data_ >> tmp);
  }
  Uint64 operator<<(int shift) const {
    if (shift == 0) return *this;
    if (shift < 0) {
      int tmp = -shift;
      if (tmp >= 64) return Uint64(0);
      return Uint64(data_ >> tmp);
    }
    int tmp = shift;
    if (tmp >= 64) return Uint64(0);
    return Uint64(data_ << tmp);
  }
};

class Uint128 {
  uint64_t data_high_;
  uint64_t data_low_;

 public:
  Uint128() : data_high_(0), data_low_(0) {}
  explicit Uint128(uint64_t data_low) : data_high_(0), data_low_(data_low) {}
  explicit Uint128(Uint64 data_low)
      : data_high_(0), data_low_(data_low.Get()) {}
  Uint128(uint64_t data_high, uint64_t data_low)
      : data_high_(data_high), data_low_(data_low) {}
  Uint64 ToUint64() const {
    VIXL_ASSERT(data_high_ == 0);
    return Uint64(data_low_);
  }
  Uint64 GetHigh64() const { return Uint64(data_high_); }
  Uint64 GetLow64() const { return Uint64(data_low_); }
  Uint128 operator~() const { return Uint128(~data_high_, ~data_low_); }
  bool operator==(Uint128 value) const {
    return (data_high_ == value.data_high_) && (data_low_ == value.data_low_);
  }
  Uint128 operator&(Uint128 value) const {
    return Uint128(data_high_ & value.data_high_, data_low_ & value.data_low_);
  }
  Uint128 operator&=(Uint128 value) {
    data_high_ &= value.data_high_;
    data_low_ &= value.data_low_;
    return *this;
  }
  Uint128 operator|=(Uint128 value) {
    data_high_ |= value.data_high_;
    data_low_ |= value.data_low_;
    return *this;
  }
  Uint128 operator>>(int shift) const {
    VIXL_ASSERT((shift >= 0) && (shift < 128));
    if (shift == 0) return *this;
    if (shift >= 64) {
      return Uint128(0, data_high_ >> (shift - 64));
    }
    uint64_t tmp = (data_high_ << (64 - shift)) | (data_low_ >> shift);
    return Uint128(data_high_ >> shift, tmp);
  }
  Uint128 operator<<(int shift) const {
    VIXL_ASSERT((shift >= 0) && (shift < 128));
    if (shift == 0) return *this;
    if (shift >= 64) {
      return Uint128(data_low_ << (shift - 64), 0);
    }
    uint64_t tmp = (data_high_ << shift) | (data_low_ >> (64 - shift));
    return Uint128(tmp, data_low_ << shift);
  }
};

Uint32::Uint32(Uint64 data) : data_(data.ToUint32().Get()) {}
Uint64::Uint64(Uint128 data) : data_(data.ToUint64().Get()) {}

Int64 BitCount(Uint32 value);

}  // namespace internal

// The default NaN values (for FPCR.DN=1).
extern const double kFP64DefaultNaN;
extern const float kFP32DefaultNaN;
extern const Float16 kFP16DefaultNaN;

// Floating-point infinity values.
extern const Float16 kFP16PositiveInfinity;
extern const Float16 kFP16NegativeInfinity;
extern const float kFP32PositiveInfinity;
extern const float kFP32NegativeInfinity;
extern const double kFP64PositiveInfinity;
extern const double kFP64NegativeInfinity;

// Floating-point zero values.
extern const Float16 kFP16PositiveZero;
extern const Float16 kFP16NegativeZero;

// AArch64 floating-point specifics. These match IEEE-754.
const unsigned kDoubleMantissaBits = 52;
const unsigned kDoubleExponentBits = 11;
const unsigned kFloatMantissaBits = 23;
const unsigned kFloatExponentBits = 8;
const unsigned kFloat16MantissaBits = 10;
const unsigned kFloat16ExponentBits = 5;

enum FPRounding {
  // The first four values are encodable directly by FPCR<RMode>.
  FPTieEven = 0x0,
  FPPositiveInfinity = 0x1,
  FPNegativeInfinity = 0x2,
  FPZero = 0x3,

  // The final rounding modes are only available when explicitly specified by
  // the instruction (such as with fcvta). It cannot be set in FPCR.
  FPTieAway,
  FPRoundOdd
};

enum UseDefaultNaN { kUseDefaultNaN, kIgnoreDefaultNaN };

// Assemble the specified IEEE-754 components into the target type and apply
// appropriate rounding.
//  sign:     0 = positive, 1 = negative
//  exponent: Unbiased IEEE-754 exponent.
//  mantissa: The mantissa of the input. The top bit (which is not encoded for
//            normal IEEE-754 values) must not be omitted. This bit has the
//            value 'pow(2, exponent)'.
//
// The input value is assumed to be a normalized value. That is, the input may
// not be infinity or NaN. If the source value is subnormal, it must be
// normalized before calling this function such that the highest set bit in the
// mantissa has the value 'pow(2, exponent)'.
//
// Callers should use FPRoundToFloat or FPRoundToDouble directly, rather than
// calling a templated FPRound.
template <class T, int ebits, int mbits>
T FPRound(int64_t sign,
          int64_t exponent,
          uint64_t mantissa,
          FPRounding round_mode) {
  VIXL_ASSERT((sign == 0) || (sign == 1));

  // Only FPTieEven and FPRoundOdd rounding modes are implemented.
  VIXL_ASSERT((round_mode == FPTieEven) || (round_mode == FPRoundOdd));

  // Rounding can promote subnormals to normals, and normals to infinities. For
  // example, a double with exponent 127 (FLT_MAX_EXP) would appear to be
  // encodable as a float, but rounding based on the low-order mantissa bits
  // could make it overflow. With ties-to-even rounding, this value would become
  // an infinity.

  // ---- Rounding Method ----
  //
  // The exponent is irrelevant in the rounding operation, so we treat the
  // lowest-order bit that will fit into the result ('onebit') as having
  // the value '1'. Similarly, the highest-order bit that won't fit into
  // the result ('halfbit') has the value '0.5'. The 'point' sits between
  // 'onebit' and 'halfbit':
  //
  //            These bits fit into the result.
  //               |---------------------|
  //  mantissa = 0bxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx
  //                                     ||
  //                                    / |
  //                                   /  halfbit
  //                               onebit
  //
  // For subnormal outputs, the range of representable bits is smaller and
  // the position of onebit and halfbit depends on the exponent of the
  // input, but the method is otherwise similar.
  //
  //   onebit(frac)
  //     |
  //     | halfbit(frac)          halfbit(adjusted)
  //     | /                      /
  //     | |                      |
  //  0b00.0 (exact)      -> 0b00.0 (exact)                    -> 0b00
  //  0b00.0...           -> 0b00.0...                         -> 0b00
  //  0b00.1 (exact)      -> 0b00.0111..111                    -> 0b00
  //  0b00.1...           -> 0b00.1...                         -> 0b01
  //  0b01.0 (exact)      -> 0b01.0 (exact)                    -> 0b01
  //  0b01.0...           -> 0b01.0...                         -> 0b01
  //  0b01.1 (exact)      -> 0b01.1 (exact)                    -> 0b10
  //  0b01.1...           -> 0b01.1...                         -> 0b10
  //  0b10.0 (exact)      -> 0b10.0 (exact)                    -> 0b10
  //  0b10.0...           -> 0b10.0...                         -> 0b10
  //  0b10.1 (exact)      -> 0b10.0111..111                    -> 0b10
  //  0b10.1...           -> 0b10.1...                         -> 0b11
  //  0b11.0 (exact)      -> 0b11.0 (exact)                    -> 0b11
  //  ...                   /             |                      /   |
  //                       /              |                     /    |
  //                                                           /     |
  // adjusted = frac - (halfbit(mantissa) & ~onebit(frac));   /      |
  //
  //                   mantissa = (mantissa >> shift) + halfbit(adjusted);

  static const int mantissa_offset = 0;
  static const int exponent_offset = mantissa_offset + mbits;
  static const int sign_offset = exponent_offset + ebits;
  VIXL_ASSERT(sign_offset == (sizeof(T) * 8 - 1));

  // Bail out early for zero inputs.
  if (mantissa == 0) {
    return static_cast<T>(sign << sign_offset);
  }

  // If all bits in the exponent are set, the value is infinite or NaN.
  // This is true for all binary IEEE-754 formats.
  static const int infinite_exponent = (1 << ebits) - 1;
  static const int max_normal_exponent = infinite_exponent - 1;

  // Apply the exponent bias to encode it for the result. Doing this early makes
  // it easy to detect values that will be infinite or subnormal.
  exponent += max_normal_exponent >> 1;

  if (exponent > max_normal_exponent) {
    // Overflow: the input is too large for the result type to represent.
    if (round_mode == FPTieEven) {
      // FPTieEven rounding mode handles overflows using infinities.
      exponent = infinite_exponent;
      mantissa = 0;
    } else {
      VIXL_ASSERT(round_mode == FPRoundOdd);
      // FPRoundOdd rounding mode handles overflows using the largest magnitude
      // normal number.
      exponent = max_normal_exponent;
      mantissa = (UINT64_C(1) << exponent_offset) - 1;
    }
    return static_cast<T>((sign << sign_offset) |
                          (exponent << exponent_offset) |
                          (mantissa << mantissa_offset));
  }

  // Calculate the shift required to move the top mantissa bit to the proper
  // place in the destination type.
  const int highest_significant_bit = 63 - CountLeadingZeros(mantissa);
  int shift = highest_significant_bit - mbits;

  if (exponent <= 0) {
    // The output will be subnormal (before rounding).
    // For subnormal outputs, the shift must be adjusted by the exponent. The +1
    // is necessary because the exponent of a subnormal value (encoded as 0) is
    // the same as the exponent of the smallest normal value (encoded as 1).
    shift += -exponent + 1;

    // Handle inputs that would produce a zero output.
    //
    // Shifts higher than highest_significant_bit+1 will always produce a zero
    // result. A shift of exactly highest_significant_bit+1 might produce a
    // non-zero result after rounding.
    if (shift > (highest_significant_bit + 1)) {
      if (round_mode == FPTieEven) {
        // The result will always be +/-0.0.
        return static_cast<T>(sign << sign_offset);
      } else {
        VIXL_ASSERT(round_mode == FPRoundOdd);
        VIXL_ASSERT(mantissa != 0);
        // For FPRoundOdd, if the mantissa is too small to represent and
        // non-zero return the next "odd" value.
        return static_cast<T>((sign << sign_offset) | 1);
      }
    }

    // Properly encode the exponent for a subnormal output.
    exponent = 0;
  } else {
    // Clear the topmost mantissa bit, since this is not encoded in IEEE-754
    // normal values.
    mantissa &= ~(UINT64_C(1) << highest_significant_bit);
  }

  // The casts below are only well-defined for unsigned integers.
  VIXL_STATIC_ASSERT(std::numeric_limits<T>::is_integer);
  VIXL_STATIC_ASSERT(!std::numeric_limits<T>::is_signed);

  if (shift > 0) {
    if (round_mode == FPTieEven) {
      // We have to shift the mantissa to the right. Some precision is lost, so
      // we need to apply rounding.
      uint64_t onebit_mantissa = (mantissa >> (shift)) & 1;
      uint64_t halfbit_mantissa = (mantissa >> (shift - 1)) & 1;
      uint64_t adjustment = (halfbit_mantissa & ~onebit_mantissa);
      uint64_t adjusted = mantissa - adjustment;
      T halfbit_adjusted = (adjusted >> (shift - 1)) & 1;

      T result =
          static_cast<T>((sign << sign_offset) | (exponent << exponent_offset) |
                         ((mantissa >> shift) << mantissa_offset));

      // A very large mantissa can overflow during rounding. If this happens,
      // the exponent should be incremented and the mantissa set to 1.0
      // (encoded as 0). Applying halfbit_adjusted after assembling the float
      // has the nice side-effect that this case is handled for free.
      //
      // This also handles cases where a very large finite value overflows to
      // infinity, or where a very large subnormal value overflows to become
      // normal.
      return result + halfbit_adjusted;
    } else {
      VIXL_ASSERT(round_mode == FPRoundOdd);
      // If any bits at position halfbit or below are set, onebit (ie. the
      // bottom bit of the resulting mantissa) must be set.
      uint64_t fractional_bits = mantissa & ((UINT64_C(1) << shift) - 1);
      if (fractional_bits != 0) {
        mantissa |= UINT64_C(1) << shift;
      }

      return static_cast<T>((sign << sign_offset) |
                            (exponent << exponent_offset) |
                            ((mantissa >> shift) << mantissa_offset));
    }
  } else {
    // We have to shift the mantissa to the left (or not at all). The input
    // mantissa is exactly representable in the output mantissa, so apply no
    // rounding correction.
    return static_cast<T>((sign << sign_offset) |
                          (exponent << exponent_offset) |
                          ((mantissa << -shift) << mantissa_offset));
  }
}


// See FPRound for a description of this function.
inline double FPRoundToDouble(int64_t sign,
                              int64_t exponent,
                              uint64_t mantissa,
                              FPRounding round_mode) {
  uint64_t bits =
      FPRound<uint64_t, kDoubleExponentBits, kDoubleMantissaBits>(sign,
                                                                  exponent,
                                                                  mantissa,
                                                                  round_mode);
  return RawbitsToDouble(bits);
}


// See FPRound for a description of this function.
inline Float16 FPRoundToFloat16(int64_t sign,
                                int64_t exponent,
                                uint64_t mantissa,
                                FPRounding round_mode) {
  return RawbitsToFloat16(
      FPRound<uint16_t,
              kFloat16ExponentBits,
              kFloat16MantissaBits>(sign, exponent, mantissa, round_mode));
}


// See FPRound for a description of this function.
static inline float FPRoundToFloat(int64_t sign,
                                   int64_t exponent,
                                   uint64_t mantissa,
                                   FPRounding round_mode) {
  uint32_t bits =
      FPRound<uint32_t, kFloatExponentBits, kFloatMantissaBits>(sign,
                                                                exponent,
                                                                mantissa,
                                                                round_mode);
  return RawbitsToFloat(bits);
}


float FPToFloat(Float16 value, UseDefaultNaN DN, bool* exception = NULL);
float FPToFloat(double value,
                FPRounding round_mode,
                UseDefaultNaN DN,
                bool* exception = NULL);

double FPToDouble(Float16 value, UseDefaultNaN DN, bool* exception = NULL);
double FPToDouble(float value, UseDefaultNaN DN, bool* exception = NULL);

Float16 FPToFloat16(float value,
                    FPRounding round_mode,
                    UseDefaultNaN DN,
                    bool* exception = NULL);

Float16 FPToFloat16(double value,
                    FPRounding round_mode,
                    UseDefaultNaN DN,
                    bool* exception = NULL);
}  // namespace vixl

#endif  // VIXL_UTILS_H

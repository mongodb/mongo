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

#include "jit/arm64/vixl/Utils-vixl.h"

#include <cstdio>

namespace vixl {

// The default NaN values (for FPCR.DN=1).
const double kFP64DefaultNaN = RawbitsToDouble(UINT64_C(0x7ff8000000000000));
const float kFP32DefaultNaN = RawbitsToFloat(0x7fc00000);
const Float16 kFP16DefaultNaN = RawbitsToFloat16(0x7e00);

// Floating-point zero values.
const Float16 kFP16PositiveZero = RawbitsToFloat16(0x0);
const Float16 kFP16NegativeZero = RawbitsToFloat16(0x8000);

// Floating-point infinity values.
const Float16 kFP16PositiveInfinity = RawbitsToFloat16(0x7c00);
const Float16 kFP16NegativeInfinity = RawbitsToFloat16(0xfc00);
const float kFP32PositiveInfinity = RawbitsToFloat(0x7f800000);
const float kFP32NegativeInfinity = RawbitsToFloat(0xff800000);
const double kFP64PositiveInfinity =
    RawbitsToDouble(UINT64_C(0x7ff0000000000000));
const double kFP64NegativeInfinity =
    RawbitsToDouble(UINT64_C(0xfff0000000000000));

bool IsZero(Float16 value) {
  uint16_t bits = Float16ToRawbits(value);
  return (bits == Float16ToRawbits(kFP16PositiveZero) ||
          bits == Float16ToRawbits(kFP16NegativeZero));
}

uint16_t Float16ToRawbits(Float16 value) { return value.rawbits_; }

uint32_t FloatToRawbits(float value) {
  uint32_t bits = 0;
  memcpy(&bits, &value, 4);
  return bits;
}


uint64_t DoubleToRawbits(double value) {
  uint64_t bits = 0;
  memcpy(&bits, &value, 8);
  return bits;
}


Float16 RawbitsToFloat16(uint16_t bits) {
  Float16 f;
  f.rawbits_ = bits;
  return f;
}


float RawbitsToFloat(uint32_t bits) {
  float value = 0.0;
  memcpy(&value, &bits, 4);
  return value;
}


double RawbitsToDouble(uint64_t bits) {
  double value = 0.0;
  memcpy(&value, &bits, 8);
  return value;
}


uint32_t Float16Sign(internal::SimFloat16 val) {
  uint16_t rawbits = Float16ToRawbits(val);
  return ExtractUnsignedBitfield32(15, 15, rawbits);
}


uint32_t Float16Exp(internal::SimFloat16 val) {
  uint16_t rawbits = Float16ToRawbits(val);
  return ExtractUnsignedBitfield32(14, 10, rawbits);
}

uint32_t Float16Mantissa(internal::SimFloat16 val) {
  uint16_t rawbits = Float16ToRawbits(val);
  return ExtractUnsignedBitfield32(9, 0, rawbits);
}


uint32_t FloatSign(float val) {
  uint32_t rawbits = FloatToRawbits(val);
  return ExtractUnsignedBitfield32(31, 31, rawbits);
}


uint32_t FloatExp(float val) {
  uint32_t rawbits = FloatToRawbits(val);
  return ExtractUnsignedBitfield32(30, 23, rawbits);
}


uint32_t FloatMantissa(float val) {
  uint32_t rawbits = FloatToRawbits(val);
  return ExtractUnsignedBitfield32(22, 0, rawbits);
}


uint32_t DoubleSign(double val) {
  uint64_t rawbits = DoubleToRawbits(val);
  return static_cast<uint32_t>(ExtractUnsignedBitfield64(63, 63, rawbits));
}


uint32_t DoubleExp(double val) {
  uint64_t rawbits = DoubleToRawbits(val);
  return static_cast<uint32_t>(ExtractUnsignedBitfield64(62, 52, rawbits));
}


uint64_t DoubleMantissa(double val) {
  uint64_t rawbits = DoubleToRawbits(val);
  return ExtractUnsignedBitfield64(51, 0, rawbits);
}


internal::SimFloat16 Float16Pack(uint16_t sign,
                                 uint16_t exp,
                                 uint16_t mantissa) {
  uint16_t bits = (sign << 15) | (exp << 10) | mantissa;
  return RawbitsToFloat16(bits);
}


float FloatPack(uint32_t sign, uint32_t exp, uint32_t mantissa) {
  uint32_t bits = (sign << 31) | (exp << 23) | mantissa;
  return RawbitsToFloat(bits);
}


double DoublePack(uint64_t sign, uint64_t exp, uint64_t mantissa) {
  uint64_t bits = (sign << 63) | (exp << 52) | mantissa;
  return RawbitsToDouble(bits);
}


int Float16Classify(Float16 value) {
  uint16_t bits = Float16ToRawbits(value);
  uint16_t exponent_max = (1 << 5) - 1;
  uint16_t exponent_mask = exponent_max << 10;
  uint16_t mantissa_mask = (1 << 10) - 1;

  uint16_t exponent = (bits & exponent_mask) >> 10;
  uint16_t mantissa = bits & mantissa_mask;
  if (exponent == 0) {
    if (mantissa == 0) {
      return FP_ZERO;
    }
    return FP_SUBNORMAL;
  } else if (exponent == exponent_max) {
    if (mantissa == 0) {
      return FP_INFINITE;
    }
    return FP_NAN;
  }
  return FP_NORMAL;
}


unsigned CountClearHalfWords(uint64_t imm, unsigned reg_size) {
  VIXL_ASSERT((reg_size % 8) == 0);
  int count = 0;
  for (unsigned i = 0; i < (reg_size / 16); i++) {
    if ((imm & 0xffff) == 0) {
      count++;
    }
    imm >>= 16;
  }
  return count;
}


int BitCount(uint64_t value) { return CountSetBits(value); }

// Float16 definitions.

Float16::Float16(double dvalue) {
  rawbits_ =
      Float16ToRawbits(FPToFloat16(dvalue, FPTieEven, kIgnoreDefaultNaN));
}

namespace internal {

SimFloat16 SimFloat16::operator-() const {
  return RawbitsToFloat16(rawbits_ ^ 0x8000);
}

// SimFloat16 definitions.
SimFloat16 SimFloat16::operator+(SimFloat16 rhs) const {
  return static_cast<double>(*this) + static_cast<double>(rhs);
}

SimFloat16 SimFloat16::operator-(SimFloat16 rhs) const {
  return static_cast<double>(*this) - static_cast<double>(rhs);
}

SimFloat16 SimFloat16::operator*(SimFloat16 rhs) const {
  return static_cast<double>(*this) * static_cast<double>(rhs);
}

SimFloat16 SimFloat16::operator/(SimFloat16 rhs) const {
  return static_cast<double>(*this) / static_cast<double>(rhs);
}

bool SimFloat16::operator<(SimFloat16 rhs) const {
  return static_cast<double>(*this) < static_cast<double>(rhs);
}

bool SimFloat16::operator>(SimFloat16 rhs) const {
  return static_cast<double>(*this) > static_cast<double>(rhs);
}

bool SimFloat16::operator==(SimFloat16 rhs) const {
  if (IsNaN(*this) || IsNaN(rhs)) {
    return false;
  } else if (IsZero(rhs) && IsZero(*this)) {
    // +0 and -0 should be treated as equal.
    return true;
  }
  return this->rawbits_ == rhs.rawbits_;
}

bool SimFloat16::operator!=(SimFloat16 rhs) const { return !(*this == rhs); }

bool SimFloat16::operator==(double rhs) const {
  return static_cast<double>(*this) == static_cast<double>(rhs);
}

SimFloat16::operator double() const {
  return FPToDouble(*this, kIgnoreDefaultNaN);
}

Int64 BitCount(Uint32 value) { return CountSetBits(value.Get()); }

}  // namespace internal

float FPToFloat(Float16 value, UseDefaultNaN DN, bool* exception) {
  uint16_t bits = Float16ToRawbits(value);
  uint32_t sign = bits >> 15;
  uint32_t exponent =
      ExtractUnsignedBitfield32(kFloat16MantissaBits + kFloat16ExponentBits - 1,
                                kFloat16MantissaBits,
                                bits);
  uint32_t mantissa =
      ExtractUnsignedBitfield32(kFloat16MantissaBits - 1, 0, bits);

  switch (Float16Classify(value)) {
    case FP_ZERO:
      return (sign == 0) ? 0.0f : -0.0f;

    case FP_INFINITE:
      return (sign == 0) ? kFP32PositiveInfinity : kFP32NegativeInfinity;

    case FP_SUBNORMAL: {
      // Calculate shift required to put mantissa into the most-significant bits
      // of the destination mantissa.
      int shift = CountLeadingZeros(mantissa << (32 - 10));

      // Shift mantissa and discard implicit '1'.
      mantissa <<= (kFloatMantissaBits - kFloat16MantissaBits) + shift + 1;
      mantissa &= (1 << kFloatMantissaBits) - 1;

      // Adjust the exponent for the shift applied, and rebias.
      exponent = exponent - shift + (-15 + 127);
      break;
    }

    case FP_NAN:
      if (IsSignallingNaN(value)) {
        if (exception != NULL) {
          *exception = true;
        }
      }
      if (DN == kUseDefaultNaN) return kFP32DefaultNaN;

      // Convert NaNs as the processor would:
      //  - The sign is propagated.
      //  - The payload (mantissa) is transferred entirely, except that the top
      //    bit is forced to '1', making the result a quiet NaN. The unused
      //    (low-order) payload bits are set to 0.
      exponent = (1 << kFloatExponentBits) - 1;

      // Increase bits in mantissa, making low-order bits 0.
      mantissa <<= (kFloatMantissaBits - kFloat16MantissaBits);
      mantissa |= 1 << 22;  // Force a quiet NaN.
      break;

    case FP_NORMAL:
      // Increase bits in mantissa, making low-order bits 0.
      mantissa <<= (kFloatMantissaBits - kFloat16MantissaBits);

      // Change exponent bias.
      exponent += (-15 + 127);
      break;

    default:
      VIXL_UNREACHABLE();
  }
  return RawbitsToFloat((sign << 31) | (exponent << kFloatMantissaBits) |
                        mantissa);
}


float FPToFloat(double value,
                FPRounding round_mode,
                UseDefaultNaN DN,
                bool* exception) {
  // Only the FPTieEven rounding mode is implemented.
  VIXL_ASSERT((round_mode == FPTieEven) || (round_mode == FPRoundOdd));
  USE(round_mode);

  switch (std::fpclassify(value)) {
    case FP_NAN: {
      if (IsSignallingNaN(value)) {
        if (exception != NULL) {
          *exception = true;
        }
      }
      if (DN == kUseDefaultNaN) return kFP32DefaultNaN;

      // Convert NaNs as the processor would:
      //  - The sign is propagated.
      //  - The payload (mantissa) is transferred as much as possible, except
      //    that the top bit is forced to '1', making the result a quiet NaN.
      uint64_t raw = DoubleToRawbits(value);

      uint32_t sign = raw >> 63;
      uint32_t exponent = (1 << 8) - 1;
      uint32_t payload =
          static_cast<uint32_t>(ExtractUnsignedBitfield64(50, 52 - 23, raw));
      payload |= (1 << 22);  // Force a quiet NaN.

      return RawbitsToFloat((sign << 31) | (exponent << 23) | payload);
    }

    case FP_ZERO:
    case FP_INFINITE: {
      // In a C++ cast, any value representable in the target type will be
      // unchanged. This is always the case for +/-0.0 and infinities.
      return static_cast<float>(value);
    }

    case FP_NORMAL:
    case FP_SUBNORMAL: {
      // Convert double-to-float as the processor would, assuming that FPCR.FZ
      // (flush-to-zero) is not set.
      uint64_t raw = DoubleToRawbits(value);
      // Extract the IEEE-754 double components.
      uint32_t sign = raw >> 63;
      // Extract the exponent and remove the IEEE-754 encoding bias.
      int32_t exponent =
          static_cast<int32_t>(ExtractUnsignedBitfield64(62, 52, raw)) - 1023;
      // Extract the mantissa and add the implicit '1' bit.
      uint64_t mantissa = ExtractUnsignedBitfield64(51, 0, raw);
      if (std::fpclassify(value) == FP_NORMAL) {
        mantissa |= (UINT64_C(1) << 52);
      }
      return FPRoundToFloat(sign, exponent, mantissa, round_mode);
    }
  }

  VIXL_UNREACHABLE();
  return value;
}

// TODO: We should consider implementing a full FPToDouble(Float16)
// conversion function (for performance reasons).
double FPToDouble(Float16 value, UseDefaultNaN DN, bool* exception) {
  // We can rely on implicit float to double conversion here.
  return FPToFloat(value, DN, exception);
}


double FPToDouble(float value, UseDefaultNaN DN, bool* exception) {
  switch (std::fpclassify(value)) {
    case FP_NAN: {
      if (IsSignallingNaN(value)) {
        if (exception != NULL) {
          *exception = true;
        }
      }
      if (DN == kUseDefaultNaN) return kFP64DefaultNaN;

      // Convert NaNs as the processor would:
      //  - The sign is propagated.
      //  - The payload (mantissa) is transferred entirely, except that the top
      //    bit is forced to '1', making the result a quiet NaN. The unused
      //    (low-order) payload bits are set to 0.
      uint32_t raw = FloatToRawbits(value);

      uint64_t sign = raw >> 31;
      uint64_t exponent = (1 << 11) - 1;
      uint64_t payload = ExtractUnsignedBitfield64(21, 0, raw);
      payload <<= (52 - 23);           // The unused low-order bits should be 0.
      payload |= (UINT64_C(1) << 51);  // Force a quiet NaN.

      return RawbitsToDouble((sign << 63) | (exponent << 52) | payload);
    }

    case FP_ZERO:
    case FP_NORMAL:
    case FP_SUBNORMAL:
    case FP_INFINITE: {
      // All other inputs are preserved in a standard cast, because every value
      // representable using an IEEE-754 float is also representable using an
      // IEEE-754 double.
      return static_cast<double>(value);
    }
  }

  VIXL_UNREACHABLE();
  return static_cast<double>(value);
}


Float16 FPToFloat16(float value,
                    FPRounding round_mode,
                    UseDefaultNaN DN,
                    bool* exception) {
  // Only the FPTieEven rounding mode is implemented.
  VIXL_ASSERT(round_mode == FPTieEven);
  USE(round_mode);

  uint32_t raw = FloatToRawbits(value);
  int32_t sign = raw >> 31;
  int32_t exponent = ExtractUnsignedBitfield32(30, 23, raw) - 127;
  uint32_t mantissa = ExtractUnsignedBitfield32(22, 0, raw);

  switch (std::fpclassify(value)) {
    case FP_NAN: {
      if (IsSignallingNaN(value)) {
        if (exception != NULL) {
          *exception = true;
        }
      }
      if (DN == kUseDefaultNaN) return kFP16DefaultNaN;

      // Convert NaNs as the processor would:
      //  - The sign is propagated.
      //  - The payload (mantissa) is transferred as much as possible, except
      //    that the top bit is forced to '1', making the result a quiet NaN.
      uint16_t result = (sign == 0) ? Float16ToRawbits(kFP16PositiveInfinity)
                                    : Float16ToRawbits(kFP16NegativeInfinity);
      result |= mantissa >> (kFloatMantissaBits - kFloat16MantissaBits);
      result |= (1 << 9);  // Force a quiet NaN;
      return RawbitsToFloat16(result);
    }

    case FP_ZERO:
      return (sign == 0) ? kFP16PositiveZero : kFP16NegativeZero;

    case FP_INFINITE:
      return (sign == 0) ? kFP16PositiveInfinity : kFP16NegativeInfinity;

    case FP_NORMAL:
    case FP_SUBNORMAL: {
      // Convert float-to-half as the processor would, assuming that FPCR.FZ
      // (flush-to-zero) is not set.

      // Add the implicit '1' bit to the mantissa.
      mantissa += (1 << 23);
      return FPRoundToFloat16(sign, exponent, mantissa, round_mode);
    }
  }

  VIXL_UNREACHABLE();
  return kFP16PositiveZero;
}


Float16 FPToFloat16(double value,
                    FPRounding round_mode,
                    UseDefaultNaN DN,
                    bool* exception) {
  // Only the FPTieEven rounding mode is implemented.
  VIXL_ASSERT(round_mode == FPTieEven);
  USE(round_mode);

  uint64_t raw = DoubleToRawbits(value);
  int32_t sign = raw >> 63;
  int64_t exponent = ExtractUnsignedBitfield64(62, 52, raw) - 1023;
  uint64_t mantissa = ExtractUnsignedBitfield64(51, 0, raw);

  switch (std::fpclassify(value)) {
    case FP_NAN: {
      if (IsSignallingNaN(value)) {
        if (exception != NULL) {
          *exception = true;
        }
      }
      if (DN == kUseDefaultNaN) return kFP16DefaultNaN;

      // Convert NaNs as the processor would:
      //  - The sign is propagated.
      //  - The payload (mantissa) is transferred as much as possible, except
      //    that the top bit is forced to '1', making the result a quiet NaN.
      uint16_t result = (sign == 0) ? Float16ToRawbits(kFP16PositiveInfinity)
                                    : Float16ToRawbits(kFP16NegativeInfinity);
      result |= mantissa >> (kDoubleMantissaBits - kFloat16MantissaBits);
      result |= (1 << 9);  // Force a quiet NaN;
      return RawbitsToFloat16(result);
    }

    case FP_ZERO:
      return (sign == 0) ? kFP16PositiveZero : kFP16NegativeZero;

    case FP_INFINITE:
      return (sign == 0) ? kFP16PositiveInfinity : kFP16NegativeInfinity;
    case FP_NORMAL:
    case FP_SUBNORMAL: {
      // Convert double-to-half as the processor would, assuming that FPCR.FZ
      // (flush-to-zero) is not set.

      // Add the implicit '1' bit to the mantissa.
      mantissa += (UINT64_C(1) << 52);
      return FPRoundToFloat16(sign, exponent, mantissa, round_mode);
    }
  }

  VIXL_UNREACHABLE();
  return kFP16PositiveZero;
}

}  // namespace vixl

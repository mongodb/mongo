// Copyright 2015, ARM Limited
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

#ifdef JS_SIMULATOR_ARM64

#include <cmath>

#include "jit/arm64/vixl/Simulator-vixl.h"

namespace vixl {

template<> double Simulator::FPDefaultNaN<double>() {
  return kFP64DefaultNaN;
}


template<> float Simulator::FPDefaultNaN<float>() {
  return kFP32DefaultNaN;
}

// See FPRound for a description of this function.
static inline double FPRoundToDouble(int64_t sign, int64_t exponent,
                                     uint64_t mantissa, FPRounding round_mode) {
  int64_t bits =
      FPRound<int64_t, kDoubleExponentBits, kDoubleMantissaBits>(sign,
                                                                 exponent,
                                                                 mantissa,
                                                                 round_mode);
  return rawbits_to_double(bits);
}


// See FPRound for a description of this function.
static inline float FPRoundToFloat(int64_t sign, int64_t exponent,
                                   uint64_t mantissa, FPRounding round_mode) {
  int32_t bits =
      FPRound<int32_t, kFloatExponentBits, kFloatMantissaBits>(sign,
                                                               exponent,
                                                               mantissa,
                                                               round_mode);
  return rawbits_to_float(bits);
}


// See FPRound for a description of this function.
static inline float16 FPRoundToFloat16(int64_t sign,
                                       int64_t exponent,
                                       uint64_t mantissa,
                                       FPRounding round_mode) {
  return FPRound<float16, kFloat16ExponentBits, kFloat16MantissaBits>(
      sign, exponent, mantissa, round_mode);
}


double Simulator::FixedToDouble(int64_t src, int fbits, FPRounding round) {
  if (src >= 0) {
    return UFixedToDouble(src, fbits, round);
  } else {
    // This works for all negative values, including INT64_MIN.
    return -UFixedToDouble(-src, fbits, round);
  }
}


double Simulator::UFixedToDouble(uint64_t src, int fbits, FPRounding round) {
  // An input of 0 is a special case because the result is effectively
  // subnormal: The exponent is encoded as 0 and there is no implicit 1 bit.
  if (src == 0) {
    return 0.0;
  }

  // Calculate the exponent. The highest significant bit will have the value
  // 2^exponent.
  const int highest_significant_bit = 63 - CountLeadingZeros(src);
  const int64_t exponent = highest_significant_bit - fbits;

  return FPRoundToDouble(0, exponent, src, round);
}


float Simulator::FixedToFloat(int64_t src, int fbits, FPRounding round) {
  if (src >= 0) {
    return UFixedToFloat(src, fbits, round);
  } else {
    // This works for all negative values, including INT64_MIN.
    return -UFixedToFloat(-src, fbits, round);
  }
}


float Simulator::UFixedToFloat(uint64_t src, int fbits, FPRounding round) {
  // An input of 0 is a special case because the result is effectively
  // subnormal: The exponent is encoded as 0 and there is no implicit 1 bit.
  if (src == 0) {
    return 0.0f;
  }

  // Calculate the exponent. The highest significant bit will have the value
  // 2^exponent.
  const int highest_significant_bit = 63 - CountLeadingZeros(src);
  const int32_t exponent = highest_significant_bit - fbits;

  return FPRoundToFloat(0, exponent, src, round);
}


double Simulator::FPToDouble(float value) {
  switch (std::fpclassify(value)) {
    case FP_NAN: {
      if (IsSignallingNaN(value)) {
        FPProcessException();
      }
      if (DN()) return kFP64DefaultNaN;

      // Convert NaNs as the processor would:
      //  - The sign is propagated.
      //  - The payload (mantissa) is transferred entirely, except that the top
      //    bit is forced to '1', making the result a quiet NaN. The unused
      //    (low-order) payload bits are set to 0.
      uint32_t raw = float_to_rawbits(value);

      uint64_t sign = raw >> 31;
      uint64_t exponent = (1 << 11) - 1;
      uint64_t payload = unsigned_bitextract_64(21, 0, raw);
      payload <<= (52 - 23);  // The unused low-order bits should be 0.
      payload |= (UINT64_C(1) << 51);  // Force a quiet NaN.

      return rawbits_to_double((sign << 63) | (exponent << 52) | payload);
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


float Simulator::FPToFloat(float16 value) {
  uint32_t sign = value >> 15;
  uint32_t exponent = unsigned_bitextract_32(
      kFloat16MantissaBits + kFloat16ExponentBits - 1, kFloat16MantissaBits,
      value);
  uint32_t mantissa = unsigned_bitextract_32(
      kFloat16MantissaBits - 1, 0, value);

  switch (float16classify(value)) {
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
        FPProcessException();
      }
      if (DN()) return kFP32DefaultNaN;

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

    default: VIXL_UNREACHABLE();
  }
  return rawbits_to_float((sign << 31) |
                          (exponent << kFloatMantissaBits) |
                          mantissa);
}


float16 Simulator::FPToFloat16(float value, FPRounding round_mode) {
  // Only the FPTieEven rounding mode is implemented.
  VIXL_ASSERT(round_mode == FPTieEven);
  USE(round_mode);

  uint32_t raw = float_to_rawbits(value);
  int32_t sign = raw >> 31;
  int32_t exponent = unsigned_bitextract_32(30, 23, raw) - 127;
  uint32_t mantissa = unsigned_bitextract_32(22, 0, raw);

  switch (std::fpclassify(value)) {
    case FP_NAN: {
      if (IsSignallingNaN(value)) {
        FPProcessException();
      }
      if (DN()) return kFP16DefaultNaN;

      // Convert NaNs as the processor would:
      //  - The sign is propagated.
      //  - The payload (mantissa) is transferred as much as possible, except
      //    that the top bit is forced to '1', making the result a quiet NaN.
      float16 result = (sign == 0) ? kFP16PositiveInfinity
                                   : kFP16NegativeInfinity;
      result |= mantissa >> (kFloatMantissaBits - kFloat16MantissaBits);
      result |= (1 << 9);  // Force a quiet NaN;
      return result;
    }

    case FP_ZERO:
      return (sign == 0) ? 0 : 0x8000;

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
  return 0;
}


float16 Simulator::FPToFloat16(double value, FPRounding round_mode) {
  // Only the FPTieEven rounding mode is implemented.
  VIXL_ASSERT(round_mode == FPTieEven);
  USE(round_mode);

  uint64_t raw = double_to_rawbits(value);
  int32_t sign = raw >> 63;
  int64_t exponent = unsigned_bitextract_64(62, 52, raw) - 1023;
  uint64_t mantissa = unsigned_bitextract_64(51, 0, raw);

  switch (std::fpclassify(value)) {
    case FP_NAN: {
      if (IsSignallingNaN(value)) {
        FPProcessException();
      }
      if (DN()) return kFP16DefaultNaN;

      // Convert NaNs as the processor would:
      //  - The sign is propagated.
      //  - The payload (mantissa) is transferred as much as possible, except
      //    that the top bit is forced to '1', making the result a quiet NaN.
      float16 result = (sign == 0) ? kFP16PositiveInfinity
                                   : kFP16NegativeInfinity;
      result |= mantissa >> (kDoubleMantissaBits - kFloat16MantissaBits);
      result |= (1 << 9);  // Force a quiet NaN;
      return result;
    }

    case FP_ZERO:
      return (sign == 0) ? 0 : 0x8000;

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
  return 0;
}


float Simulator::FPToFloat(double value, FPRounding round_mode) {
  // Only the FPTieEven rounding mode is implemented.
  VIXL_ASSERT((round_mode == FPTieEven) || (round_mode == FPRoundOdd));
  USE(round_mode);

  switch (std::fpclassify(value)) {
    case FP_NAN: {
      if (IsSignallingNaN(value)) {
        FPProcessException();
      }
      if (DN()) return kFP32DefaultNaN;

      // Convert NaNs as the processor would:
      //  - The sign is propagated.
      //  - The payload (mantissa) is transferred as much as possible, except
      //    that the top bit is forced to '1', making the result a quiet NaN.
      uint64_t raw = double_to_rawbits(value);

      uint32_t sign = raw >> 63;
      uint32_t exponent = (1 << 8) - 1;
      uint32_t payload =
          static_cast<uint32_t>(unsigned_bitextract_64(50, 52 - 23, raw));
      payload |= (1 << 22);   // Force a quiet NaN.

      return rawbits_to_float((sign << 31) | (exponent << 23) | payload);
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
      uint64_t raw = double_to_rawbits(value);
      // Extract the IEEE-754 double components.
      uint32_t sign = raw >> 63;
      // Extract the exponent and remove the IEEE-754 encoding bias.
      int32_t exponent =
          static_cast<int32_t>(unsigned_bitextract_64(62, 52, raw)) - 1023;
      // Extract the mantissa and add the implicit '1' bit.
      uint64_t mantissa = unsigned_bitextract_64(51, 0, raw);
      if (std::fpclassify(value) == FP_NORMAL) {
        mantissa |= (UINT64_C(1) << 52);
      }
      return FPRoundToFloat(sign, exponent, mantissa, round_mode);
    }
  }

  VIXL_UNREACHABLE();
  return value;
}


void Simulator::ld1(VectorFormat vform,
                    LogicVRegister dst,
                    uint64_t addr) {
  dst.ClearForWrite(vform);
  for (int i = 0; i < LaneCountFromFormat(vform); i++) {
    dst.ReadUintFromMem(vform, i, addr);
    addr += LaneSizeInBytesFromFormat(vform);
  }
}


void Simulator::ld1(VectorFormat vform,
                    LogicVRegister dst,
                    int index,
                    uint64_t addr) {
  dst.ReadUintFromMem(vform, index, addr);
}


void Simulator::ld1r(VectorFormat vform,
                     LogicVRegister dst,
                     uint64_t addr) {
  dst.ClearForWrite(vform);
  for (int i = 0; i < LaneCountFromFormat(vform); i++) {
    dst.ReadUintFromMem(vform, i, addr);
  }
}


void Simulator::ld2(VectorFormat vform,
                    LogicVRegister dst1,
                    LogicVRegister dst2,
                    uint64_t addr1) {
  dst1.ClearForWrite(vform);
  dst2.ClearForWrite(vform);
  int esize = LaneSizeInBytesFromFormat(vform);
  uint64_t addr2 = addr1 + esize;
  for (int i = 0; i < LaneCountFromFormat(vform); i++) {
    dst1.ReadUintFromMem(vform, i, addr1);
    dst2.ReadUintFromMem(vform, i, addr2);
    addr1 += 2 * esize;
    addr2 += 2 * esize;
  }
}


void Simulator::ld2(VectorFormat vform,
                    LogicVRegister dst1,
                    LogicVRegister dst2,
                    int index,
                    uint64_t addr1) {
  dst1.ClearForWrite(vform);
  dst2.ClearForWrite(vform);
  uint64_t addr2 = addr1 + LaneSizeInBytesFromFormat(vform);
  dst1.ReadUintFromMem(vform, index, addr1);
  dst2.ReadUintFromMem(vform, index, addr2);
}


void Simulator::ld2r(VectorFormat vform,
                     LogicVRegister dst1,
                     LogicVRegister dst2,
                     uint64_t addr) {
  dst1.ClearForWrite(vform);
  dst2.ClearForWrite(vform);
  uint64_t addr2 = addr + LaneSizeInBytesFromFormat(vform);
  for (int i = 0; i < LaneCountFromFormat(vform); i++) {
    dst1.ReadUintFromMem(vform, i, addr);
    dst2.ReadUintFromMem(vform, i, addr2);
  }
}


void Simulator::ld3(VectorFormat vform,
                    LogicVRegister dst1,
                    LogicVRegister dst2,
                    LogicVRegister dst3,
                    uint64_t addr1) {
  dst1.ClearForWrite(vform);
  dst2.ClearForWrite(vform);
  dst3.ClearForWrite(vform);
  int esize = LaneSizeInBytesFromFormat(vform);
  uint64_t addr2 = addr1 + esize;
  uint64_t addr3 = addr2 + esize;
  for (int i = 0; i < LaneCountFromFormat(vform); i++) {
    dst1.ReadUintFromMem(vform, i, addr1);
    dst2.ReadUintFromMem(vform, i, addr2);
    dst3.ReadUintFromMem(vform, i, addr3);
    addr1 += 3 * esize;
    addr2 += 3 * esize;
    addr3 += 3 * esize;
  }
}


void Simulator::ld3(VectorFormat vform,
                    LogicVRegister dst1,
                    LogicVRegister dst2,
                    LogicVRegister dst3,
                    int index,
                    uint64_t addr1) {
  dst1.ClearForWrite(vform);
  dst2.ClearForWrite(vform);
  dst3.ClearForWrite(vform);
  uint64_t addr2 = addr1 + LaneSizeInBytesFromFormat(vform);
  uint64_t addr3 = addr2 + LaneSizeInBytesFromFormat(vform);
  dst1.ReadUintFromMem(vform, index, addr1);
  dst2.ReadUintFromMem(vform, index, addr2);
  dst3.ReadUintFromMem(vform, index, addr3);
}


void Simulator::ld3r(VectorFormat vform,
                     LogicVRegister dst1,
                     LogicVRegister dst2,
                     LogicVRegister dst3,
                     uint64_t addr) {
  dst1.ClearForWrite(vform);
  dst2.ClearForWrite(vform);
  dst3.ClearForWrite(vform);
  uint64_t addr2 = addr + LaneSizeInBytesFromFormat(vform);
  uint64_t addr3 = addr2 + LaneSizeInBytesFromFormat(vform);
  for (int i = 0; i < LaneCountFromFormat(vform); i++) {
    dst1.ReadUintFromMem(vform, i, addr);
    dst2.ReadUintFromMem(vform, i, addr2);
    dst3.ReadUintFromMem(vform, i, addr3);
  }
}


void Simulator::ld4(VectorFormat vform,
                    LogicVRegister dst1,
                    LogicVRegister dst2,
                    LogicVRegister dst3,
                    LogicVRegister dst4,
                    uint64_t addr1) {
  dst1.ClearForWrite(vform);
  dst2.ClearForWrite(vform);
  dst3.ClearForWrite(vform);
  dst4.ClearForWrite(vform);
  int esize = LaneSizeInBytesFromFormat(vform);
  uint64_t addr2 = addr1 + esize;
  uint64_t addr3 = addr2 + esize;
  uint64_t addr4 = addr3 + esize;
  for (int i = 0; i < LaneCountFromFormat(vform); i++) {
    dst1.ReadUintFromMem(vform, i, addr1);
    dst2.ReadUintFromMem(vform, i, addr2);
    dst3.ReadUintFromMem(vform, i, addr3);
    dst4.ReadUintFromMem(vform, i, addr4);
    addr1 += 4 * esize;
    addr2 += 4 * esize;
    addr3 += 4 * esize;
    addr4 += 4 * esize;
  }
}


void Simulator::ld4(VectorFormat vform,
                    LogicVRegister dst1,
                    LogicVRegister dst2,
                    LogicVRegister dst3,
                    LogicVRegister dst4,
                    int index,
                    uint64_t addr1) {
  dst1.ClearForWrite(vform);
  dst2.ClearForWrite(vform);
  dst3.ClearForWrite(vform);
  dst4.ClearForWrite(vform);
  uint64_t addr2 = addr1 + LaneSizeInBytesFromFormat(vform);
  uint64_t addr3 = addr2 + LaneSizeInBytesFromFormat(vform);
  uint64_t addr4 = addr3 + LaneSizeInBytesFromFormat(vform);
  dst1.ReadUintFromMem(vform, index, addr1);
  dst2.ReadUintFromMem(vform, index, addr2);
  dst3.ReadUintFromMem(vform, index, addr3);
  dst4.ReadUintFromMem(vform, index, addr4);
}


void Simulator::ld4r(VectorFormat vform,
                     LogicVRegister dst1,
                     LogicVRegister dst2,
                     LogicVRegister dst3,
                     LogicVRegister dst4,
                     uint64_t addr) {
  dst1.ClearForWrite(vform);
  dst2.ClearForWrite(vform);
  dst3.ClearForWrite(vform);
  dst4.ClearForWrite(vform);
  uint64_t addr2 = addr + LaneSizeInBytesFromFormat(vform);
  uint64_t addr3 = addr2 + LaneSizeInBytesFromFormat(vform);
  uint64_t addr4 = addr3 + LaneSizeInBytesFromFormat(vform);
  for (int i = 0; i < LaneCountFromFormat(vform); i++) {
    dst1.ReadUintFromMem(vform, i, addr);
    dst2.ReadUintFromMem(vform, i, addr2);
    dst3.ReadUintFromMem(vform, i, addr3);
    dst4.ReadUintFromMem(vform, i, addr4);
  }
}


void Simulator::st1(VectorFormat vform,
                    LogicVRegister src,
                    uint64_t addr) {
  for (int i = 0; i < LaneCountFromFormat(vform); i++) {
    src.WriteUintToMem(vform, i, addr);
    addr += LaneSizeInBytesFromFormat(vform);
  }
}


void Simulator::st1(VectorFormat vform,
                    LogicVRegister src,
                    int index,
                    uint64_t addr) {
  src.WriteUintToMem(vform, index, addr);
}


void Simulator::st2(VectorFormat vform,
                    LogicVRegister dst,
                    LogicVRegister dst2,
                    uint64_t addr) {
  int esize = LaneSizeInBytesFromFormat(vform);
  uint64_t addr2 = addr + esize;
  for (int i = 0; i < LaneCountFromFormat(vform); i++) {
    dst.WriteUintToMem(vform, i, addr);
    dst2.WriteUintToMem(vform, i, addr2);
    addr += 2 * esize;
    addr2 += 2 * esize;
  }
}


void Simulator::st2(VectorFormat vform,
                    LogicVRegister dst,
                    LogicVRegister dst2,
                    int index,
                    uint64_t addr) {
  int esize = LaneSizeInBytesFromFormat(vform);
  dst.WriteUintToMem(vform, index, addr);
  dst2.WriteUintToMem(vform, index, addr + 1 * esize);
}


void Simulator::st3(VectorFormat vform,
                    LogicVRegister dst,
                    LogicVRegister dst2,
                    LogicVRegister dst3,
                    uint64_t addr) {
  int esize = LaneSizeInBytesFromFormat(vform);
  uint64_t addr2 = addr + esize;
  uint64_t addr3 = addr2 + esize;
  for (int i = 0; i < LaneCountFromFormat(vform); i++) {
    dst.WriteUintToMem(vform, i, addr);
    dst2.WriteUintToMem(vform, i, addr2);
    dst3.WriteUintToMem(vform, i, addr3);
    addr += 3 * esize;
    addr2 += 3 * esize;
    addr3 += 3 * esize;
  }
}


void Simulator::st3(VectorFormat vform,
                    LogicVRegister dst,
                    LogicVRegister dst2,
                    LogicVRegister dst3,
                    int index,
                    uint64_t addr) {
  int esize = LaneSizeInBytesFromFormat(vform);
  dst.WriteUintToMem(vform, index, addr);
  dst2.WriteUintToMem(vform, index, addr + 1 * esize);
  dst3.WriteUintToMem(vform, index, addr + 2 * esize);
}


void Simulator::st4(VectorFormat vform,
                    LogicVRegister dst,
                    LogicVRegister dst2,
                    LogicVRegister dst3,
                    LogicVRegister dst4,
                    uint64_t addr) {
  int esize = LaneSizeInBytesFromFormat(vform);
  uint64_t addr2 = addr + esize;
  uint64_t addr3 = addr2 + esize;
  uint64_t addr4 = addr3 + esize;
  for (int i = 0; i < LaneCountFromFormat(vform); i++) {
    dst.WriteUintToMem(vform, i, addr);
    dst2.WriteUintToMem(vform, i, addr2);
    dst3.WriteUintToMem(vform, i, addr3);
    dst4.WriteUintToMem(vform, i, addr4);
    addr += 4 * esize;
    addr2 += 4 * esize;
    addr3 += 4 * esize;
    addr4 += 4 * esize;
  }
}


void Simulator::st4(VectorFormat vform,
                    LogicVRegister dst,
                    LogicVRegister dst2,
                    LogicVRegister dst3,
                    LogicVRegister dst4,
                    int index,
                    uint64_t addr) {
  int esize = LaneSizeInBytesFromFormat(vform);
  dst.WriteUintToMem(vform, index, addr);
  dst2.WriteUintToMem(vform, index, addr + 1 * esize);
  dst3.WriteUintToMem(vform, index, addr + 2 * esize);
  dst4.WriteUintToMem(vform, index, addr + 3 * esize);
}


LogicVRegister Simulator::cmp(VectorFormat vform,
                              LogicVRegister dst,
                              const LogicVRegister& src1,
                              const LogicVRegister& src2,
                              Condition cond) {
  dst.ClearForWrite(vform);
  for (int i = 0; i < LaneCountFromFormat(vform); i++) {
    int64_t  sa = src1.Int(vform, i);
    int64_t  sb = src2.Int(vform, i);
    uint64_t ua = src1.Uint(vform, i);
    uint64_t ub = src2.Uint(vform, i);
    bool result = false;
    switch (cond) {
      case eq: result = (ua == ub); break;
      case ge: result = (sa >= sb); break;
      case gt: result = (sa > sb) ; break;
      case hi: result = (ua > ub) ; break;
      case hs: result = (ua >= ub); break;
      case lt: result = (sa < sb) ; break;
      case le: result = (sa <= sb); break;
      default: VIXL_UNREACHABLE(); break;
    }
    dst.SetUint(vform, i, result ? MaxUintFromFormat(vform) : 0);
  }
  return dst;
}


LogicVRegister Simulator::cmp(VectorFormat vform,
                              LogicVRegister dst,
                              const LogicVRegister& src1,
                              int imm,
                              Condition cond) {
  SimVRegister temp;
  LogicVRegister imm_reg = dup_immediate(vform, temp, imm);
  return cmp(vform, dst, src1, imm_reg, cond);
}


LogicVRegister Simulator::cmptst(VectorFormat vform,
                                 LogicVRegister dst,
                                 const LogicVRegister& src1,
                                 const LogicVRegister& src2) {
  dst.ClearForWrite(vform);
  for (int i = 0; i < LaneCountFromFormat(vform); i++) {
    uint64_t ua = src1.Uint(vform, i);
    uint64_t ub = src2.Uint(vform, i);
    dst.SetUint(vform, i, ((ua & ub) != 0) ? MaxUintFromFormat(vform) : 0);
  }
  return dst;
}


LogicVRegister Simulator::add(VectorFormat vform,
                              LogicVRegister dst,
                              const LogicVRegister& src1,
                              const LogicVRegister& src2) {
  dst.ClearForWrite(vform);
  // TODO(all): consider assigning the result of LaneCountFromFormat to a local.
  for (int i = 0; i < LaneCountFromFormat(vform); i++) {
    // Test for unsigned saturation.
    uint64_t ua = src1.UintLeftJustified(vform, i);
    uint64_t ub = src2.UintLeftJustified(vform, i);
    uint64_t ur = ua + ub;
    if (ur < ua) {
      dst.SetUnsignedSat(i, true);
    }

    // Test for signed saturation.
    int64_t sa = src1.IntLeftJustified(vform, i);
    int64_t sb = src2.IntLeftJustified(vform, i);
    int64_t sr = sa + sb;
    // If the signs of the operands are the same, but different from the result,
    // there was an overflow.
    if (((sa >= 0) == (sb >= 0)) && ((sa >= 0) != (sr >= 0))) {
      dst.SetSignedSat(i, sa >= 0);
    }

    dst.SetInt(vform, i, src1.Int(vform, i) + src2.Int(vform, i));
  }
  return dst;
}


LogicVRegister Simulator::addp(VectorFormat vform,
                               LogicVRegister dst,
                               const LogicVRegister& src1,
                               const LogicVRegister& src2) {
  SimVRegister temp1, temp2;
  uzp1(vform, temp1, src1, src2);
  uzp2(vform, temp2, src1, src2);
  add(vform, dst, temp1, temp2);
  return dst;
}


LogicVRegister Simulator::mla(VectorFormat vform,
                              LogicVRegister dst,
                              const LogicVRegister& src1,
                              const LogicVRegister& src2) {
  SimVRegister temp;
  mul(vform, temp, src1, src2);
  add(vform, dst, dst, temp);
  return dst;
}


LogicVRegister Simulator::mls(VectorFormat vform,
                              LogicVRegister dst,
                              const LogicVRegister& src1,
                              const LogicVRegister& src2) {
  SimVRegister temp;
  mul(vform, temp, src1, src2);
  sub(vform, dst, dst, temp);
  return dst;
}


LogicVRegister Simulator::mul(VectorFormat vform,
                              LogicVRegister dst,
                              const LogicVRegister& src1,
                              const LogicVRegister& src2) {
  dst.ClearForWrite(vform);
  for (int i = 0; i < LaneCountFromFormat(vform); i++) {
    dst.SetUint(vform, i, src1.Uint(vform, i) * src2.Uint(vform, i));
  }
  return dst;
}


LogicVRegister Simulator::mul(VectorFormat vform,
                              LogicVRegister dst,
                              const LogicVRegister& src1,
                              const LogicVRegister& src2,
                              int index) {
  SimVRegister temp;
  VectorFormat indexform = VectorFormatFillQ(vform);
  return mul(vform, dst, src1, dup_element(indexform, temp, src2, index));
}


LogicVRegister Simulator::mla(VectorFormat vform,
                              LogicVRegister dst,
                              const LogicVRegister& src1,
                              const LogicVRegister& src2,
                              int index) {
  SimVRegister temp;
  VectorFormat indexform = VectorFormatFillQ(vform);
  return mla(vform, dst, src1, dup_element(indexform, temp, src2, index));
}


LogicVRegister Simulator::mls(VectorFormat vform,
                              LogicVRegister dst,
                              const LogicVRegister& src1,
                              const LogicVRegister& src2,
                              int index) {
  SimVRegister temp;
  VectorFormat indexform = VectorFormatFillQ(vform);
  return mls(vform, dst, src1, dup_element(indexform, temp, src2, index));
}


LogicVRegister Simulator::smull(VectorFormat vform,
                                LogicVRegister dst,
                                const LogicVRegister& src1,
                                const LogicVRegister& src2,
                                int index) {
  SimVRegister temp;
  VectorFormat indexform =
               VectorFormatHalfWidthDoubleLanes(VectorFormatFillQ(vform));
  return smull(vform, dst, src1, dup_element(indexform, temp, src2, index));
}


LogicVRegister Simulator::smull2(VectorFormat vform,
                                LogicVRegister dst,
                                const LogicVRegister& src1,
                                const LogicVRegister& src2,
                                int index) {
  SimVRegister temp;
  VectorFormat indexform =
               VectorFormatHalfWidthDoubleLanes(VectorFormatFillQ(vform));
  return smull2(vform, dst, src1, dup_element(indexform, temp, src2, index));
}


LogicVRegister Simulator::umull(VectorFormat vform,
                                LogicVRegister dst,
                                const LogicVRegister& src1,
                                const LogicVRegister& src2,
                                int index) {
  SimVRegister temp;
  VectorFormat indexform =
               VectorFormatHalfWidthDoubleLanes(VectorFormatFillQ(vform));
  return umull(vform, dst, src1, dup_element(indexform, temp, src2, index));
}


LogicVRegister Simulator::umull2(VectorFormat vform,
                                LogicVRegister dst,
                                const LogicVRegister& src1,
                                const LogicVRegister& src2,
                                int index) {
  SimVRegister temp;
  VectorFormat indexform =
               VectorFormatHalfWidthDoubleLanes(VectorFormatFillQ(vform));
  return umull2(vform, dst, src1, dup_element(indexform, temp, src2, index));
}


LogicVRegister Simulator::smlal(VectorFormat vform,
                                LogicVRegister dst,
                                const LogicVRegister& src1,
                                const LogicVRegister& src2,
                                int index) {
  SimVRegister temp;
  VectorFormat indexform =
               VectorFormatHalfWidthDoubleLanes(VectorFormatFillQ(vform));
  return smlal(vform, dst, src1, dup_element(indexform, temp, src2, index));
}


LogicVRegister Simulator::smlal2(VectorFormat vform,
                                LogicVRegister dst,
                                const LogicVRegister& src1,
                                const LogicVRegister& src2,
                                int index) {
  SimVRegister temp;
  VectorFormat indexform =
               VectorFormatHalfWidthDoubleLanes(VectorFormatFillQ(vform));
  return smlal2(vform, dst, src1, dup_element(indexform, temp, src2, index));
}


LogicVRegister Simulator::umlal(VectorFormat vform,
                                LogicVRegister dst,
                                const LogicVRegister& src1,
                                const LogicVRegister& src2,
                                int index) {
  SimVRegister temp;
  VectorFormat indexform =
               VectorFormatHalfWidthDoubleLanes(VectorFormatFillQ(vform));
  return umlal(vform, dst, src1, dup_element(indexform, temp, src2, index));
}


LogicVRegister Simulator::umlal2(VectorFormat vform,
                                LogicVRegister dst,
                                const LogicVRegister& src1,
                                const LogicVRegister& src2,
                                int index) {
  SimVRegister temp;
  VectorFormat indexform =
               VectorFormatHalfWidthDoubleLanes(VectorFormatFillQ(vform));
  return umlal2(vform, dst, src1, dup_element(indexform, temp, src2, index));
}


LogicVRegister Simulator::smlsl(VectorFormat vform,
                                LogicVRegister dst,
                                const LogicVRegister& src1,
                                const LogicVRegister& src2,
                                int index) {
  SimVRegister temp;
  VectorFormat indexform =
               VectorFormatHalfWidthDoubleLanes(VectorFormatFillQ(vform));
  return smlsl(vform, dst, src1, dup_element(indexform, temp, src2, index));
}


LogicVRegister Simulator::smlsl2(VectorFormat vform,
                                LogicVRegister dst,
                                const LogicVRegister& src1,
                                const LogicVRegister& src2,
                                int index) {
  SimVRegister temp;
  VectorFormat indexform =
               VectorFormatHalfWidthDoubleLanes(VectorFormatFillQ(vform));
  return smlsl2(vform, dst, src1, dup_element(indexform, temp, src2, index));
}


LogicVRegister Simulator::umlsl(VectorFormat vform,
                                LogicVRegister dst,
                                const LogicVRegister& src1,
                                const LogicVRegister& src2,
                                int index) {
  SimVRegister temp;
  VectorFormat indexform =
               VectorFormatHalfWidthDoubleLanes(VectorFormatFillQ(vform));
  return umlsl(vform, dst, src1, dup_element(indexform, temp, src2, index));
}


LogicVRegister Simulator::umlsl2(VectorFormat vform,
                                LogicVRegister dst,
                                const LogicVRegister& src1,
                                const LogicVRegister& src2,
                                int index) {
  SimVRegister temp;
  VectorFormat indexform =
               VectorFormatHalfWidthDoubleLanes(VectorFormatFillQ(vform));
  return umlsl2(vform, dst, src1, dup_element(indexform, temp, src2, index));
}


LogicVRegister Simulator::sqdmull(VectorFormat vform,
                                  LogicVRegister dst,
                                  const LogicVRegister& src1,
                                  const LogicVRegister& src2,
                                  int index) {
  SimVRegister temp;
  VectorFormat indexform =
      VectorFormatHalfWidthDoubleLanes(VectorFormatFillQ(vform));
  return sqdmull(vform, dst, src1, dup_element(indexform, temp, src2, index));
}


LogicVRegister Simulator::sqdmull2(VectorFormat vform,
                                  LogicVRegister dst,
                                  const LogicVRegister& src1,
                                  const LogicVRegister& src2,
                                  int index) {
  SimVRegister temp;
  VectorFormat indexform =
      VectorFormatHalfWidthDoubleLanes(VectorFormatFillQ(vform));
  return sqdmull2(vform, dst, src1, dup_element(indexform, temp, src2, index));
}


LogicVRegister Simulator::sqdmlal(VectorFormat vform,
                                  LogicVRegister dst,
                                  const LogicVRegister& src1,
                                  const LogicVRegister& src2,
                                  int index) {
  SimVRegister temp;
  VectorFormat indexform =
      VectorFormatHalfWidthDoubleLanes(VectorFormatFillQ(vform));
  return sqdmlal(vform, dst, src1, dup_element(indexform, temp, src2, index));
}


LogicVRegister Simulator::sqdmlal2(VectorFormat vform,
                                  LogicVRegister dst,
                                  const LogicVRegister& src1,
                                  const LogicVRegister& src2,
                                  int index) {
  SimVRegister temp;
  VectorFormat indexform =
      VectorFormatHalfWidthDoubleLanes(VectorFormatFillQ(vform));
  return sqdmlal2(vform, dst, src1, dup_element(indexform, temp, src2, index));
}


LogicVRegister Simulator::sqdmlsl(VectorFormat vform,
                                  LogicVRegister dst,
                                  const LogicVRegister& src1,
                                  const LogicVRegister& src2,
                                  int index) {
  SimVRegister temp;
  VectorFormat indexform =
      VectorFormatHalfWidthDoubleLanes(VectorFormatFillQ(vform));
  return sqdmlsl(vform, dst, src1, dup_element(indexform, temp, src2, index));
}


LogicVRegister Simulator::sqdmlsl2(VectorFormat vform,
                                  LogicVRegister dst,
                                  const LogicVRegister& src1,
                                  const LogicVRegister& src2,
                                  int index) {
  SimVRegister temp;
  VectorFormat indexform =
      VectorFormatHalfWidthDoubleLanes(VectorFormatFillQ(vform));
  return sqdmlsl2(vform, dst, src1, dup_element(indexform, temp, src2, index));
}


LogicVRegister Simulator::sqdmulh(VectorFormat vform,
                                  LogicVRegister dst,
                                  const LogicVRegister& src1,
                                  const LogicVRegister& src2,
                                  int index) {
  SimVRegister temp;
  VectorFormat indexform = VectorFormatFillQ(vform);
  return sqdmulh(vform, dst, src1, dup_element(indexform, temp, src2, index));
}


LogicVRegister Simulator::sqrdmulh(VectorFormat vform,
                                  LogicVRegister dst,
                                  const LogicVRegister& src1,
                                  const LogicVRegister& src2,
                                  int index) {
  SimVRegister temp;
  VectorFormat indexform = VectorFormatFillQ(vform);
  return sqrdmulh(vform, dst, src1, dup_element(indexform, temp, src2, index));
}


uint16_t Simulator::PolynomialMult(uint8_t op1, uint8_t op2) {
  uint16_t result = 0;
  uint16_t extended_op2 = op2;
  for (int i = 0; i < 8; ++i) {
    if ((op1 >> i) & 1) {
      result = result ^ (extended_op2 << i);
    }
  }
  return result;
}


LogicVRegister Simulator::pmul(VectorFormat vform,
                               LogicVRegister dst,
                               const LogicVRegister& src1,
                               const LogicVRegister& src2) {
  dst.ClearForWrite(vform);
  for (int i = 0; i < LaneCountFromFormat(vform); i++) {
    dst.SetUint(vform, i,
                PolynomialMult(src1.Uint(vform, i), src2.Uint(vform, i)));
  }
  return dst;
}


LogicVRegister Simulator::pmull(VectorFormat vform,
                               LogicVRegister dst,
                               const LogicVRegister& src1,
                               const LogicVRegister& src2) {
  VectorFormat vform_src = VectorFormatHalfWidth(vform);
  dst.ClearForWrite(vform);
  for (int i = 0; i < LaneCountFromFormat(vform); i++) {
    dst.SetUint(vform, i, PolynomialMult(src1.Uint(vform_src, i),
                                         src2.Uint(vform_src, i)));
  }
  return dst;
}


LogicVRegister Simulator::pmull2(VectorFormat vform,
                                LogicVRegister dst,
                                const LogicVRegister& src1,
                                const LogicVRegister& src2) {
  VectorFormat vform_src = VectorFormatHalfWidthDoubleLanes(vform);
  dst.ClearForWrite(vform);
  int lane_count = LaneCountFromFormat(vform);
  for (int i = 0; i < lane_count; i++) {
    dst.SetUint(vform, i, PolynomialMult(src1.Uint(vform_src, lane_count + i),
                                         src2.Uint(vform_src, lane_count + i)));
  }
  return dst;
}


LogicVRegister Simulator::sub(VectorFormat vform,
                              LogicVRegister dst,
                              const LogicVRegister& src1,
                              const LogicVRegister& src2) {
  dst.ClearForWrite(vform);
  for (int i = 0; i < LaneCountFromFormat(vform); i++) {
    // Test for unsigned saturation.
    if (src2.Uint(vform, i) > src1.Uint(vform, i)) {
      dst.SetUnsignedSat(i, false);
    }

    // Test for signed saturation.
    int64_t sa = src1.IntLeftJustified(vform, i);
    int64_t sb = src2.IntLeftJustified(vform, i);
    int64_t sr = sa - sb;
    // If the signs of the operands are different, and the sign of the first
    // operand doesn't match the result, there was an overflow.
    if (((sa >= 0) != (sb >= 0)) && ((sa >= 0) != (sr >= 0))) {
      dst.SetSignedSat(i, sr < 0);
    }

    dst.SetInt(vform, i, src1.Int(vform, i) - src2.Int(vform, i));
  }
  return dst;
}


LogicVRegister Simulator::and_(VectorFormat vform,
                               LogicVRegister dst,
                               const LogicVRegister& src1,
                               const LogicVRegister& src2) {
  dst.ClearForWrite(vform);
  for (int i = 0; i < LaneCountFromFormat(vform); i++) {
    dst.SetUint(vform, i, src1.Uint(vform, i) & src2.Uint(vform, i));
  }
  return dst;
}


LogicVRegister Simulator::orr(VectorFormat vform,
                              LogicVRegister dst,
                              const LogicVRegister& src1,
                              const LogicVRegister& src2) {
  dst.ClearForWrite(vform);
  for (int i = 0; i < LaneCountFromFormat(vform); i++) {
    dst.SetUint(vform, i, src1.Uint(vform, i) | src2.Uint(vform, i));
  }
  return dst;
}


LogicVRegister Simulator::orn(VectorFormat vform,
                              LogicVRegister dst,
                              const LogicVRegister& src1,
                              const LogicVRegister& src2) {
  dst.ClearForWrite(vform);
  for (int i = 0; i < LaneCountFromFormat(vform); i++) {
    dst.SetUint(vform, i, src1.Uint(vform, i) | ~src2.Uint(vform, i));
  }
  return dst;
}


LogicVRegister Simulator::eor(VectorFormat vform,
                              LogicVRegister dst,
                              const LogicVRegister& src1,
                              const LogicVRegister& src2) {
  dst.ClearForWrite(vform);
  for (int i = 0; i < LaneCountFromFormat(vform); i++) {
    dst.SetUint(vform, i, src1.Uint(vform, i) ^ src2.Uint(vform, i));
  }
  return dst;
}


LogicVRegister Simulator::bic(VectorFormat vform,
                              LogicVRegister dst,
                              const LogicVRegister& src1,
                              const LogicVRegister& src2) {
  dst.ClearForWrite(vform);
  for (int i = 0; i < LaneCountFromFormat(vform); i++) {
    dst.SetUint(vform, i, src1.Uint(vform, i) & ~src2.Uint(vform, i));
  }
  return dst;
}


LogicVRegister Simulator::bic(VectorFormat vform,
                              LogicVRegister dst,
                              const LogicVRegister& src,
                              uint64_t imm) {
  uint64_t result[16];
  int laneCount = LaneCountFromFormat(vform);
  for (int i = 0; i < laneCount; ++i) {
    result[i] = src.Uint(vform, i) & ~imm;
  }
  dst.ClearForWrite(vform);
  for (int i = 0; i < laneCount; ++i) {
    dst.SetUint(vform, i, result[i]);
  }
  return dst;
}


LogicVRegister Simulator::bif(VectorFormat vform,
                              LogicVRegister dst,
                              const LogicVRegister& src1,
                              const LogicVRegister& src2) {
  dst.ClearForWrite(vform);
  for (int i = 0; i < LaneCountFromFormat(vform); i++) {
    uint64_t operand1 = dst.Uint(vform, i);
    uint64_t operand2 = ~src2.Uint(vform, i);
    uint64_t operand3 = src1.Uint(vform, i);
    uint64_t result = operand1 ^ ((operand1 ^ operand3) & operand2);
    dst.SetUint(vform, i, result);
  }
  return dst;
}


LogicVRegister Simulator::bit(VectorFormat vform,
                              LogicVRegister dst,
                              const LogicVRegister& src1,
                              const LogicVRegister& src2) {
  dst.ClearForWrite(vform);
  for (int i = 0; i < LaneCountFromFormat(vform); i++) {
    uint64_t operand1 = dst.Uint(vform, i);
    uint64_t operand2 = src2.Uint(vform, i);
    uint64_t operand3 = src1.Uint(vform, i);
    uint64_t result = operand1 ^ ((operand1 ^ operand3) & operand2);
    dst.SetUint(vform, i, result);
  }
  return dst;
}


LogicVRegister Simulator::bsl(VectorFormat vform,
                              LogicVRegister dst,
                              const LogicVRegister& src1,
                              const LogicVRegister& src2) {
  dst.ClearForWrite(vform);
  for (int i = 0; i < LaneCountFromFormat(vform); i++) {
    uint64_t operand1 = src2.Uint(vform, i);
    uint64_t operand2 = dst.Uint(vform, i);
    uint64_t operand3 = src1.Uint(vform, i);
    uint64_t result = operand1 ^ ((operand1 ^ operand3) & operand2);
    dst.SetUint(vform, i, result);
  }
  return dst;
}


LogicVRegister Simulator::sminmax(VectorFormat vform,
                                  LogicVRegister dst,
                                  const LogicVRegister& src1,
                                  const LogicVRegister& src2,
                                  bool max) {
  dst.ClearForWrite(vform);
  for (int i = 0; i < LaneCountFromFormat(vform); i++) {
    int64_t src1_val = src1.Int(vform, i);
    int64_t src2_val = src2.Int(vform, i);
    int64_t dst_val;
    if (max == true) {
      dst_val = (src1_val > src2_val) ? src1_val : src2_val;
    } else {
      dst_val = (src1_val < src2_val) ? src1_val : src2_val;
    }
    dst.SetInt(vform, i, dst_val);
  }
  return dst;
}


LogicVRegister Simulator::smax(VectorFormat vform,
                               LogicVRegister dst,
                               const LogicVRegister& src1,
                               const LogicVRegister& src2) {
  return sminmax(vform, dst, src1, src2, true);
}


LogicVRegister Simulator::smin(VectorFormat vform,
                               LogicVRegister dst,
                               const LogicVRegister& src1,
                               const LogicVRegister& src2) {
  return sminmax(vform, dst, src1, src2, false);
}


LogicVRegister Simulator::sminmaxp(VectorFormat vform,
                                   LogicVRegister dst,
                                   int dst_index,
                                   const LogicVRegister& src,
                                   bool max) {
  for (int i = 0; i < LaneCountFromFormat(vform); i += 2) {
    int64_t src1_val = src.Int(vform, i);
    int64_t src2_val = src.Int(vform, i + 1);
    int64_t dst_val;
    if (max == true) {
      dst_val = (src1_val > src2_val) ? src1_val : src2_val;
    } else {
      dst_val = (src1_val < src2_val) ? src1_val : src2_val;
    }
    dst.SetInt(vform, dst_index + (i >> 1), dst_val);
  }
  return dst;
}


LogicVRegister Simulator::smaxp(VectorFormat vform,
                                LogicVRegister dst,
                                const LogicVRegister& src1,
                                const LogicVRegister& src2) {
  dst.ClearForWrite(vform);
  sminmaxp(vform, dst, 0, src1, true);
  sminmaxp(vform, dst, LaneCountFromFormat(vform) >> 1, src2, true);
  return dst;
}


LogicVRegister Simulator::sminp(VectorFormat vform,
                                LogicVRegister dst,
                                const LogicVRegister& src1,
                                const LogicVRegister& src2) {
  dst.ClearForWrite(vform);
  sminmaxp(vform, dst, 0, src1, false);
  sminmaxp(vform, dst, LaneCountFromFormat(vform) >> 1, src2, false);
  return dst;
}


LogicVRegister Simulator::addp(VectorFormat vform,
                               LogicVRegister dst,
                               const LogicVRegister& src) {
  VIXL_ASSERT(vform == kFormatD);

  int64_t dst_val = src.Int(kFormat2D, 0) + src.Int(kFormat2D, 1);
  dst.ClearForWrite(vform);
  dst.SetInt(vform, 0, dst_val);
  return dst;
}


LogicVRegister Simulator::addv(VectorFormat vform,
                               LogicVRegister dst,
                               const LogicVRegister& src) {
  VectorFormat vform_dst
    = ScalarFormatFromLaneSize(LaneSizeInBitsFromFormat(vform));


  int64_t dst_val = 0;
  for (int i = 0; i < LaneCountFromFormat(vform); i++) {
    dst_val += src.Int(vform, i);
  }

  dst.ClearForWrite(vform_dst);
  dst.SetInt(vform_dst, 0, dst_val);
  return dst;
}


LogicVRegister Simulator::saddlv(VectorFormat vform,
                                 LogicVRegister dst,
                                 const LogicVRegister& src) {
  VectorFormat vform_dst
    = ScalarFormatFromLaneSize(LaneSizeInBitsFromFormat(vform) * 2);

  int64_t dst_val = 0;
  for (int i = 0; i < LaneCountFromFormat(vform); i++) {
    dst_val += src.Int(vform, i);
  }

  dst.ClearForWrite(vform_dst);
  dst.SetInt(vform_dst, 0, dst_val);
  return dst;
}


LogicVRegister Simulator::uaddlv(VectorFormat vform,
                                 LogicVRegister dst,
                                 const LogicVRegister& src) {
  VectorFormat vform_dst
    = ScalarFormatFromLaneSize(LaneSizeInBitsFromFormat(vform) * 2);

  uint64_t dst_val = 0;
  for (int i = 0; i < LaneCountFromFormat(vform); i++) {
    dst_val += src.Uint(vform, i);
  }

  dst.ClearForWrite(vform_dst);
  dst.SetUint(vform_dst, 0, dst_val);
  return dst;
}


LogicVRegister Simulator::sminmaxv(VectorFormat vform,
                                   LogicVRegister dst,
                                   const LogicVRegister& src,
                                   bool max) {
  dst.ClearForWrite(vform);
  int64_t dst_val = max ? INT64_MIN : INT64_MAX;
  for (int i = 0; i < LaneCountFromFormat(vform); i++) {
    dst.SetInt(vform, i, 0);
    int64_t src_val = src.Int(vform, i);
    if (max == true) {
      dst_val = (src_val > dst_val) ? src_val : dst_val;
    } else {
      dst_val = (src_val < dst_val) ? src_val : dst_val;
    }
  }
  dst.SetInt(vform, 0, dst_val);
  return dst;
}


LogicVRegister Simulator::smaxv(VectorFormat vform,
                                LogicVRegister dst,
                                const LogicVRegister& src) {
  sminmaxv(vform, dst, src, true);
  return dst;
}


LogicVRegister Simulator::sminv(VectorFormat vform,
                                LogicVRegister dst,
                                const LogicVRegister& src) {
  sminmaxv(vform, dst, src, false);
  return dst;
}


LogicVRegister Simulator::uminmax(VectorFormat vform,
                                  LogicVRegister dst,
                                  const LogicVRegister& src1,
                                  const LogicVRegister& src2,
                                  bool max) {
  dst.ClearForWrite(vform);
  for (int i = 0; i < LaneCountFromFormat(vform); i++) {
    uint64_t src1_val = src1.Uint(vform, i);
    uint64_t src2_val = src2.Uint(vform, i);
    uint64_t dst_val;
    if (max == true) {
      dst_val = (src1_val > src2_val) ? src1_val : src2_val;
    } else {
      dst_val = (src1_val < src2_val) ? src1_val : src2_val;
    }
    dst.SetUint(vform, i, dst_val);
  }
  return dst;
}


LogicVRegister Simulator::umax(VectorFormat vform,
                               LogicVRegister dst,
                               const LogicVRegister& src1,
                               const LogicVRegister& src2) {
  return uminmax(vform, dst, src1, src2, true);
}


LogicVRegister Simulator::umin(VectorFormat vform,
                               LogicVRegister dst,
                               const LogicVRegister& src1,
                               const LogicVRegister& src2) {
  return uminmax(vform, dst, src1, src2, false);
}


LogicVRegister Simulator::uminmaxp(VectorFormat vform,
                                   LogicVRegister dst,
                                   int dst_index,
                                   const LogicVRegister& src,
                                   bool max) {
  for (int i = 0; i < LaneCountFromFormat(vform); i += 2) {
    uint64_t src1_val = src.Uint(vform, i);
    uint64_t src2_val = src.Uint(vform, i + 1);
    uint64_t dst_val;
    if (max == true) {
      dst_val = (src1_val > src2_val) ? src1_val : src2_val;
    } else {
      dst_val = (src1_val < src2_val) ? src1_val : src2_val;
    }
    dst.SetUint(vform, dst_index + (i >> 1), dst_val);
  }
  return dst;
}


LogicVRegister Simulator::umaxp(VectorFormat vform,
                                LogicVRegister dst,
                                const LogicVRegister& src1,
                                const LogicVRegister& src2) {
  dst.ClearForWrite(vform);
  uminmaxp(vform, dst, 0, src1, true);
  uminmaxp(vform, dst, LaneCountFromFormat(vform) >> 1, src2, true);
  return dst;
}


LogicVRegister Simulator::uminp(VectorFormat vform,
                                LogicVRegister dst,
                                const LogicVRegister& src1,
                                const LogicVRegister& src2) {
  dst.ClearForWrite(vform);
  uminmaxp(vform, dst, 0, src1, false);
  uminmaxp(vform, dst, LaneCountFromFormat(vform) >> 1, src2, false);
  return dst;
}


LogicVRegister Simulator::uminmaxv(VectorFormat vform,
                                   LogicVRegister dst,
                                   const LogicVRegister& src,
                                   bool max) {
  dst.ClearForWrite(vform);
  uint64_t dst_val = max ? 0 : UINT64_MAX;
  for (int i = 0; i < LaneCountFromFormat(vform); i++) {
    dst.SetUint(vform, i, 0);
    uint64_t src_val = src.Uint(vform, i);
    if (max == true) {
      dst_val = (src_val > dst_val) ? src_val : dst_val;
    } else {
      dst_val = (src_val < dst_val) ? src_val : dst_val;
    }
  }
  dst.SetUint(vform, 0, dst_val);
  return dst;
}


LogicVRegister Simulator::umaxv(VectorFormat vform,
                                LogicVRegister dst,
                                const LogicVRegister& src) {
  uminmaxv(vform, dst, src, true);
  return dst;
}


LogicVRegister Simulator::uminv(VectorFormat vform,
                                LogicVRegister dst,
                                const LogicVRegister& src) {
  uminmaxv(vform, dst, src, false);
  return dst;
}


LogicVRegister Simulator::shl(VectorFormat vform,
                              LogicVRegister dst,
                              const LogicVRegister& src,
                              int shift) {
  VIXL_ASSERT(shift >= 0);
  SimVRegister temp;
  LogicVRegister shiftreg = dup_immediate(vform, temp, shift);
  return ushl(vform, dst, src, shiftreg);
}


LogicVRegister Simulator::sshll(VectorFormat vform,
                                LogicVRegister dst,
                                const LogicVRegister& src,
                                int shift) {
  VIXL_ASSERT(shift >= 0);
  SimVRegister temp1, temp2;
  LogicVRegister shiftreg = dup_immediate(vform, temp1, shift);
  LogicVRegister extendedreg = sxtl(vform, temp2, src);
  return sshl(vform, dst, extendedreg, shiftreg);
}


LogicVRegister Simulator::sshll2(VectorFormat vform,
                                 LogicVRegister dst,
                                 const LogicVRegister& src,
                                 int shift) {
  VIXL_ASSERT(shift >= 0);
  SimVRegister temp1, temp2;
  LogicVRegister shiftreg = dup_immediate(vform, temp1, shift);
  LogicVRegister extendedreg = sxtl2(vform, temp2, src);
  return sshl(vform, dst, extendedreg, shiftreg);
}


LogicVRegister Simulator::shll(VectorFormat vform,
                               LogicVRegister dst,
                               const LogicVRegister& src) {
  int shift = LaneSizeInBitsFromFormat(vform) / 2;
  return sshll(vform, dst, src, shift);
}


LogicVRegister Simulator::shll2(VectorFormat vform,
                                LogicVRegister dst,
                                const LogicVRegister& src) {
  int shift = LaneSizeInBitsFromFormat(vform) / 2;
  return sshll2(vform, dst, src, shift);
}


LogicVRegister Simulator::ushll(VectorFormat vform,
                                LogicVRegister dst,
                                const LogicVRegister& src,
                                int shift) {
  VIXL_ASSERT(shift >= 0);
  SimVRegister temp1, temp2;
  LogicVRegister shiftreg = dup_immediate(vform, temp1, shift);
  LogicVRegister extendedreg = uxtl(vform, temp2, src);
  return ushl(vform, dst, extendedreg, shiftreg);
}


LogicVRegister Simulator::ushll2(VectorFormat vform,
                                 LogicVRegister dst,
                                 const LogicVRegister& src,
                                 int shift) {
  VIXL_ASSERT(shift >= 0);
  SimVRegister temp1, temp2;
  LogicVRegister shiftreg = dup_immediate(vform, temp1, shift);
  LogicVRegister extendedreg = uxtl2(vform, temp2, src);
  return ushl(vform, dst, extendedreg, shiftreg);
}


LogicVRegister Simulator::sli(VectorFormat vform,
                              LogicVRegister dst,
                              const LogicVRegister& src,
                              int shift) {
  dst.ClearForWrite(vform);
  int laneCount = LaneCountFromFormat(vform);
  for (int i = 0; i < laneCount; i++) {
    uint64_t src_lane = src.Uint(vform, i);
    uint64_t dst_lane = dst.Uint(vform, i);
    uint64_t shifted = src_lane << shift;
    uint64_t mask = MaxUintFromFormat(vform) << shift;
    dst.SetUint(vform, i, (dst_lane & ~mask) | shifted);
  }
  return dst;
}


LogicVRegister Simulator::sqshl(VectorFormat vform,
                                LogicVRegister dst,
                                const LogicVRegister& src,
                                int shift) {
  VIXL_ASSERT(shift >= 0);
  SimVRegister temp;
  LogicVRegister shiftreg = dup_immediate(vform, temp, shift);
  return sshl(vform, dst, src, shiftreg).SignedSaturate(vform);
}


LogicVRegister Simulator::uqshl(VectorFormat vform,
                                LogicVRegister dst,
                                const LogicVRegister& src,
                                int shift) {
  VIXL_ASSERT(shift >= 0);
  SimVRegister temp;
  LogicVRegister shiftreg = dup_immediate(vform, temp, shift);
  return ushl(vform, dst, src, shiftreg).UnsignedSaturate(vform);
}


LogicVRegister Simulator::sqshlu(VectorFormat vform,
                                 LogicVRegister dst,
                                 const LogicVRegister& src,
                                 int shift) {
  VIXL_ASSERT(shift >= 0);
  SimVRegister temp;
  LogicVRegister shiftreg = dup_immediate(vform, temp, shift);
  return sshl(vform, dst, src, shiftreg).UnsignedSaturate(vform);
}


LogicVRegister Simulator::sri(VectorFormat vform,
                              LogicVRegister dst,
                              const LogicVRegister& src,
                              int shift) {
  dst.ClearForWrite(vform);
  int laneCount = LaneCountFromFormat(vform);
  VIXL_ASSERT((shift > 0) &&
              (shift <= static_cast<int>(LaneSizeInBitsFromFormat(vform))));
  for (int i = 0; i < laneCount; i++) {
    uint64_t src_lane = src.Uint(vform, i);
    uint64_t dst_lane = dst.Uint(vform, i);
    uint64_t shifted;
    uint64_t mask;
    if (shift == 64) {
      shifted = 0;
      mask = 0;
    } else {
      shifted = src_lane >> shift;
      mask = MaxUintFromFormat(vform) >> shift;
    }
    dst.SetUint(vform, i, (dst_lane & ~mask) | shifted);
  }
  return dst;
}


LogicVRegister Simulator::ushr(VectorFormat vform,
                               LogicVRegister dst,
                               const LogicVRegister& src,
                               int shift) {
  VIXL_ASSERT(shift >= 0);
  SimVRegister temp;
  LogicVRegister shiftreg = dup_immediate(vform, temp, -shift);
  return ushl(vform, dst, src, shiftreg);
}


LogicVRegister Simulator::sshr(VectorFormat vform,
                               LogicVRegister dst,
                               const LogicVRegister& src,
                               int shift) {
  VIXL_ASSERT(shift >= 0);
  SimVRegister temp;
  LogicVRegister shiftreg = dup_immediate(vform, temp, -shift);
  return sshl(vform, dst, src, shiftreg);
}


LogicVRegister Simulator::ssra(VectorFormat vform,
                               LogicVRegister dst,
                               const LogicVRegister& src,
                               int shift) {
  SimVRegister temp;
  LogicVRegister shifted_reg = sshr(vform, temp, src, shift);
  return add(vform, dst, dst, shifted_reg);
}


LogicVRegister Simulator::usra(VectorFormat vform,
                               LogicVRegister dst,
                               const LogicVRegister& src,
                               int shift) {
  SimVRegister temp;
  LogicVRegister shifted_reg = ushr(vform, temp, src, shift);
  return add(vform, dst, dst, shifted_reg);
}


LogicVRegister Simulator::srsra(VectorFormat vform,
                                LogicVRegister dst,
                                const LogicVRegister& src,
                                int shift) {
  SimVRegister temp;
  LogicVRegister shifted_reg = sshr(vform, temp, src, shift).Round(vform);
  return add(vform, dst, dst, shifted_reg);
}


LogicVRegister Simulator::ursra(VectorFormat vform,
                                LogicVRegister dst,
                                const LogicVRegister& src,
                                int shift) {
  SimVRegister temp;
  LogicVRegister shifted_reg = ushr(vform, temp, src, shift).Round(vform);
  return add(vform, dst, dst, shifted_reg);
}


LogicVRegister Simulator::cls(VectorFormat vform,
                              LogicVRegister dst,
                              const LogicVRegister& src) {
  uint64_t result[16];
  int laneSizeInBits  = LaneSizeInBitsFromFormat(vform);
  int laneCount = LaneCountFromFormat(vform);
  for (int i = 0; i < laneCount; i++) {
    result[i] = CountLeadingSignBits(src.Int(vform, i), laneSizeInBits);
  }

  dst.ClearForWrite(vform);
  for (int i = 0; i < laneCount; ++i) {
    dst.SetUint(vform, i, result[i]);
  }
  return dst;
}


LogicVRegister Simulator::clz(VectorFormat vform,
                              LogicVRegister dst,
                              const LogicVRegister& src) {
  uint64_t result[16];
  int laneSizeInBits  = LaneSizeInBitsFromFormat(vform);
  int laneCount = LaneCountFromFormat(vform);
  for (int i = 0; i < laneCount; i++) {
    result[i] = CountLeadingZeros(src.Uint(vform, i), laneSizeInBits);
  }

  dst.ClearForWrite(vform);
  for (int i = 0; i < laneCount; ++i) {
    dst.SetUint(vform, i, result[i]);
  }
  return dst;
}


LogicVRegister Simulator::cnt(VectorFormat vform,
                              LogicVRegister dst,
                              const LogicVRegister& src) {
  uint64_t result[16];
  int laneSizeInBits  = LaneSizeInBitsFromFormat(vform);
  int laneCount = LaneCountFromFormat(vform);
  for (int i = 0; i < laneCount; i++) {
    uint64_t value = src.Uint(vform, i);
    result[i] = 0;
    for (int j = 0; j < laneSizeInBits; j++) {
      result[i] += (value & 1);
      value >>= 1;
    }
  }

  dst.ClearForWrite(vform);
  for (int i = 0; i < laneCount; ++i) {
    dst.SetUint(vform, i, result[i]);
  }
  return dst;
}


LogicVRegister Simulator::sshl(VectorFormat vform,
                               LogicVRegister dst,
                               const LogicVRegister& src1,
                               const LogicVRegister& src2) {
  dst.ClearForWrite(vform);
  for (int i = 0; i < LaneCountFromFormat(vform); i++) {
    int8_t shift_val = src2.Int(vform, i);
    int64_t lj_src_val = src1.IntLeftJustified(vform, i);

    // Set signed saturation state.
    if ((shift_val > CountLeadingSignBits(lj_src_val)) &&
        (lj_src_val != 0)) {
      dst.SetSignedSat(i, lj_src_val >= 0);
    }

    // Set unsigned saturation state.
    if (lj_src_val < 0) {
      dst.SetUnsignedSat(i, false);
    } else if ((shift_val > CountLeadingZeros(lj_src_val)) &&
               (lj_src_val != 0)) {
      dst.SetUnsignedSat(i, true);
    }

    int64_t src_val = src1.Int(vform, i);
    if (shift_val > 63) {
      dst.SetInt(vform, i, 0);
    } else if (shift_val < -63) {
      dst.SetRounding(i, src_val < 0);
      dst.SetInt(vform, i, (src_val < 0) ? -1 : 0);
    } else {
      if (shift_val < 0) {
        // Set rounding state. Rounding only needed on right shifts.
        if (((src_val >> (-shift_val - 1)) & 1) == 1) {
          dst.SetRounding(i, true);
        }
        src_val >>= -shift_val;
      } else {
        src_val <<= shift_val;
      }
      dst.SetInt(vform, i, src_val);
    }
  }
  return dst;
}


LogicVRegister Simulator::ushl(VectorFormat vform,
                               LogicVRegister dst,
                               const LogicVRegister& src1,
                               const LogicVRegister& src2) {
  dst.ClearForWrite(vform);
  for (int i = 0; i < LaneCountFromFormat(vform); i++) {
    int8_t shift_val = src2.Int(vform, i);
    uint64_t lj_src_val = src1.UintLeftJustified(vform, i);

    // Set saturation state.
    if ((shift_val > CountLeadingZeros(lj_src_val)) && (lj_src_val != 0)) {
      dst.SetUnsignedSat(i, true);
    }

    uint64_t src_val = src1.Uint(vform, i);
    if ((shift_val > 63) || (shift_val < -64)) {
      dst.SetUint(vform, i, 0);
    } else {
      if (shift_val < 0) {
        // Set rounding state. Rounding only needed on right shifts.
        if (((src_val >> (-shift_val - 1)) & 1) == 1) {
          dst.SetRounding(i, true);
        }

        if (shift_val == -64) {
          src_val = 0;
        } else {
          src_val >>= -shift_val;
        }
      } else {
        src_val <<= shift_val;
      }
      dst.SetUint(vform, i, src_val);
    }
  }
  return dst;
}


LogicVRegister Simulator::neg(VectorFormat vform,
                              LogicVRegister dst,
                              const LogicVRegister& src) {
  dst.ClearForWrite(vform);
  for (int i = 0; i < LaneCountFromFormat(vform); i++) {
    // Test for signed saturation.
    int64_t sa = src.Int(vform, i);
    if (sa == MinIntFromFormat(vform)) {
      dst.SetSignedSat(i, true);
    }
    dst.SetInt(vform, i, -sa);
  }
  return dst;
}


LogicVRegister Simulator::suqadd(VectorFormat vform,
                                 LogicVRegister dst,
                                 const LogicVRegister& src) {
  dst.ClearForWrite(vform);
  for (int i = 0; i < LaneCountFromFormat(vform); i++) {
    int64_t  sa = dst.IntLeftJustified(vform, i);
    uint64_t ub = src.UintLeftJustified(vform, i);
    int64_t  sr = sa + ub;

    if (sr < sa) {  // Test for signed positive saturation.
      dst.SetInt(vform, i, MaxIntFromFormat(vform));
    } else {
      dst.SetInt(vform, i, dst.Int(vform, i) + src.Int(vform, i));
    }
  }
  return dst;
}


LogicVRegister Simulator::usqadd(VectorFormat vform,
                                 LogicVRegister dst,
                                 const LogicVRegister& src) {
  dst.ClearForWrite(vform);
  for (int i = 0; i < LaneCountFromFormat(vform); i++) {
    uint64_t  ua = dst.UintLeftJustified(vform, i);
    int64_t   sb = src.IntLeftJustified(vform, i);
    uint64_t  ur = ua + sb;

    if ((sb > 0) && (ur <= ua)) {
      dst.SetUint(vform, i, MaxUintFromFormat(vform));  // Positive saturation.
    } else if ((sb < 0) && (ur >= ua)) {
      dst.SetUint(vform, i, 0);                         // Negative saturation.
    } else {
      dst.SetUint(vform, i, dst.Uint(vform, i) + src.Int(vform, i));
    }
  }
  return dst;
}


LogicVRegister Simulator::abs(VectorFormat vform,
                              LogicVRegister dst,
                              const LogicVRegister& src) {
  dst.ClearForWrite(vform);
  for (int i = 0; i < LaneCountFromFormat(vform); i++) {
    // Test for signed saturation.
    int64_t sa = src.Int(vform, i);
    if (sa == MinIntFromFormat(vform)) {
      dst.SetSignedSat(i, true);
    }
    if (sa < 0) {
      dst.SetInt(vform, i, -sa);
    } else {
      dst.SetInt(vform, i, sa);
    }
  }
  return dst;
}


LogicVRegister Simulator::extractnarrow(VectorFormat dstform,
                                        LogicVRegister dst,
                                        bool dstIsSigned,
                                        const LogicVRegister& src,
                                        bool srcIsSigned) {
  bool upperhalf = false;
  VectorFormat srcform = kFormatUndefined;
  int64_t  ssrc[8];
  uint64_t usrc[8];

  switch (dstform) {
    case kFormat8B : upperhalf = false; srcform = kFormat8H; break;
    case kFormat16B: upperhalf = true;  srcform = kFormat8H; break;
    case kFormat4H : upperhalf = false; srcform = kFormat4S; break;
    case kFormat8H : upperhalf = true;  srcform = kFormat4S; break;
    case kFormat2S : upperhalf = false; srcform = kFormat2D; break;
    case kFormat4S : upperhalf = true;  srcform = kFormat2D; break;
    case kFormatB  : upperhalf = false; srcform = kFormatH;  break;
    case kFormatH  : upperhalf = false; srcform = kFormatS;  break;
    case kFormatS  : upperhalf = false; srcform = kFormatD;  break;
    default:VIXL_UNIMPLEMENTED();
  }

  for (int i = 0; i < LaneCountFromFormat(srcform); i++) {
    ssrc[i] = src.Int(srcform, i);
    usrc[i] = src.Uint(srcform, i);
  }

  int offset;
  if (upperhalf) {
    offset = LaneCountFromFormat(dstform) / 2;
  } else {
    offset = 0;
    dst.ClearForWrite(dstform);
  }

  for (int i = 0; i < LaneCountFromFormat(srcform); i++) {
    // Test for signed saturation
    if (ssrc[i] > MaxIntFromFormat(dstform)) {
      dst.SetSignedSat(offset + i, true);
    } else if (ssrc[i] < MinIntFromFormat(dstform)) {
      dst.SetSignedSat(offset + i, false);
    }

    // Test for unsigned saturation
    if (srcIsSigned) {
      if (ssrc[i] > static_cast<int64_t>(MaxUintFromFormat(dstform))) {
        dst.SetUnsignedSat(offset + i, true);
      } else if (ssrc[i] < 0) {
        dst.SetUnsignedSat(offset + i, false);
      }
    } else {
      if (usrc[i] > MaxUintFromFormat(dstform)) {
        dst.SetUnsignedSat(offset + i, true);
      }
    }

    int64_t result;
    if (srcIsSigned) {
      result = ssrc[i] & MaxUintFromFormat(dstform);
    } else {
      result = usrc[i] & MaxUintFromFormat(dstform);
    }

    if (dstIsSigned) {
      dst.SetInt(dstform, offset + i, result);
    } else {
      dst.SetUint(dstform, offset + i, result);
    }
  }
  return dst;
}


LogicVRegister Simulator::xtn(VectorFormat vform,
                              LogicVRegister dst,
                              const LogicVRegister& src) {
  return extractnarrow(vform, dst, true, src, true);
}


LogicVRegister Simulator::sqxtn(VectorFormat vform,
                                LogicVRegister dst,
                                const LogicVRegister& src) {
  return extractnarrow(vform, dst, true, src, true).SignedSaturate(vform);
}


LogicVRegister Simulator::sqxtun(VectorFormat vform,
                                 LogicVRegister dst,
                                 const LogicVRegister& src) {
  return extractnarrow(vform, dst, false, src, true).UnsignedSaturate(vform);
}


LogicVRegister Simulator::uqxtn(VectorFormat vform,
                                LogicVRegister dst,
                                const LogicVRegister& src) {
  return extractnarrow(vform, dst, false, src, false).UnsignedSaturate(vform);
}


LogicVRegister Simulator::absdiff(VectorFormat vform,
                                  LogicVRegister dst,
                                  const LogicVRegister& src1,
                                  const LogicVRegister& src2,
                                  bool issigned) {
  dst.ClearForWrite(vform);
  for (int i = 0; i < LaneCountFromFormat(vform); i++) {
    if (issigned) {
      int64_t sr = src1.Int(vform, i) - src2.Int(vform, i);
      sr = sr > 0 ? sr : -sr;
      dst.SetInt(vform, i, sr);
    } else {
      int64_t sr = src1.Uint(vform, i) - src2.Uint(vform, i);
      sr = sr > 0 ? sr : -sr;
      dst.SetUint(vform, i, sr);
    }
  }
  return dst;
}


LogicVRegister Simulator::saba(VectorFormat vform,
                               LogicVRegister dst,
                               const LogicVRegister& src1,
                               const LogicVRegister& src2) {
  SimVRegister temp;
  dst.ClearForWrite(vform);
  absdiff(vform, temp, src1, src2, true);
  add(vform, dst, dst, temp);
  return dst;
}


LogicVRegister Simulator::uaba(VectorFormat vform,
                               LogicVRegister dst,
                               const LogicVRegister& src1,
                               const LogicVRegister& src2) {
  SimVRegister temp;
  dst.ClearForWrite(vform);
  absdiff(vform, temp, src1, src2, false);
  add(vform, dst, dst, temp);
  return dst;
}


LogicVRegister Simulator::not_(VectorFormat vform,
                               LogicVRegister dst,
                               const LogicVRegister& src) {
  dst.ClearForWrite(vform);
  for (int i = 0; i < LaneCountFromFormat(vform); i++) {
    dst.SetUint(vform, i, ~src.Uint(vform, i));
  }
  return dst;
}


LogicVRegister Simulator::rbit(VectorFormat vform,
                               LogicVRegister dst,
                               const LogicVRegister& src) {
  uint64_t result[16];
  int laneCount = LaneCountFromFormat(vform);
  int laneSizeInBits  = LaneSizeInBitsFromFormat(vform);
  uint64_t reversed_value;
  uint64_t value;
  for (int i = 0; i < laneCount; i++) {
    value = src.Uint(vform, i);
    reversed_value = 0;
    for (int j = 0; j < laneSizeInBits; j++) {
      reversed_value = (reversed_value << 1) | (value & 1);
      value >>= 1;
    }
    result[i] = reversed_value;
  }

  dst.ClearForWrite(vform);
  for (int i = 0; i < laneCount; ++i) {
    dst.SetUint(vform, i, result[i]);
  }
  return dst;
}


LogicVRegister Simulator::rev(VectorFormat vform,
                              LogicVRegister dst,
                              const LogicVRegister& src,
                              int revSize) {
  uint64_t result[16];
  int laneCount = LaneCountFromFormat(vform);
  int laneSize = LaneSizeInBytesFromFormat(vform);
  int lanesPerLoop =  revSize / laneSize;
  for (int i = 0; i < laneCount; i += lanesPerLoop) {
    for (int j = 0; j < lanesPerLoop; j++) {
      result[i + lanesPerLoop - 1 - j] = src.Uint(vform, i + j);
    }
  }
  dst.ClearForWrite(vform);
  for (int i = 0; i < laneCount; ++i) {
    dst.SetUint(vform, i, result[i]);
  }
  return dst;
}


LogicVRegister Simulator::rev16(VectorFormat vform,
                                LogicVRegister dst,
                                const LogicVRegister& src) {
  return rev(vform, dst, src, 2);
}


LogicVRegister Simulator::rev32(VectorFormat vform,
                                LogicVRegister dst,
                                const LogicVRegister& src) {
  return rev(vform, dst, src, 4);
}


LogicVRegister Simulator::rev64(VectorFormat vform,
                                LogicVRegister dst,
                                const LogicVRegister& src) {
  return rev(vform, dst, src, 8);
}


LogicVRegister Simulator::addlp(VectorFormat vform,
                                 LogicVRegister dst,
                                 const LogicVRegister& src,
                                 bool is_signed,
                                 bool do_accumulate) {
  VectorFormat vformsrc = VectorFormatHalfWidthDoubleLanes(vform);

  int64_t  sr[16];
  uint64_t ur[16];

  int laneCount = LaneCountFromFormat(vform);
  for (int i = 0; i < laneCount; ++i) {
    if (is_signed) {
      sr[i] = src.Int(vformsrc, 2 * i) + src.Int(vformsrc, 2 * i + 1);
    } else {
      ur[i] = src.Uint(vformsrc, 2 * i) + src.Uint(vformsrc, 2 * i + 1);
    }
  }

  dst.ClearForWrite(vform);
  for (int i = 0; i < laneCount; ++i) {
    if (do_accumulate) {
      if (is_signed) {
        dst.SetInt(vform, i, dst.Int(vform, i) + sr[i]);
      } else {
        dst.SetUint(vform, i, dst.Uint(vform, i) + ur[i]);
      }
    } else {
      if (is_signed) {
        dst.SetInt(vform, i, sr[i]);
      } else {
        dst.SetUint(vform, i, ur[i]);
      }
    }
  }

  return dst;
}


LogicVRegister Simulator::saddlp(VectorFormat vform,
                                 LogicVRegister dst,
                                 const LogicVRegister& src) {
  return addlp(vform, dst, src, true, false);
}


LogicVRegister Simulator::uaddlp(VectorFormat vform,
                                 LogicVRegister dst,
                                 const LogicVRegister& src) {
  return addlp(vform, dst, src, false, false);
}


LogicVRegister Simulator::sadalp(VectorFormat vform,
                                 LogicVRegister dst,
                                 const LogicVRegister& src) {
  return addlp(vform, dst, src, true, true);
}


LogicVRegister Simulator::uadalp(VectorFormat vform,
                                 LogicVRegister dst,
                                 const LogicVRegister& src) {
  return addlp(vform, dst, src, false, true);
}


LogicVRegister Simulator::ext(VectorFormat vform,
                              LogicVRegister dst,
                              const LogicVRegister& src1,
                              const LogicVRegister& src2,
                              int index) {
  uint8_t result[16];
  int laneCount = LaneCountFromFormat(vform);
  for (int i = 0; i < laneCount - index; ++i) {
    result[i] = src1.Uint(vform, i + index);
  }
  for (int i = 0; i < index; ++i) {
    result[laneCount - index + i] = src2.Uint(vform, i);
  }
  dst.ClearForWrite(vform);
  for (int i = 0; i < laneCount; ++i) {
    dst.SetUint(vform, i, result[i]);
  }
  return dst;
}


LogicVRegister Simulator::dup_element(VectorFormat vform,
                                      LogicVRegister dst,
                                      const LogicVRegister& src,
                                      int src_index) {
  int laneCount = LaneCountFromFormat(vform);
  uint64_t value = src.Uint(vform, src_index);
  dst.ClearForWrite(vform);
  for (int i = 0; i < laneCount; ++i) {
    dst.SetUint(vform, i, value);
  }
  return dst;
}


LogicVRegister Simulator::dup_immediate(VectorFormat vform,
                                        LogicVRegister dst,
                                        uint64_t imm) {
  int laneCount = LaneCountFromFormat(vform);
  uint64_t value = imm & MaxUintFromFormat(vform);
  dst.ClearForWrite(vform);
  for (int i = 0; i < laneCount; ++i) {
    dst.SetUint(vform, i, value);
  }
  return dst;
}


LogicVRegister Simulator::ins_element(VectorFormat vform,
                                      LogicVRegister dst,
                                      int dst_index,
                                      const LogicVRegister& src,
                                      int src_index) {
  dst.SetUint(vform, dst_index, src.Uint(vform, src_index));
  return dst;
}


LogicVRegister Simulator::ins_immediate(VectorFormat vform,
                                        LogicVRegister dst,
                                        int dst_index,
                                        uint64_t imm) {
  uint64_t value = imm & MaxUintFromFormat(vform);
  dst.SetUint(vform, dst_index, value);
  return dst;
}


LogicVRegister Simulator::movi(VectorFormat vform,
                               LogicVRegister dst,
                               uint64_t imm) {
  int laneCount = LaneCountFromFormat(vform);
  dst.ClearForWrite(vform);
  for (int i = 0; i < laneCount; ++i) {
    dst.SetUint(vform, i, imm);
  }
  return dst;
}


LogicVRegister Simulator::mvni(VectorFormat vform,
                               LogicVRegister dst,
                               uint64_t imm) {
  int laneCount = LaneCountFromFormat(vform);
  dst.ClearForWrite(vform);
  for (int i = 0; i < laneCount; ++i) {
    dst.SetUint(vform, i, ~imm);
  }
  return dst;
}


LogicVRegister Simulator::orr(VectorFormat vform,
                              LogicVRegister dst,
                              const LogicVRegister& src,
                              uint64_t imm) {
  uint64_t result[16];
  int laneCount = LaneCountFromFormat(vform);
  for (int i = 0; i < laneCount; ++i) {
    result[i] = src.Uint(vform, i) | imm;
  }
  dst.ClearForWrite(vform);
  for (int i = 0; i < laneCount; ++i) {
    dst.SetUint(vform, i, result[i]);
  }
  return dst;
}


LogicVRegister Simulator::uxtl(VectorFormat vform,
                               LogicVRegister dst,
                               const LogicVRegister& src) {
  VectorFormat vform_half = VectorFormatHalfWidth(vform);

  dst.ClearForWrite(vform);
  for (int i = 0; i < LaneCountFromFormat(vform); i++) {
    dst.SetUint(vform, i, src.Uint(vform_half, i));
  }
  return dst;
}


LogicVRegister Simulator::sxtl(VectorFormat vform,
                               LogicVRegister dst,
                               const LogicVRegister& src) {
  VectorFormat vform_half = VectorFormatHalfWidth(vform);

  dst.ClearForWrite(vform);
  for (int i = 0; i < LaneCountFromFormat(vform); i++) {
    dst.SetInt(vform, i, src.Int(vform_half, i));
  }
  return dst;
}


LogicVRegister Simulator::uxtl2(VectorFormat vform,
                                LogicVRegister dst,
                                const LogicVRegister& src) {
  VectorFormat vform_half = VectorFormatHalfWidth(vform);
  int lane_count = LaneCountFromFormat(vform);

  dst.ClearForWrite(vform);
  for (int i = 0; i < lane_count; i++) {
    dst.SetUint(vform, i, src.Uint(vform_half, lane_count + i));
  }
  return dst;
}


LogicVRegister Simulator::sxtl2(VectorFormat vform,
                                LogicVRegister dst,
                                const LogicVRegister& src) {
  VectorFormat vform_half = VectorFormatHalfWidth(vform);
  int lane_count = LaneCountFromFormat(vform);

  dst.ClearForWrite(vform);
  for (int i = 0; i < lane_count; i++) {
    dst.SetInt(vform, i, src.Int(vform_half, lane_count + i));
  }
  return dst;
}


LogicVRegister Simulator::shrn(VectorFormat vform,
                               LogicVRegister dst,
                               const LogicVRegister& src,
                               int shift) {
  SimVRegister temp;
  VectorFormat vform_src = VectorFormatDoubleWidth(vform);
  VectorFormat vform_dst = vform;
  LogicVRegister shifted_src = ushr(vform_src, temp, src, shift);
  return extractnarrow(vform_dst, dst, false, shifted_src, false);
}


LogicVRegister Simulator::shrn2(VectorFormat vform,
                                LogicVRegister dst,
                                const LogicVRegister& src,
                                int shift) {
  SimVRegister temp;
  VectorFormat vformsrc = VectorFormatDoubleWidth(VectorFormatHalfLanes(vform));
  VectorFormat vformdst = vform;
  LogicVRegister shifted_src = ushr(vformsrc, temp, src, shift);
  return extractnarrow(vformdst, dst, false, shifted_src, false);
}


LogicVRegister Simulator::rshrn(VectorFormat vform,
                                LogicVRegister dst,
                                const LogicVRegister& src,
                                int shift) {
  SimVRegister temp;
  VectorFormat vformsrc = VectorFormatDoubleWidth(vform);
  VectorFormat vformdst = vform;
  LogicVRegister shifted_src = ushr(vformsrc, temp, src, shift).Round(vformsrc);
  return extractnarrow(vformdst, dst, false, shifted_src, false);
}


LogicVRegister Simulator::rshrn2(VectorFormat vform,
                                 LogicVRegister dst,
                                 const LogicVRegister& src,
                                 int shift) {
  SimVRegister temp;
  VectorFormat vformsrc = VectorFormatDoubleWidth(VectorFormatHalfLanes(vform));
  VectorFormat vformdst = vform;
  LogicVRegister shifted_src = ushr(vformsrc, temp, src, shift).Round(vformsrc);
  return extractnarrow(vformdst, dst, false, shifted_src, false);
}


LogicVRegister Simulator::tbl(VectorFormat vform,
                              LogicVRegister dst,
                              const LogicVRegister& tab,
                              const LogicVRegister& ind) {
    movi(vform, dst, 0);
    return tbx(vform, dst, tab, ind);
}


LogicVRegister Simulator::tbl(VectorFormat vform,
                              LogicVRegister dst,
                              const LogicVRegister& tab,
                              const LogicVRegister& tab2,
                              const LogicVRegister& ind) {
    movi(vform, dst, 0);
    return tbx(vform, dst, tab, tab2, ind);
}


LogicVRegister Simulator::tbl(VectorFormat vform,
                              LogicVRegister dst,
                              const LogicVRegister& tab,
                              const LogicVRegister& tab2,
                              const LogicVRegister& tab3,
                              const LogicVRegister& ind) {
    movi(vform, dst, 0);
    return tbx(vform, dst, tab, tab2, tab3, ind);
}


LogicVRegister Simulator::tbl(VectorFormat vform,
                              LogicVRegister dst,
                              const LogicVRegister& tab,
                              const LogicVRegister& tab2,
                              const LogicVRegister& tab3,
                              const LogicVRegister& tab4,
                              const LogicVRegister& ind) {
    movi(vform, dst, 0);
    return tbx(vform, dst, tab, tab2, tab3, tab4, ind);
}


LogicVRegister Simulator::tbx(VectorFormat vform,
                              LogicVRegister dst,
                              const LogicVRegister& tab,
                              const LogicVRegister& ind) {
  dst.ClearForWrite(vform);
  for (int i = 0; i < LaneCountFromFormat(vform); i++) {
    uint64_t j = ind.Uint(vform, i);
    switch (j >> 4) {
      case 0: dst.SetUint(vform, i, tab.Uint(kFormat16B, j & 15)); break;
    }
  }
  return dst;
}


LogicVRegister Simulator::tbx(VectorFormat vform,
                              LogicVRegister dst,
                              const LogicVRegister& tab,
                              const LogicVRegister& tab2,
                              const LogicVRegister& ind) {
  dst.ClearForWrite(vform);
  for (int i = 0; i < LaneCountFromFormat(vform); i++) {
    uint64_t j = ind.Uint(vform, i);
    switch (j >> 4) {
      case 0: dst.SetUint(vform, i, tab.Uint(kFormat16B, j & 15)); break;
      case 1: dst.SetUint(vform, i, tab2.Uint(kFormat16B, j & 15)); break;
    }
  }
  return dst;
}


LogicVRegister Simulator::tbx(VectorFormat vform,
                              LogicVRegister dst,
                              const LogicVRegister& tab,
                              const LogicVRegister& tab2,
                              const LogicVRegister& tab3,
                              const LogicVRegister& ind) {
  dst.ClearForWrite(vform);
  for (int i = 0; i < LaneCountFromFormat(vform); i++) {
    uint64_t j = ind.Uint(vform, i);
    switch (j >> 4) {
      case 0: dst.SetUint(vform, i, tab.Uint(kFormat16B, j & 15)); break;
      case 1: dst.SetUint(vform, i, tab2.Uint(kFormat16B, j & 15)); break;
      case 2: dst.SetUint(vform, i, tab3.Uint(kFormat16B, j & 15)); break;
    }
  }
  return dst;
}


LogicVRegister Simulator::tbx(VectorFormat vform,
                              LogicVRegister dst,
                              const LogicVRegister& tab,
                              const LogicVRegister& tab2,
                              const LogicVRegister& tab3,
                              const LogicVRegister& tab4,
                              const LogicVRegister& ind) {
  dst.ClearForWrite(vform);
  for (int i = 0; i < LaneCountFromFormat(vform); i++) {
    uint64_t j = ind.Uint(vform, i);
    switch (j >> 4) {
      case 0: dst.SetUint(vform, i, tab.Uint(kFormat16B, j & 15)); break;
      case 1: dst.SetUint(vform, i, tab2.Uint(kFormat16B, j & 15)); break;
      case 2: dst.SetUint(vform, i, tab3.Uint(kFormat16B, j & 15)); break;
      case 3: dst.SetUint(vform, i, tab4.Uint(kFormat16B, j & 15)); break;
    }
  }
  return dst;
}


LogicVRegister Simulator::uqshrn(VectorFormat vform,
                                 LogicVRegister dst,
                                 const LogicVRegister& src,
                                 int shift) {
  return shrn(vform, dst, src, shift).UnsignedSaturate(vform);
}


LogicVRegister Simulator::uqshrn2(VectorFormat vform,
                                  LogicVRegister dst,
                                  const LogicVRegister& src,
                                  int shift) {
  return shrn2(vform, dst, src, shift).UnsignedSaturate(vform);
}


LogicVRegister Simulator::uqrshrn(VectorFormat vform,
                                  LogicVRegister dst,
                                  const LogicVRegister& src,
                                  int shift) {
  return rshrn(vform, dst, src, shift).UnsignedSaturate(vform);
}


LogicVRegister Simulator::uqrshrn2(VectorFormat vform,
                                   LogicVRegister dst,
                                   const LogicVRegister& src,
                                   int shift) {
  return rshrn2(vform, dst, src, shift).UnsignedSaturate(vform);
}


LogicVRegister Simulator::sqshrn(VectorFormat vform,
                                 LogicVRegister dst,
                                 const LogicVRegister& src,
                                 int shift) {
  SimVRegister temp;
  VectorFormat vformsrc = VectorFormatDoubleWidth(vform);
  VectorFormat vformdst = vform;
  LogicVRegister shifted_src = sshr(vformsrc, temp, src, shift);
  return sqxtn(vformdst, dst, shifted_src);
}


LogicVRegister Simulator::sqshrn2(VectorFormat vform,
                                  LogicVRegister dst,
                                  const LogicVRegister& src,
                                  int shift) {
  SimVRegister temp;
  VectorFormat vformsrc = VectorFormatDoubleWidth(VectorFormatHalfLanes(vform));
  VectorFormat vformdst = vform;
  LogicVRegister shifted_src = sshr(vformsrc, temp, src, shift);
  return sqxtn(vformdst, dst, shifted_src);
}


LogicVRegister Simulator::sqrshrn(VectorFormat vform,
                                  LogicVRegister dst,
                                  const LogicVRegister& src,
                                  int shift) {
  SimVRegister temp;
  VectorFormat vformsrc = VectorFormatDoubleWidth(vform);
  VectorFormat vformdst = vform;
  LogicVRegister shifted_src = sshr(vformsrc, temp, src, shift).Round(vformsrc);
  return sqxtn(vformdst, dst, shifted_src);
}


LogicVRegister Simulator::sqrshrn2(VectorFormat vform,
                                   LogicVRegister dst,
                                   const LogicVRegister& src,
                                   int shift) {
  SimVRegister temp;
  VectorFormat vformsrc = VectorFormatDoubleWidth(VectorFormatHalfLanes(vform));
  VectorFormat vformdst = vform;
  LogicVRegister shifted_src = sshr(vformsrc, temp, src, shift).Round(vformsrc);
  return sqxtn(vformdst, dst, shifted_src);
}


LogicVRegister Simulator::sqshrun(VectorFormat vform,
                                  LogicVRegister dst,
                                  const LogicVRegister& src,
                                  int shift) {
  SimVRegister temp;
  VectorFormat vformsrc = VectorFormatDoubleWidth(vform);
  VectorFormat vformdst = vform;
  LogicVRegister shifted_src = sshr(vformsrc, temp, src, shift);
  return sqxtun(vformdst, dst, shifted_src);
}


LogicVRegister Simulator::sqshrun2(VectorFormat vform,
                                   LogicVRegister dst,
                                   const LogicVRegister& src,
                                   int shift) {
  SimVRegister temp;
  VectorFormat vformsrc = VectorFormatDoubleWidth(VectorFormatHalfLanes(vform));
  VectorFormat vformdst = vform;
  LogicVRegister shifted_src = sshr(vformsrc, temp, src, shift);
  return sqxtun(vformdst, dst, shifted_src);
}


LogicVRegister Simulator::sqrshrun(VectorFormat vform,
                                   LogicVRegister dst,
                                   const LogicVRegister& src,
                                   int shift) {
  SimVRegister temp;
  VectorFormat vformsrc = VectorFormatDoubleWidth(vform);
  VectorFormat vformdst = vform;
  LogicVRegister shifted_src = sshr(vformsrc, temp, src, shift).Round(vformsrc);
  return sqxtun(vformdst, dst, shifted_src);
}


LogicVRegister Simulator::sqrshrun2(VectorFormat vform,
                                    LogicVRegister dst,
                                    const LogicVRegister& src,
                                    int shift) {
  SimVRegister temp;
  VectorFormat vformsrc = VectorFormatDoubleWidth(VectorFormatHalfLanes(vform));
  VectorFormat vformdst = vform;
  LogicVRegister shifted_src = sshr(vformsrc, temp, src, shift).Round(vformsrc);
  return sqxtun(vformdst, dst, shifted_src);
}


LogicVRegister Simulator::uaddl(VectorFormat vform,
                                LogicVRegister dst,
                                const LogicVRegister& src1,
                                const LogicVRegister& src2) {
  SimVRegister temp1, temp2;
  uxtl(vform, temp1, src1);
  uxtl(vform, temp2, src2);
  add(vform, dst, temp1, temp2);
  return dst;
}


LogicVRegister Simulator::uaddl2(VectorFormat vform,
                                 LogicVRegister dst,
                                 const LogicVRegister& src1,
                                 const LogicVRegister& src2) {
  SimVRegister temp1, temp2;
  uxtl2(vform, temp1, src1);
  uxtl2(vform, temp2, src2);
  add(vform, dst, temp1, temp2);
  return dst;
}


LogicVRegister Simulator::uaddw(VectorFormat vform,
                                LogicVRegister dst,
                                const LogicVRegister& src1,
                                const LogicVRegister& src2) {
  SimVRegister temp;
  uxtl(vform, temp, src2);
  add(vform, dst, src1, temp);
  return dst;
}


LogicVRegister Simulator::uaddw2(VectorFormat vform,
                                 LogicVRegister dst,
                                 const LogicVRegister& src1,
                                 const LogicVRegister& src2) {
  SimVRegister temp;
  uxtl2(vform, temp, src2);
  add(vform, dst, src1, temp);
  return dst;
}


LogicVRegister Simulator::saddl(VectorFormat vform,
                                LogicVRegister dst,
                                const LogicVRegister& src1,
                                const LogicVRegister& src2) {
  SimVRegister temp1, temp2;
  sxtl(vform, temp1, src1);
  sxtl(vform, temp2, src2);
  add(vform, dst, temp1, temp2);
  return dst;
}


LogicVRegister Simulator::saddl2(VectorFormat vform,
                                 LogicVRegister dst,
                                 const LogicVRegister& src1,
                                 const LogicVRegister& src2) {
  SimVRegister temp1, temp2;
  sxtl2(vform, temp1, src1);
  sxtl2(vform, temp2, src2);
  add(vform, dst, temp1, temp2);
  return dst;
}


LogicVRegister Simulator::saddw(VectorFormat vform,
                                LogicVRegister dst,
                                const LogicVRegister& src1,
                                const LogicVRegister& src2) {
  SimVRegister temp;
  sxtl(vform, temp, src2);
  add(vform, dst, src1, temp);
  return dst;
}


LogicVRegister Simulator::saddw2(VectorFormat vform,
                                 LogicVRegister dst,
                                 const LogicVRegister& src1,
                                 const LogicVRegister& src2) {
  SimVRegister temp;
  sxtl2(vform, temp, src2);
  add(vform, dst, src1, temp);
  return dst;
}


LogicVRegister Simulator::usubl(VectorFormat vform,
                                LogicVRegister dst,
                                const LogicVRegister& src1,
                                const LogicVRegister& src2) {
  SimVRegister temp1, temp2;
  uxtl(vform, temp1, src1);
  uxtl(vform, temp2, src2);
  sub(vform, dst, temp1, temp2);
  return dst;
}


LogicVRegister Simulator::usubl2(VectorFormat vform,
                                 LogicVRegister dst,
                                 const LogicVRegister& src1,
                                 const LogicVRegister& src2) {
  SimVRegister temp1, temp2;
  uxtl2(vform, temp1, src1);
  uxtl2(vform, temp2, src2);
  sub(vform, dst, temp1, temp2);
  return dst;
}


LogicVRegister Simulator::usubw(VectorFormat vform,
                                LogicVRegister dst,
                                const LogicVRegister& src1,
                                const LogicVRegister& src2) {
  SimVRegister temp;
  uxtl(vform, temp, src2);
  sub(vform, dst, src1, temp);
  return dst;
}


LogicVRegister Simulator::usubw2(VectorFormat vform,
                                 LogicVRegister dst,
                                 const LogicVRegister& src1,
                                 const LogicVRegister& src2) {
  SimVRegister temp;
  uxtl2(vform, temp, src2);
  sub(vform, dst, src1, temp);
  return dst;
}


LogicVRegister Simulator::ssubl(VectorFormat vform,
                                LogicVRegister dst,
                                const LogicVRegister& src1,
                                const LogicVRegister& src2) {
  SimVRegister temp1, temp2;
  sxtl(vform, temp1, src1);
  sxtl(vform, temp2, src2);
  sub(vform, dst, temp1, temp2);
  return dst;
}


LogicVRegister Simulator::ssubl2(VectorFormat vform,
                                 LogicVRegister dst,
                                 const LogicVRegister& src1,
                                 const LogicVRegister& src2) {
  SimVRegister temp1, temp2;
  sxtl2(vform, temp1, src1);
  sxtl2(vform, temp2, src2);
  sub(vform, dst, temp1, temp2);
  return dst;
}


LogicVRegister Simulator::ssubw(VectorFormat vform,
                                LogicVRegister dst,
                                const LogicVRegister& src1,
                                const LogicVRegister& src2) {
  SimVRegister temp;
  sxtl(vform, temp, src2);
  sub(vform, dst, src1, temp);
  return dst;
}


LogicVRegister Simulator::ssubw2(VectorFormat vform,
                                 LogicVRegister dst,
                                 const LogicVRegister& src1,
                                 const LogicVRegister& src2) {
  SimVRegister temp;
  sxtl2(vform, temp, src2);
  sub(vform, dst, src1, temp);
  return dst;
}


LogicVRegister Simulator::uabal(VectorFormat vform,
                                LogicVRegister dst,
                                const LogicVRegister& src1,
                                const LogicVRegister& src2) {
  SimVRegister temp1, temp2;
  uxtl(vform, temp1, src1);
  uxtl(vform, temp2, src2);
  uaba(vform, dst, temp1, temp2);
  return dst;
}


LogicVRegister Simulator::uabal2(VectorFormat vform,
                                 LogicVRegister dst,
                                 const LogicVRegister& src1,
                                 const LogicVRegister& src2) {
  SimVRegister temp1, temp2;
  uxtl2(vform, temp1, src1);
  uxtl2(vform, temp2, src2);
  uaba(vform, dst, temp1, temp2);
  return dst;
}


LogicVRegister Simulator::sabal(VectorFormat vform,
                                LogicVRegister dst,
                                const LogicVRegister& src1,
                                const LogicVRegister& src2) {
  SimVRegister temp1, temp2;
  sxtl(vform, temp1, src1);
  sxtl(vform, temp2, src2);
  saba(vform, dst, temp1, temp2);
  return dst;
}


LogicVRegister Simulator::sabal2(VectorFormat vform,
                                 LogicVRegister dst,
                                 const LogicVRegister& src1,
                                 const LogicVRegister& src2) {
  SimVRegister temp1, temp2;
  sxtl2(vform, temp1, src1);
  sxtl2(vform, temp2, src2);
  saba(vform, dst, temp1, temp2);
  return dst;
}


LogicVRegister Simulator::uabdl(VectorFormat vform,
                                LogicVRegister dst,
                                const LogicVRegister& src1,
                                const LogicVRegister& src2) {
  SimVRegister temp1, temp2;
  uxtl(vform, temp1, src1);
  uxtl(vform, temp2, src2);
  absdiff(vform, dst, temp1, temp2, false);
  return dst;
}


LogicVRegister Simulator::uabdl2(VectorFormat vform,
                                 LogicVRegister dst,
                                 const LogicVRegister& src1,
                                 const LogicVRegister& src2) {
  SimVRegister temp1, temp2;
  uxtl2(vform, temp1, src1);
  uxtl2(vform, temp2, src2);
  absdiff(vform, dst, temp1, temp2, false);
  return dst;
}


LogicVRegister Simulator::sabdl(VectorFormat vform,
                                LogicVRegister dst,
                                const LogicVRegister& src1,
                                const LogicVRegister& src2) {
  SimVRegister temp1, temp2;
  sxtl(vform, temp1, src1);
  sxtl(vform, temp2, src2);
  absdiff(vform, dst, temp1, temp2, true);
  return dst;
}


LogicVRegister Simulator::sabdl2(VectorFormat vform,
                                 LogicVRegister dst,
                                 const LogicVRegister& src1,
                                 const LogicVRegister& src2) {
  SimVRegister temp1, temp2;
  sxtl2(vform, temp1, src1);
  sxtl2(vform, temp2, src2);
  absdiff(vform, dst, temp1, temp2, true);
  return dst;
}


LogicVRegister Simulator::umull(VectorFormat vform,
                                LogicVRegister dst,
                                const LogicVRegister& src1,
                                const LogicVRegister& src2) {
  SimVRegister temp1, temp2;
  uxtl(vform, temp1, src1);
  uxtl(vform, temp2, src2);
  mul(vform, dst, temp1, temp2);
  return dst;
}


LogicVRegister Simulator::umull2(VectorFormat vform,
                                 LogicVRegister dst,
                                 const LogicVRegister& src1,
                                 const LogicVRegister& src2) {
  SimVRegister temp1, temp2;
  uxtl2(vform, temp1, src1);
  uxtl2(vform, temp2, src2);
  mul(vform, dst, temp1, temp2);
  return dst;
}


LogicVRegister Simulator::smull(VectorFormat vform,
                                LogicVRegister dst,
                                const LogicVRegister& src1,
                                const LogicVRegister& src2) {
  SimVRegister temp1, temp2;
  sxtl(vform, temp1, src1);
  sxtl(vform, temp2, src2);
  mul(vform, dst, temp1, temp2);
  return dst;
}


LogicVRegister Simulator::smull2(VectorFormat vform,
                                 LogicVRegister dst,
                                 const LogicVRegister& src1,
                                 const LogicVRegister& src2) {
  SimVRegister temp1, temp2;
  sxtl2(vform, temp1, src1);
  sxtl2(vform, temp2, src2);
  mul(vform, dst, temp1, temp2);
  return dst;
}


LogicVRegister Simulator::umlsl(VectorFormat vform,
                                LogicVRegister dst,
                                const LogicVRegister& src1,
                                const LogicVRegister& src2) {
  SimVRegister temp1, temp2;
  uxtl(vform, temp1, src1);
  uxtl(vform, temp2, src2);
  mls(vform, dst, temp1, temp2);
  return dst;
}


LogicVRegister Simulator::umlsl2(VectorFormat vform,
                                 LogicVRegister dst,
                                 const LogicVRegister& src1,
                                 const LogicVRegister& src2) {
  SimVRegister temp1, temp2;
  uxtl2(vform, temp1, src1);
  uxtl2(vform, temp2, src2);
  mls(vform, dst, temp1, temp2);
  return dst;
}


LogicVRegister Simulator::smlsl(VectorFormat vform,
                                LogicVRegister dst,
                                const LogicVRegister& src1,
                                const LogicVRegister& src2) {
  SimVRegister temp1, temp2;
  sxtl(vform, temp1, src1);
  sxtl(vform, temp2, src2);
  mls(vform, dst, temp1, temp2);
  return dst;
}


LogicVRegister Simulator::smlsl2(VectorFormat vform,
                                 LogicVRegister dst,
                                 const LogicVRegister& src1,
                                 const LogicVRegister& src2) {
  SimVRegister temp1, temp2;
  sxtl2(vform, temp1, src1);
  sxtl2(vform, temp2, src2);
  mls(vform, dst, temp1, temp2);
  return dst;
}


LogicVRegister Simulator::umlal(VectorFormat vform,
                                LogicVRegister dst,
                                const LogicVRegister& src1,
                                const LogicVRegister& src2) {
  SimVRegister temp1, temp2;
  uxtl(vform, temp1, src1);
  uxtl(vform, temp2, src2);
  mla(vform, dst, temp1, temp2);
  return dst;
}


LogicVRegister Simulator::umlal2(VectorFormat vform,
                                 LogicVRegister dst,
                                 const LogicVRegister& src1,
                                 const LogicVRegister& src2) {
  SimVRegister temp1, temp2;
  uxtl2(vform, temp1, src1);
  uxtl2(vform, temp2, src2);
  mla(vform, dst, temp1, temp2);
  return dst;
}


LogicVRegister Simulator::smlal(VectorFormat vform,
                                LogicVRegister dst,
                                const LogicVRegister& src1,
                                const LogicVRegister& src2) {
  SimVRegister temp1, temp2;
  sxtl(vform, temp1, src1);
  sxtl(vform, temp2, src2);
  mla(vform, dst, temp1, temp2);
  return dst;
}


LogicVRegister Simulator::smlal2(VectorFormat vform,
                                 LogicVRegister dst,
                                 const LogicVRegister& src1,
                                 const LogicVRegister& src2) {
  SimVRegister temp1, temp2;
  sxtl2(vform, temp1, src1);
  sxtl2(vform, temp2, src2);
  mla(vform, dst, temp1, temp2);
  return dst;
}


LogicVRegister Simulator::sqdmlal(VectorFormat vform,
                                  LogicVRegister dst,
                                  const LogicVRegister& src1,
                                  const LogicVRegister& src2) {
  SimVRegister temp;
  LogicVRegister product = sqdmull(vform, temp, src1, src2);
  return add(vform, dst, dst, product).SignedSaturate(vform);
}


LogicVRegister Simulator::sqdmlal2(VectorFormat vform,
                                  LogicVRegister dst,
                                  const LogicVRegister& src1,
                                  const LogicVRegister& src2) {
  SimVRegister temp;
  LogicVRegister product = sqdmull2(vform, temp, src1, src2);
  return add(vform, dst, dst, product).SignedSaturate(vform);
}


LogicVRegister Simulator::sqdmlsl(VectorFormat vform,
                                  LogicVRegister dst,
                                  const LogicVRegister& src1,
                                  const LogicVRegister& src2) {
  SimVRegister temp;
  LogicVRegister product = sqdmull(vform, temp, src1, src2);
  return sub(vform, dst, dst, product).SignedSaturate(vform);
}


LogicVRegister Simulator::sqdmlsl2(VectorFormat vform,
                                  LogicVRegister dst,
                                  const LogicVRegister& src1,
                                  const LogicVRegister& src2) {
  SimVRegister temp;
  LogicVRegister product = sqdmull2(vform, temp, src1, src2);
  return sub(vform, dst, dst, product).SignedSaturate(vform);
}


LogicVRegister Simulator::sqdmull(VectorFormat vform,
                                  LogicVRegister dst,
                                  const LogicVRegister& src1,
                                  const LogicVRegister& src2) {
  SimVRegister temp;
  LogicVRegister product = smull(vform, temp, src1, src2);
  return add(vform, dst, product, product).SignedSaturate(vform);
}


LogicVRegister Simulator::sqdmull2(VectorFormat vform,
                                  LogicVRegister dst,
                                  const LogicVRegister& src1,
                                  const LogicVRegister& src2) {
  SimVRegister temp;
  LogicVRegister product = smull2(vform, temp, src1, src2);
  return add(vform, dst, product, product).SignedSaturate(vform);
}


LogicVRegister Simulator::sqrdmulh(VectorFormat vform,
                                   LogicVRegister dst,
                                   const LogicVRegister& src1,
                                   const LogicVRegister& src2,
                                   bool round) {
  // 2 * INT_32_MIN * INT_32_MIN causes int64_t to overflow.
  // To avoid this, we use (src1 * src2 + 1 << (esize - 2)) >> (esize - 1)
  // which is same as (2 * src1 * src2 + 1 << (esize - 1)) >> esize.

  int esize = LaneSizeInBitsFromFormat(vform);
  int round_const = round ? (1 << (esize - 2)) : 0;
  int64_t product;

  dst.ClearForWrite(vform);
  for (int i = 0; i < LaneCountFromFormat(vform); i++) {
    product = src1.Int(vform, i) * src2.Int(vform, i);
    product += round_const;
    product = product >> (esize - 1);

    if (product > MaxIntFromFormat(vform)) {
      product = MaxIntFromFormat(vform);
    } else if (product < MinIntFromFormat(vform)) {
      product = MinIntFromFormat(vform);
    }
    dst.SetInt(vform, i, product);
  }
  return dst;
}


LogicVRegister Simulator::sqdmulh(VectorFormat vform,
                                  LogicVRegister dst,
                                  const LogicVRegister& src1,
                                  const LogicVRegister& src2) {
  return sqrdmulh(vform, dst, src1, src2, false);
}


LogicVRegister Simulator::addhn(VectorFormat vform,
                                LogicVRegister dst,
                                const LogicVRegister& src1,
                                const LogicVRegister& src2) {
  SimVRegister temp;
  add(VectorFormatDoubleWidth(vform), temp, src1, src2);
  shrn(vform, dst, temp, LaneSizeInBitsFromFormat(vform));
  return dst;
}


LogicVRegister Simulator::addhn2(VectorFormat vform,
                                 LogicVRegister dst,
                                 const LogicVRegister& src1,
                                 const LogicVRegister& src2) {
  SimVRegister temp;
  add(VectorFormatDoubleWidth(VectorFormatHalfLanes(vform)), temp, src1, src2);
  shrn2(vform, dst, temp, LaneSizeInBitsFromFormat(vform));
  return dst;
}


LogicVRegister Simulator::raddhn(VectorFormat vform,
                                 LogicVRegister dst,
                                 const LogicVRegister& src1,
                                 const LogicVRegister& src2) {
  SimVRegister temp;
  add(VectorFormatDoubleWidth(vform), temp, src1, src2);
  rshrn(vform, dst, temp, LaneSizeInBitsFromFormat(vform));
  return dst;
}


LogicVRegister Simulator::raddhn2(VectorFormat vform,
                                  LogicVRegister dst,
                                  const LogicVRegister& src1,
                                  const LogicVRegister& src2) {
  SimVRegister temp;
  add(VectorFormatDoubleWidth(VectorFormatHalfLanes(vform)), temp, src1, src2);
  rshrn2(vform, dst, temp, LaneSizeInBitsFromFormat(vform));
  return dst;
}


LogicVRegister Simulator::subhn(VectorFormat vform,
                                LogicVRegister dst,
                                const LogicVRegister& src1,
                                const LogicVRegister& src2) {
  SimVRegister temp;
  sub(VectorFormatDoubleWidth(vform), temp, src1, src2);
  shrn(vform, dst, temp, LaneSizeInBitsFromFormat(vform));
  return dst;
}


LogicVRegister Simulator::subhn2(VectorFormat vform,
                                 LogicVRegister dst,
                                 const LogicVRegister& src1,
                                 const LogicVRegister& src2) {
  SimVRegister temp;
  sub(VectorFormatDoubleWidth(VectorFormatHalfLanes(vform)), temp, src1, src2);
  shrn2(vform, dst, temp, LaneSizeInBitsFromFormat(vform));
  return dst;
}


LogicVRegister Simulator::rsubhn(VectorFormat vform,
                                 LogicVRegister dst,
                                 const LogicVRegister& src1,
                                 const LogicVRegister& src2) {
  SimVRegister temp;
  sub(VectorFormatDoubleWidth(vform), temp, src1, src2);
  rshrn(vform, dst, temp, LaneSizeInBitsFromFormat(vform));
  return dst;
}


LogicVRegister Simulator::rsubhn2(VectorFormat vform,
                                  LogicVRegister dst,
                                  const LogicVRegister& src1,
                                  const LogicVRegister& src2) {
  SimVRegister temp;
  sub(VectorFormatDoubleWidth(VectorFormatHalfLanes(vform)), temp, src1, src2);
  rshrn2(vform, dst, temp, LaneSizeInBitsFromFormat(vform));
  return dst;
}


LogicVRegister Simulator::trn1(VectorFormat vform,
                               LogicVRegister dst,
                               const LogicVRegister& src1,
                               const LogicVRegister& src2) {
  uint64_t result[16];
  int laneCount = LaneCountFromFormat(vform);
  int pairs = laneCount / 2;
  for (int i = 0; i < pairs; ++i) {
    result[2 * i]       = src1.Uint(vform, 2 * i);
    result[(2 * i) + 1] = src2.Uint(vform, 2 * i);
  }

  dst.ClearForWrite(vform);
  for (int i = 0; i < laneCount; ++i) {
    dst.SetUint(vform, i, result[i]);
  }
  return dst;
}


LogicVRegister Simulator::trn2(VectorFormat vform,
                               LogicVRegister dst,
                               const LogicVRegister& src1,
                               const LogicVRegister& src2) {
  uint64_t result[16];
  int laneCount = LaneCountFromFormat(vform);
  int pairs = laneCount / 2;
  for (int i = 0; i < pairs; ++i) {
    result[2 * i]       = src1.Uint(vform, (2 * i) + 1);
    result[(2 * i) + 1] = src2.Uint(vform, (2 * i) + 1);
  }

  dst.ClearForWrite(vform);
  for (int i = 0; i < laneCount; ++i) {
    dst.SetUint(vform, i, result[i]);
  }
  return dst;
}


LogicVRegister Simulator::zip1(VectorFormat vform,
                               LogicVRegister dst,
                               const LogicVRegister& src1,
                               const LogicVRegister& src2) {
  uint64_t result[16];
  int laneCount = LaneCountFromFormat(vform);
  int pairs = laneCount / 2;
  for (int i = 0; i < pairs; ++i) {
    result[2 * i]       = src1.Uint(vform, i);
    result[(2 * i) + 1] = src2.Uint(vform, i);
  }

  dst.ClearForWrite(vform);
  for (int i = 0; i < laneCount; ++i) {
    dst.SetUint(vform, i, result[i]);
  }
  return dst;
}


LogicVRegister Simulator::zip2(VectorFormat vform,
                               LogicVRegister dst,
                               const LogicVRegister& src1,
                               const LogicVRegister& src2) {
  uint64_t result[16];
  int laneCount = LaneCountFromFormat(vform);
  int pairs = laneCount / 2;
  for (int i = 0; i < pairs; ++i) {
    result[2 * i]       = src1.Uint(vform, pairs + i);
    result[(2 * i) + 1] = src2.Uint(vform, pairs + i);
  }

  dst.ClearForWrite(vform);
  for (int i = 0; i < laneCount; ++i) {
    dst.SetUint(vform, i, result[i]);
  }
  return dst;
}


LogicVRegister Simulator::uzp1(VectorFormat vform,
                               LogicVRegister dst,
                               const LogicVRegister& src1,
                               const LogicVRegister& src2) {
  uint64_t result[32];
  int laneCount = LaneCountFromFormat(vform);
  for (int i = 0; i < laneCount; ++i) {
    result[i]             = src1.Uint(vform, i);
    result[laneCount + i] = src2.Uint(vform, i);
  }

  dst.ClearForWrite(vform);
  for (int i = 0; i < laneCount; ++i) {
    dst.SetUint(vform, i, result[2 * i]);
  }
  return dst;
}


LogicVRegister Simulator::uzp2(VectorFormat vform,
                               LogicVRegister dst,
                               const LogicVRegister& src1,
                               const LogicVRegister& src2) {
  uint64_t result[32];
  int laneCount = LaneCountFromFormat(vform);
  for (int i = 0; i < laneCount; ++i) {
    result[i]             = src1.Uint(vform, i);
    result[laneCount + i] = src2.Uint(vform, i);
  }

  dst.ClearForWrite(vform);
  for (int i = 0; i < laneCount; ++i) {
    dst.SetUint(vform, i, result[ (2 * i) + 1]);
  }
  return dst;
}


template <typename T>
T Simulator::FPAdd(T op1, T op2) {
  T result = FPProcessNaNs(op1, op2);
  if (std::isnan(result)) return result;

  if (std::isinf(op1) && std::isinf(op2) && (op1 != op2)) {
    // inf + -inf returns the default NaN.
    FPProcessException();
    return FPDefaultNaN<T>();
  } else {
    // Other cases should be handled by standard arithmetic.
    return op1 + op2;
  }
}


template <typename T>
T Simulator::FPSub(T op1, T op2) {
  // NaNs should be handled elsewhere.
  VIXL_ASSERT(!std::isnan(op1) && !std::isnan(op2));

  if (std::isinf(op1) && std::isinf(op2) && (op1 == op2)) {
    // inf - inf returns the default NaN.
    FPProcessException();
    return FPDefaultNaN<T>();
  } else {
    // Other cases should be handled by standard arithmetic.
    return op1 - op2;
  }
}


template <typename T>
T Simulator::FPMul(T op1, T op2) {
  // NaNs should be handled elsewhere.
  VIXL_ASSERT(!std::isnan(op1) && !std::isnan(op2));

  if ((std::isinf(op1) && (op2 == 0.0)) || (std::isinf(op2) && (op1 == 0.0))) {
    // inf * 0.0 returns the default NaN.
    FPProcessException();
    return FPDefaultNaN<T>();
  } else {
    // Other cases should be handled by standard arithmetic.
    return op1 * op2;
  }
}


template<typename T>
T Simulator::FPMulx(T op1, T op2) {
  if ((std::isinf(op1) && (op2 == 0.0)) || (std::isinf(op2) && (op1 == 0.0))) {
    // inf * 0.0 returns +/-2.0.
    T two = 2.0;
    return copysign(1.0, op1) * copysign(1.0, op2) * two;
  }
  return FPMul(op1, op2);
}


template<typename T>
T Simulator::FPMulAdd(T a, T op1, T op2) {
  T result = FPProcessNaNs3(a, op1, op2);

  T sign_a = copysign(1.0, a);
  T sign_prod = copysign(1.0, op1) * copysign(1.0, op2);
  bool isinf_prod = std::isinf(op1) || std::isinf(op2);
  bool operation_generates_nan =
      (std::isinf(op1) && (op2 == 0.0)) ||                     // inf * 0.0
      (std::isinf(op2) && (op1 == 0.0)) ||                     // 0.0 * inf
      (std::isinf(a) && isinf_prod && (sign_a != sign_prod));  // inf - inf

  if (std::isnan(result)) {
    // Generated NaNs override quiet NaNs propagated from a.
    if (operation_generates_nan && IsQuietNaN(a)) {
      FPProcessException();
      return FPDefaultNaN<T>();
    } else {
      return result;
    }
  }

  // If the operation would produce a NaN, return the default NaN.
  if (operation_generates_nan) {
    FPProcessException();
    return FPDefaultNaN<T>();
  }

  // Work around broken fma implementations for exact zero results: The sign of
  // exact 0.0 results is positive unless both a and op1 * op2 are negative.
  if (((op1 == 0.0) || (op2 == 0.0)) && (a == 0.0)) {
    return ((sign_a < 0) && (sign_prod < 0)) ? -0.0 : 0.0;
  }

  result = FusedMultiplyAdd(op1, op2, a);
  VIXL_ASSERT(!std::isnan(result));

  // Work around broken fma implementations for rounded zero results: If a is
  // 0.0, the sign of the result is the sign of op1 * op2 before rounding.
  if ((a == 0.0) && (result == 0.0)) {
    return copysign(0.0, sign_prod);
  }

  return result;
}


template <typename T>
T Simulator::FPDiv(T op1, T op2) {
  // NaNs should be handled elsewhere.
  VIXL_ASSERT(!std::isnan(op1) && !std::isnan(op2));

  if ((std::isinf(op1) && std::isinf(op2)) || ((op1 == 0.0) && (op2 == 0.0))) {
    // inf / inf and 0.0 / 0.0 return the default NaN.
    FPProcessException();
    return FPDefaultNaN<T>();
  } else {
    if (op2 == 0.0) FPProcessException();

    // Other cases should be handled by standard arithmetic.
    return op1 / op2;
  }
}


template <typename T>
T Simulator::FPSqrt(T op) {
  if (std::isnan(op)) {
    return FPProcessNaN(op);
  } else if (op < 0.0) {
    FPProcessException();
    return FPDefaultNaN<T>();
  } else {
    return sqrt(op);
  }
}


template <typename T>
T Simulator::FPMax(T a, T b) {
  T result = FPProcessNaNs(a, b);
  if (std::isnan(result)) return result;

  if ((a == 0.0) && (b == 0.0) &&
      (copysign(1.0, a) != copysign(1.0, b))) {
    // a and b are zero, and the sign differs: return +0.0.
    return 0.0;
  } else {
    return (a > b) ? a : b;
  }
}


template <typename T>
T Simulator::FPMaxNM(T a, T b) {
  if (IsQuietNaN(a) && !IsQuietNaN(b)) {
    a = kFP64NegativeInfinity;
  } else if (!IsQuietNaN(a) && IsQuietNaN(b)) {
    b = kFP64NegativeInfinity;
  }

  T result = FPProcessNaNs(a, b);
  return std::isnan(result) ? result : FPMax(a, b);
}


template <typename T>
T Simulator::FPMin(T a, T b) {
  T result = FPProcessNaNs(a, b);
  if (std::isnan(result)) return result;

  if ((a == 0.0) && (b == 0.0) &&
      (copysign(1.0, a) != copysign(1.0, b))) {
    // a and b are zero, and the sign differs: return -0.0.
    return -0.0;
  } else {
    return (a < b) ? a : b;
  }
}


template <typename T>
T Simulator::FPMinNM(T a, T b) {
  if (IsQuietNaN(a) && !IsQuietNaN(b)) {
    a = kFP64PositiveInfinity;
  } else if (!IsQuietNaN(a) && IsQuietNaN(b)) {
    b = kFP64PositiveInfinity;
  }

  T result = FPProcessNaNs(a, b);
  return std::isnan(result) ? result : FPMin(a, b);
}


template <typename T>
T Simulator::FPRecipStepFused(T op1, T op2) {
  const T two = 2.0;
  if ((std::isinf(op1) && (op2 == 0.0))
      || ((op1 == 0.0) && (std::isinf(op2)))) {
    return two;
  } else if (std::isinf(op1) || std::isinf(op2)) {
    // Return +inf if signs match, otherwise -inf.
    return ((op1 >= 0.0) == (op2 >= 0.0)) ? kFP64PositiveInfinity
                                          : kFP64NegativeInfinity;
  } else {
    return FusedMultiplyAdd(op1, op2, two);
  }
}


template <typename T>
T Simulator::FPRSqrtStepFused(T op1, T op2) {
  const T one_point_five = 1.5;
  const T two = 2.0;

  if ((std::isinf(op1) && (op2 == 0.0))
      || ((op1 == 0.0) && (std::isinf(op2)))) {
    return one_point_five;
  } else if (std::isinf(op1) || std::isinf(op2)) {
    // Return +inf if signs match, otherwise -inf.
    return ((op1 >= 0.0) == (op2 >= 0.0)) ? kFP64PositiveInfinity
                                          : kFP64NegativeInfinity;
  } else {
    // The multiply-add-halve operation must be fully fused, so avoid interim
    // rounding by checking which operand can be losslessly divided by two
    // before doing the multiply-add.
    if (std::isnormal(op1 / two)) {
      return FusedMultiplyAdd(op1 / two, op2, one_point_five);
    } else if (std::isnormal(op2 / two)) {
      return FusedMultiplyAdd(op1, op2 / two, one_point_five);
    } else {
      // Neither operand is normal after halving: the result is dominated by
      // the addition term, so just return that.
      return one_point_five;
    }
  }
}


double Simulator::FPRoundInt(double value, FPRounding round_mode) {
  if ((value == 0.0) || (value == kFP64PositiveInfinity) ||
      (value == kFP64NegativeInfinity)) {
    return value;
  } else if (std::isnan(value)) {
    return FPProcessNaN(value);
  }

  double int_result = std::floor(value);
  double error = value - int_result;
  switch (round_mode) {
    case FPTieAway: {
      // Take care of correctly handling the range ]-0.5, -0.0], which must
      // yield -0.0.
      if ((-0.5 < value) && (value < 0.0)) {
        int_result = -0.0;

      } else if ((error > 0.5) || ((error == 0.5) && (int_result >= 0.0))) {
        // If the error is greater than 0.5, or is equal to 0.5 and the integer
        // result is positive, round up.
        int_result++;
      }
      break;
    }
    case FPTieEven: {
      // Take care of correctly handling the range [-0.5, -0.0], which must
      // yield -0.0.
      if ((-0.5 <= value) && (value < 0.0)) {
        int_result = -0.0;

      // If the error is greater than 0.5, or is equal to 0.5 and the integer
      // result is odd, round up.
      } else if ((error > 0.5) ||
          ((error == 0.5) && (std::fmod(int_result, 2) != 0))) {
        int_result++;
      }
      break;
    }
    case FPZero: {
      // If value>0 then we take floor(value)
      // otherwise, ceil(value).
      if (value < 0) {
         int_result = ceil(value);
      }
      break;
    }
    case FPNegativeInfinity: {
      // We always use floor(value).
      break;
    }
    case FPPositiveInfinity: {
      // Take care of correctly handling the range ]-1.0, -0.0], which must
      // yield -0.0.
      if ((-1.0 < value) && (value < 0.0)) {
        int_result = -0.0;

      // If the error is non-zero, round up.
      } else if (error > 0.0) {
        int_result++;
      }
      break;
    }
    default: VIXL_UNIMPLEMENTED();
  }
  return int_result;
}


int32_t Simulator::FPToInt32(double value, FPRounding rmode) {
  value = FPRoundInt(value, rmode);
  if (value >= kWMaxInt) {
    return kWMaxInt;
  } else if (value < kWMinInt) {
    return kWMinInt;
  }
  return std::isnan(value) ? 0 : static_cast<int32_t>(value);
}


int64_t Simulator::FPToInt64(double value, FPRounding rmode) {
  value = FPRoundInt(value, rmode);
  if (value >= kXMaxInt) {
    return kXMaxInt;
  } else if (value < kXMinInt) {
    return kXMinInt;
  }
  return std::isnan(value) ? 0 : static_cast<int64_t>(value);
}


uint32_t Simulator::FPToUInt32(double value, FPRounding rmode) {
  value = FPRoundInt(value, rmode);
  if (value >= kWMaxUInt) {
    return kWMaxUInt;
  } else if (value < 0.0) {
    return 0;
  }
  return std::isnan(value) ? 0 : static_cast<uint32_t>(value);
}


uint64_t Simulator::FPToUInt64(double value, FPRounding rmode) {
  value = FPRoundInt(value, rmode);
  if (value >= kXMaxUInt) {
    return kXMaxUInt;
  } else if (value < 0.0) {
    return 0;
  }
  return std::isnan(value) ? 0 : static_cast<uint64_t>(value);
}


#define DEFINE_NEON_FP_VECTOR_OP(FN, OP, PROCNAN)                \
template <typename T>                                            \
LogicVRegister Simulator::FN(VectorFormat vform,                 \
                             LogicVRegister dst,                 \
                             const LogicVRegister& src1,         \
                             const LogicVRegister& src2) {       \
  dst.ClearForWrite(vform);                                      \
  for (int i = 0; i < LaneCountFromFormat(vform); i++) {         \
    T op1 = src1.Float<T>(i);                                    \
    T op2 = src2.Float<T>(i);                                    \
    T result;                                                    \
    if (PROCNAN) {                                               \
      result = FPProcessNaNs(op1, op2);                          \
      if (!std::isnan(result)) {                                      \
        result = OP(op1, op2);                                   \
      }                                                          \
    } else {                                                     \
      result = OP(op1, op2);                                     \
    }                                                            \
    dst.SetFloat(i, result);                                     \
  }                                                              \
  return dst;                                                    \
}                                                                \
                                                                 \
LogicVRegister Simulator::FN(VectorFormat vform,                 \
                             LogicVRegister dst,                 \
                             const LogicVRegister& src1,         \
                             const LogicVRegister& src2) {       \
  if (LaneSizeInBitsFromFormat(vform) == kSRegSize) {            \
    FN<float>(vform, dst, src1, src2);                           \
  } else {                                                       \
    VIXL_ASSERT(LaneSizeInBitsFromFormat(vform) == kDRegSize);   \
    FN<double>(vform, dst, src1, src2);                          \
  }                                                              \
  return dst;                                                    \
}
NEON_FP3SAME_LIST(DEFINE_NEON_FP_VECTOR_OP)
#undef DEFINE_NEON_FP_VECTOR_OP


LogicVRegister Simulator::fnmul(VectorFormat vform,
                                LogicVRegister dst,
                                const LogicVRegister& src1,
                                const LogicVRegister& src2) {
  SimVRegister temp;
  LogicVRegister product = fmul(vform, temp, src1, src2);
  return fneg(vform, dst, product);
}


template <typename T>
LogicVRegister Simulator::frecps(VectorFormat vform,
                                 LogicVRegister dst,
                                 const LogicVRegister& src1,
                                 const LogicVRegister& src2) {
  dst.ClearForWrite(vform);
  for (int i = 0; i < LaneCountFromFormat(vform); i++) {
    T op1 = -src1.Float<T>(i);
    T op2 = src2.Float<T>(i);
    T result = FPProcessNaNs(op1, op2);
    dst.SetFloat(i, std::isnan(result) ? result : FPRecipStepFused(op1, op2));
  }
  return dst;
}


LogicVRegister Simulator::frecps(VectorFormat vform,
                                 LogicVRegister dst,
                                 const LogicVRegister& src1,
                                 const LogicVRegister& src2) {
  if (LaneSizeInBitsFromFormat(vform) == kSRegSize) {
    frecps<float>(vform, dst, src1, src2);
  } else {
    VIXL_ASSERT(LaneSizeInBitsFromFormat(vform) == kDRegSize);
    frecps<double>(vform, dst, src1, src2);
  }
  return dst;
}


template <typename T>
LogicVRegister Simulator::frsqrts(VectorFormat vform,
                                  LogicVRegister dst,
                                  const LogicVRegister& src1,
                                  const LogicVRegister& src2) {
  dst.ClearForWrite(vform);
  for (int i = 0; i < LaneCountFromFormat(vform); i++) {
    T op1 = -src1.Float<T>(i);
    T op2 = src2.Float<T>(i);
    T result = FPProcessNaNs(op1, op2);
    dst.SetFloat(i, std::isnan(result) ? result : FPRSqrtStepFused(op1, op2));
  }
  return dst;
}


LogicVRegister Simulator::frsqrts(VectorFormat vform,
                                  LogicVRegister dst,
                                  const LogicVRegister& src1,
                                  const LogicVRegister& src2) {
  if (LaneSizeInBitsFromFormat(vform) == kSRegSize) {
    frsqrts<float>(vform, dst, src1, src2);
  } else {
    VIXL_ASSERT(LaneSizeInBitsFromFormat(vform) == kDRegSize);
    frsqrts<double>(vform, dst, src1, src2);
  }
  return dst;
}


template <typename T>
LogicVRegister Simulator::fcmp(VectorFormat vform,
                               LogicVRegister dst,
                               const LogicVRegister& src1,
                               const LogicVRegister& src2,
                               Condition cond) {
  dst.ClearForWrite(vform);
  for (int i = 0; i < LaneCountFromFormat(vform); i++) {
    bool result = false;
    T op1 = src1.Float<T>(i);
    T op2 = src2.Float<T>(i);
    T nan_result = FPProcessNaNs(op1, op2);
    if (!std::isnan(nan_result)) {
      switch (cond) {
        case eq: result = (op1 == op2); break;
        case ge: result = (op1 >= op2); break;
        case gt: result = (op1 > op2) ; break;
        case le: result = (op1 <= op2); break;
        case lt: result = (op1 < op2) ; break;
        default: VIXL_UNREACHABLE(); break;
      }
    }
    dst.SetUint(vform, i, result ? MaxUintFromFormat(vform) : 0);
  }
  return dst;
}


LogicVRegister Simulator::fcmp(VectorFormat vform,
                               LogicVRegister dst,
                               const LogicVRegister& src1,
                               const LogicVRegister& src2,
                               Condition cond) {
  if (LaneSizeInBitsFromFormat(vform) == kSRegSize) {
    fcmp<float>(vform, dst, src1, src2, cond);
  } else {
    VIXL_ASSERT(LaneSizeInBitsFromFormat(vform) == kDRegSize);
    fcmp<double>(vform, dst, src1, src2, cond);
  }
  return dst;
}


LogicVRegister Simulator::fcmp_zero(VectorFormat vform,
                                    LogicVRegister dst,
                                    const LogicVRegister& src,
                                    Condition cond) {
  SimVRegister temp;
  if (LaneSizeInBitsFromFormat(vform) == kSRegSize) {
    LogicVRegister zero_reg = dup_immediate(vform, temp, float_to_rawbits(0.0));
    fcmp<float>(vform, dst, src, zero_reg, cond);
  } else {
    VIXL_ASSERT(LaneSizeInBitsFromFormat(vform) == kDRegSize);
    LogicVRegister zero_reg = dup_immediate(vform, temp,
                                            double_to_rawbits(0.0));
    fcmp<double>(vform, dst, src, zero_reg, cond);
  }
  return dst;
}


LogicVRegister Simulator::fabscmp(VectorFormat vform,
                                  LogicVRegister dst,
                                  const LogicVRegister& src1,
                                  const LogicVRegister& src2,
                                  Condition cond) {
  SimVRegister temp1, temp2;
  if (LaneSizeInBitsFromFormat(vform) == kSRegSize) {
    LogicVRegister abs_src1 = fabs_<float>(vform, temp1, src1);
    LogicVRegister abs_src2 = fabs_<float>(vform, temp2, src2);
    fcmp<float>(vform, dst, abs_src1, abs_src2, cond);
  } else {
    VIXL_ASSERT(LaneSizeInBitsFromFormat(vform) == kDRegSize);
    LogicVRegister abs_src1 = fabs_<double>(vform, temp1, src1);
    LogicVRegister abs_src2 = fabs_<double>(vform, temp2, src2);
    fcmp<double>(vform, dst, abs_src1, abs_src2, cond);
  }
  return dst;
}


template <typename T>
LogicVRegister Simulator::fmla(VectorFormat vform,
                               LogicVRegister dst,
                               const LogicVRegister& src1,
                               const LogicVRegister& src2) {
  dst.ClearForWrite(vform);
  for (int i = 0; i < LaneCountFromFormat(vform); i++) {
    T op1 = src1.Float<T>(i);
    T op2 = src2.Float<T>(i);
    T acc = dst.Float<T>(i);
    T result = FPMulAdd(acc, op1, op2);
    dst.SetFloat(i, result);
  }
  return dst;
}


LogicVRegister Simulator::fmla(VectorFormat vform,
                               LogicVRegister dst,
                               const LogicVRegister& src1,
                               const LogicVRegister& src2) {
  if (LaneSizeInBitsFromFormat(vform) == kSRegSize) {
    fmla<float>(vform, dst, src1, src2);
  } else {
    VIXL_ASSERT(LaneSizeInBitsFromFormat(vform) == kDRegSize);
    fmla<double>(vform, dst, src1, src2);
  }
  return dst;
}


template <typename T>
LogicVRegister Simulator::fmls(VectorFormat vform,
                               LogicVRegister dst,
                               const LogicVRegister& src1,
                               const LogicVRegister& src2) {
  dst.ClearForWrite(vform);
  for (int i = 0; i < LaneCountFromFormat(vform); i++) {
    T op1 = -src1.Float<T>(i);
    T op2 = src2.Float<T>(i);
    T acc = dst.Float<T>(i);
    T result = FPMulAdd(acc, op1, op2);
    dst.SetFloat(i, result);
  }
  return dst;
}


LogicVRegister Simulator::fmls(VectorFormat vform,
                               LogicVRegister dst,
                               const LogicVRegister& src1,
                               const LogicVRegister& src2) {
  if (LaneSizeInBitsFromFormat(vform) == kSRegSize) {
    fmls<float>(vform, dst, src1, src2);
  } else {
    VIXL_ASSERT(LaneSizeInBitsFromFormat(vform) == kDRegSize);
    fmls<double>(vform, dst, src1, src2);
  }
  return dst;
}


template <typename T>
LogicVRegister Simulator::fneg(VectorFormat vform,
                               LogicVRegister dst,
                               const LogicVRegister& src) {
  dst.ClearForWrite(vform);
  for (int i = 0; i < LaneCountFromFormat(vform); i++) {
    T op = src.Float<T>(i);
    op = -op;
    dst.SetFloat(i, op);
  }
  return dst;
}


LogicVRegister Simulator::fneg(VectorFormat vform,
                               LogicVRegister dst,
                               const LogicVRegister& src) {
  if (LaneSizeInBitsFromFormat(vform) == kSRegSize) {
    fneg<float>(vform, dst, src);
  } else {
    VIXL_ASSERT(LaneSizeInBitsFromFormat(vform) == kDRegSize);
    fneg<double>(vform, dst, src);
  }
  return dst;
}


template <typename T>
LogicVRegister Simulator::fabs_(VectorFormat vform,
                                LogicVRegister dst,
                                const LogicVRegister& src) {
  dst.ClearForWrite(vform);
  for (int i = 0; i < LaneCountFromFormat(vform); i++) {
    T op = src.Float<T>(i);
    if (copysign(1.0, op) < 0.0) {
      op = -op;
    }
    dst.SetFloat(i, op);
  }
  return dst;
}


LogicVRegister Simulator::fabs_(VectorFormat vform,
                                LogicVRegister dst,
                                const LogicVRegister& src) {
  if (LaneSizeInBitsFromFormat(vform) == kSRegSize) {
    fabs_<float>(vform, dst, src);
  } else {
    VIXL_ASSERT(LaneSizeInBitsFromFormat(vform) == kDRegSize);
    fabs_<double>(vform, dst, src);
  }
  return dst;
}


LogicVRegister Simulator::fabd(VectorFormat vform,
                               LogicVRegister dst,
                               const LogicVRegister& src1,
                               const LogicVRegister& src2) {
  SimVRegister temp;
  fsub(vform, temp, src1, src2);
  fabs_(vform, dst, temp);
  return dst;
}


LogicVRegister Simulator::fsqrt(VectorFormat vform,
                                LogicVRegister dst,
                                const LogicVRegister& src) {
  dst.ClearForWrite(vform);
  if (LaneSizeInBitsFromFormat(vform) == kSRegSize) {
    for (int i = 0; i < LaneCountFromFormat(vform); i++) {
      float result = FPSqrt(src.Float<float>(i));
      dst.SetFloat(i, result);
    }
  } else {
    VIXL_ASSERT(LaneSizeInBitsFromFormat(vform) == kDRegSize);
    for (int i = 0; i < LaneCountFromFormat(vform); i++) {
      double result = FPSqrt(src.Float<double>(i));
      dst.SetFloat(i, result);
    }
  }
  return dst;
}


#define DEFINE_NEON_FP_PAIR_OP(FNP, FN, OP)                          \
LogicVRegister Simulator::FNP(VectorFormat vform,                    \
                              LogicVRegister dst,                    \
                              const LogicVRegister& src1,            \
                              const LogicVRegister& src2) {          \
  SimVRegister temp1, temp2;                                         \
  uzp1(vform, temp1, src1, src2);                                    \
  uzp2(vform, temp2, src1, src2);                                    \
  FN(vform, dst, temp1, temp2);                                      \
  return dst;                                                        \
}                                                                    \
                                                                     \
LogicVRegister Simulator::FNP(VectorFormat vform,                    \
                              LogicVRegister dst,                    \
                              const LogicVRegister& src) {           \
  if (vform == kFormatS) {                                           \
    float result = OP(src.Float<float>(0), src.Float<float>(1));     \
    dst.SetFloat(0, result);                                         \
  } else {                                                           \
    VIXL_ASSERT(vform == kFormatD);                                  \
    double result = OP(src.Float<double>(0), src.Float<double>(1));  \
    dst.SetFloat(0, result);                                         \
  }                                                                  \
  dst.ClearForWrite(vform);                                          \
  return dst;                                                        \
}
NEON_FPPAIRWISE_LIST(DEFINE_NEON_FP_PAIR_OP)
#undef DEFINE_NEON_FP_PAIR_OP


LogicVRegister Simulator::fminmaxv(VectorFormat vform,
                                   LogicVRegister dst,
                                   const LogicVRegister& src,
                                   FPMinMaxOp Op) {
  VIXL_ASSERT(vform == kFormat4S);
  USE(vform);
  float result1 = (this->*Op)(src.Float<float>(0), src.Float<float>(1));
  float result2 = (this->*Op)(src.Float<float>(2), src.Float<float>(3));
  float result = (this->*Op)(result1, result2);
  dst.ClearForWrite(kFormatS);
  dst.SetFloat<float>(0, result);
  return dst;
}


LogicVRegister Simulator::fmaxv(VectorFormat vform,
                                LogicVRegister dst,
                                const LogicVRegister& src) {
  return fminmaxv(vform, dst, src, &Simulator::FPMax);
}


LogicVRegister Simulator::fminv(VectorFormat vform,
                                LogicVRegister dst,
                                const LogicVRegister& src) {
  return fminmaxv(vform, dst, src, &Simulator::FPMin);
}


LogicVRegister Simulator::fmaxnmv(VectorFormat vform,
                                 LogicVRegister dst,
                                 const LogicVRegister& src) {
  return fminmaxv(vform, dst, src, &Simulator::FPMaxNM);
}


LogicVRegister Simulator::fminnmv(VectorFormat vform,
                                  LogicVRegister dst,
                                  const LogicVRegister& src) {
  return fminmaxv(vform, dst, src, &Simulator::FPMinNM);
}


LogicVRegister Simulator::fmul(VectorFormat vform,
                               LogicVRegister dst,
                               const LogicVRegister& src1,
                               const LogicVRegister& src2,
                               int index) {
  dst.ClearForWrite(vform);
  SimVRegister temp;
  if (LaneSizeInBitsFromFormat(vform) == kSRegSize) {
    LogicVRegister index_reg = dup_element(kFormat4S, temp, src2, index);
    fmul<float>(vform, dst, src1, index_reg);

  } else {
    VIXL_ASSERT(LaneSizeInBitsFromFormat(vform) == kDRegSize);
    LogicVRegister index_reg = dup_element(kFormat2D, temp, src2, index);
    fmul<double>(vform, dst, src1, index_reg);
  }
  return dst;
}


LogicVRegister Simulator::fmla(VectorFormat vform,
                               LogicVRegister dst,
                               const LogicVRegister& src1,
                               const LogicVRegister& src2,
                               int index) {
  dst.ClearForWrite(vform);
  SimVRegister temp;
  if (LaneSizeInBitsFromFormat(vform) == kSRegSize) {
    LogicVRegister index_reg = dup_element(kFormat4S, temp, src2, index);
    fmla<float>(vform, dst, src1, index_reg);

  } else {
    VIXL_ASSERT(LaneSizeInBitsFromFormat(vform) == kDRegSize);
    LogicVRegister index_reg = dup_element(kFormat2D, temp, src2, index);
    fmla<double>(vform, dst, src1, index_reg);
  }
  return dst;
}


LogicVRegister Simulator::fmls(VectorFormat vform,
                               LogicVRegister dst,
                               const LogicVRegister& src1,
                               const LogicVRegister& src2,
                               int index) {
  dst.ClearForWrite(vform);
  SimVRegister temp;
  if (LaneSizeInBitsFromFormat(vform) == kSRegSize) {
    LogicVRegister index_reg = dup_element(kFormat4S, temp, src2, index);
    fmls<float>(vform, dst, src1, index_reg);

  } else {
    VIXL_ASSERT(LaneSizeInBitsFromFormat(vform) == kDRegSize);
    LogicVRegister index_reg = dup_element(kFormat2D, temp, src2, index);
    fmls<double>(vform, dst, src1, index_reg);
  }
  return dst;
}


LogicVRegister Simulator::fmulx(VectorFormat vform,
                                LogicVRegister dst,
                                const LogicVRegister& src1,
                                const LogicVRegister& src2,
                                int index) {
  dst.ClearForWrite(vform);
  SimVRegister temp;
  if (LaneSizeInBitsFromFormat(vform) == kSRegSize) {
    LogicVRegister index_reg = dup_element(kFormat4S, temp, src2, index);
    fmulx<float>(vform, dst, src1, index_reg);

  } else {
    VIXL_ASSERT(LaneSizeInBitsFromFormat(vform) == kDRegSize);
    LogicVRegister index_reg = dup_element(kFormat2D, temp, src2, index);
    fmulx<double>(vform, dst, src1, index_reg);
  }
  return dst;
}


LogicVRegister Simulator::frint(VectorFormat vform,
                                LogicVRegister dst,
                                const LogicVRegister& src,
                                FPRounding rounding_mode,
                                bool inexact_exception) {
  dst.ClearForWrite(vform);
  if (LaneSizeInBitsFromFormat(vform) == kSRegSize) {
    for (int i = 0; i < LaneCountFromFormat(vform); i++) {
      float input = src.Float<float>(i);
      float rounded = FPRoundInt(input, rounding_mode);
      if (inexact_exception && !std::isnan(input) && (input != rounded)) {
        FPProcessException();
      }
      dst.SetFloat<float>(i, rounded);
    }
  } else {
    VIXL_ASSERT(LaneSizeInBitsFromFormat(vform) == kDRegSize);
    for (int i = 0; i < LaneCountFromFormat(vform); i++) {
      double input = src.Float<double>(i);
      double rounded = FPRoundInt(input, rounding_mode);
      if (inexact_exception && !std::isnan(input) && (input != rounded)) {
        FPProcessException();
      }
      dst.SetFloat<double>(i, rounded);
    }
  }
  return dst;
}


LogicVRegister Simulator::fcvts(VectorFormat vform,
                                LogicVRegister dst,
                                const LogicVRegister& src,
                                FPRounding rounding_mode,
                                int fbits) {
  dst.ClearForWrite(vform);
  if (LaneSizeInBitsFromFormat(vform) == kSRegSize) {
    for (int i = 0; i < LaneCountFromFormat(vform); i++) {
      float op = src.Float<float>(i) * std::pow(2.0f, fbits);
      dst.SetInt(vform, i, FPToInt32(op, rounding_mode));
    }
  } else {
    VIXL_ASSERT(LaneSizeInBitsFromFormat(vform) == kDRegSize);
    for (int i = 0; i < LaneCountFromFormat(vform); i++) {
      double op = src.Float<double>(i) * std::pow(2.0, fbits);
      dst.SetInt(vform, i, FPToInt64(op, rounding_mode));
    }
  }
  return dst;
}


LogicVRegister Simulator::fcvtu(VectorFormat vform,
                                LogicVRegister dst,
                                const LogicVRegister& src,
                                FPRounding rounding_mode,
                                int fbits) {
  dst.ClearForWrite(vform);
  if (LaneSizeInBitsFromFormat(vform) == kSRegSize) {
    for (int i = 0; i < LaneCountFromFormat(vform); i++) {
      float op = src.Float<float>(i) * std::pow(2.0f, fbits);
      dst.SetUint(vform, i, FPToUInt32(op, rounding_mode));
    }
  } else {
    VIXL_ASSERT(LaneSizeInBitsFromFormat(vform) == kDRegSize);
    for (int i = 0; i < LaneCountFromFormat(vform); i++) {
      double op = src.Float<double>(i) * std::pow(2.0, fbits);
      dst.SetUint(vform, i, FPToUInt64(op, rounding_mode));
    }
  }
  return dst;
}


LogicVRegister Simulator::fcvtl(VectorFormat vform,
                                LogicVRegister dst,
                                const LogicVRegister& src) {
  if (LaneSizeInBitsFromFormat(vform) == kSRegSize) {
    for (int i = LaneCountFromFormat(vform) - 1; i >= 0; i--) {
      dst.SetFloat(i, FPToFloat(src.Float<float16>(i)));
    }
  } else {
    VIXL_ASSERT(LaneSizeInBitsFromFormat(vform) == kDRegSize);
    for (int i = LaneCountFromFormat(vform) - 1; i >= 0; i--) {
      dst.SetFloat(i, FPToDouble(src.Float<float>(i)));
    }
  }
  return dst;
}


LogicVRegister Simulator::fcvtl2(VectorFormat vform,
                                 LogicVRegister dst,
                                 const LogicVRegister& src) {
  int lane_count = LaneCountFromFormat(vform);
  if (LaneSizeInBitsFromFormat(vform) == kSRegSize) {
    for (int i = 0; i < lane_count; i++) {
      dst.SetFloat(i, FPToFloat(src.Float<float16>(i + lane_count)));
    }
  } else {
    VIXL_ASSERT(LaneSizeInBitsFromFormat(vform) == kDRegSize);
    for (int i = 0; i < lane_count; i++) {
      dst.SetFloat(i, FPToDouble(src.Float<float>(i + lane_count)));
    }
  }
  return dst;
}


LogicVRegister Simulator::fcvtn(VectorFormat vform,
                                LogicVRegister dst,
                                const LogicVRegister& src) {
  if (LaneSizeInBitsFromFormat(vform) == kHRegSize) {
    for (int i = 0; i < LaneCountFromFormat(vform); i++) {
      dst.SetFloat(i, FPToFloat16(src.Float<float>(i), FPTieEven));
    }
  } else {
    VIXL_ASSERT(LaneSizeInBitsFromFormat(vform) == kSRegSize);
    for (int i = 0; i < LaneCountFromFormat(vform); i++) {
      dst.SetFloat(i, FPToFloat(src.Float<double>(i), FPTieEven));
    }
  }
  return dst;
}


LogicVRegister Simulator::fcvtn2(VectorFormat vform,
                                 LogicVRegister dst,
                                 const LogicVRegister& src) {
  int lane_count = LaneCountFromFormat(vform) / 2;
  if (LaneSizeInBitsFromFormat(vform) == kHRegSize) {
    for (int i = lane_count - 1; i >= 0; i--) {
      dst.SetFloat(i + lane_count, FPToFloat16(src.Float<float>(i), FPTieEven));
    }
  } else {
    VIXL_ASSERT(LaneSizeInBitsFromFormat(vform) == kSRegSize);
    for (int i = lane_count - 1; i >= 0; i--) {
      dst.SetFloat(i + lane_count, FPToFloat(src.Float<double>(i), FPTieEven));
    }
  }
  return dst;
}


LogicVRegister Simulator::fcvtxn(VectorFormat vform,
                                 LogicVRegister dst,
                                 const LogicVRegister& src) {
  dst.ClearForWrite(vform);
  VIXL_ASSERT(LaneSizeInBitsFromFormat(vform) == kSRegSize);
  for (int i = 0; i < LaneCountFromFormat(vform); i++) {
    dst.SetFloat(i, FPToFloat(src.Float<double>(i), FPRoundOdd));
  }
  return dst;
}


LogicVRegister Simulator::fcvtxn2(VectorFormat vform,
                                  LogicVRegister dst,
                                  const LogicVRegister& src) {
  VIXL_ASSERT(LaneSizeInBitsFromFormat(vform) == kSRegSize);
  int lane_count = LaneCountFromFormat(vform) / 2;
  for (int i = lane_count - 1; i >= 0; i--) {
    dst.SetFloat(i + lane_count, FPToFloat(src.Float<double>(i), FPRoundOdd));
  }
  return dst;
}


// Based on reference C function recip_sqrt_estimate from ARM ARM.
double Simulator::recip_sqrt_estimate(double a) {
  int q0, q1, s;
  double r;
  if (a < 0.5) {
    q0 = static_cast<int>(a * 512.0);
    r = 1.0 / sqrt((static_cast<double>(q0) + 0.5) / 512.0);
  } else  {
    q1 = static_cast<int>(a * 256.0);
    r = 1.0 / sqrt((static_cast<double>(q1) + 0.5) / 256.0);
  }
  s = static_cast<int>(256.0 * r + 0.5);
  return static_cast<double>(s) / 256.0;
}


static inline uint64_t Bits(uint64_t val, int start_bit, int end_bit) {
  return unsigned_bitextract_64(start_bit, end_bit, val);
}


template <typename T>
T Simulator::FPRecipSqrtEstimate(T op) {
  if (std::isnan(op)) {
    return FPProcessNaN(op);
  } else if (op == 0.0) {
    if (copysign(1.0, op) < 0.0) {
      return kFP64NegativeInfinity;
    } else {
      return kFP64PositiveInfinity;
    }
  } else if (copysign(1.0, op) < 0.0) {
    FPProcessException();
    return FPDefaultNaN<T>();
  } else if (std::isinf(op)) {
    return 0.0;
  } else {
    uint64_t fraction;
    int exp, result_exp;

    if (sizeof(T) == sizeof(float)) {  // NOLINT(runtime/sizeof)
      exp = float_exp(op);
      fraction = float_mantissa(op);
      fraction <<= 29;
    } else {
      exp = double_exp(op);
      fraction = double_mantissa(op);
    }

    if (exp == 0) {
      while (Bits(fraction, 51, 51) == 0) {
        fraction = Bits(fraction, 50, 0) << 1;
        exp -= 1;
      }
      fraction = Bits(fraction, 50, 0) << 1;
    }

    double scaled;
    if (Bits(exp, 0, 0) == 0) {
      scaled = double_pack(0, 1022, Bits(fraction, 51, 44) << 44);
    } else {
      scaled = double_pack(0, 1021, Bits(fraction, 51, 44) << 44);
    }

    if (sizeof(T) == sizeof(float)) {  // NOLINT(runtime/sizeof)
      result_exp = (380 - exp) / 2;
    } else {
      result_exp = (3068 - exp) / 2;
    }

    uint64_t estimate = double_to_rawbits(recip_sqrt_estimate(scaled));

    if (sizeof(T) == sizeof(float)) {  // NOLINT(runtime/sizeof)
      uint32_t exp_bits = static_cast<uint32_t>(Bits(result_exp, 7, 0));
      uint32_t est_bits = static_cast<uint32_t>(Bits(estimate, 51, 29));
      return float_pack(0, exp_bits, est_bits);
    } else {
      return double_pack(0, Bits(result_exp, 10, 0), Bits(estimate, 51, 0));
    }
  }
}


LogicVRegister Simulator::frsqrte(VectorFormat vform,
                                  LogicVRegister dst,
                                  const LogicVRegister& src) {
  dst.ClearForWrite(vform);
  if (LaneSizeInBitsFromFormat(vform) == kSRegSize) {
    for (int i = 0; i < LaneCountFromFormat(vform); i++) {
      float input = src.Float<float>(i);
      dst.SetFloat(i, FPRecipSqrtEstimate<float>(input));
    }
  } else {
    VIXL_ASSERT(LaneSizeInBitsFromFormat(vform) == kDRegSize);
    for (int i = 0; i < LaneCountFromFormat(vform); i++) {
      double input = src.Float<double>(i);
      dst.SetFloat(i, FPRecipSqrtEstimate<double>(input));
    }
  }
  return dst;
}

template <typename T>
T Simulator::FPRecipEstimate(T op, FPRounding rounding) {
  uint32_t sign;

  if (sizeof(T) == sizeof(float)) {  // NOLINT(runtime/sizeof)
    sign = float_sign(op);
  } else {
    sign = double_sign(op);
  }

  if (std::isnan(op)) {
    return FPProcessNaN(op);
  } else if (std::isinf(op)) {
    return (sign == 1) ? -0.0 : 0.0;
  } else if (op == 0.0) {
    FPProcessException();  // FPExc_DivideByZero exception.
    return (sign == 1) ? kFP64NegativeInfinity : kFP64PositiveInfinity;
  } else if (((sizeof(T) == sizeof(float)) &&  // NOLINT(runtime/sizeof)
              (std::fabs(op) < std::pow(2.0, -128.0))) ||
             ((sizeof(T) == sizeof(double)) &&  // NOLINT(runtime/sizeof)
              (std::fabs(op) < std::pow(2.0, -1024.0)))) {
    bool overflow_to_inf = false;
    switch (rounding) {
      case FPTieEven: overflow_to_inf = true; break;
      case FPPositiveInfinity: overflow_to_inf = (sign == 0); break;
      case FPNegativeInfinity: overflow_to_inf = (sign == 1); break;
      case FPZero: overflow_to_inf = false; break;
      default: break;
    }
    FPProcessException();  // FPExc_Overflow and FPExc_Inexact.
    if (overflow_to_inf) {
      return (sign == 1) ? kFP64NegativeInfinity : kFP64PositiveInfinity;
    } else {
      // Return FPMaxNormal(sign).
      if (sizeof(T) == sizeof(float)) {  // NOLINT(runtime/sizeof)
        return float_pack(sign, 0xfe, 0x07fffff);
      } else {
        return double_pack(sign, 0x7fe, 0x0fffffffffffffl);
      }
    }
  } else {
    uint64_t fraction;
    int exp, result_exp;
    uint32_t sign;

    if (sizeof(T) == sizeof(float)) {  // NOLINT(runtime/sizeof)
      sign = float_sign(op);
      exp = float_exp(op);
      fraction = float_mantissa(op);
      fraction <<= 29;
    } else {
      sign = double_sign(op);
      exp = double_exp(op);
      fraction = double_mantissa(op);
    }

    if (exp == 0) {
      if (Bits(fraction, 51, 51) == 0) {
        exp -= 1;
        fraction = Bits(fraction, 49, 0) << 2;
      } else {
        fraction = Bits(fraction, 50, 0) << 1;
      }
    }

    double scaled = double_pack(0, 1022, Bits(fraction, 51, 44) << 44);

    if (sizeof(T) == sizeof(float)) {  // NOLINT(runtime/sizeof)
      result_exp = (253 - exp);  // In range 253-254 = -1 to 253+1 = 254.
    } else {
      result_exp = (2045 - exp);  // In range 2045-2046 = -1 to 2045+1 = 2046.
    }

    double estimate = recip_estimate(scaled);

    fraction = double_mantissa(estimate);
    if (result_exp == 0) {
      fraction = (UINT64_C(1) << 51) | Bits(fraction, 51, 1);
    } else if (result_exp == -1) {
      fraction = (UINT64_C(1) << 50) | Bits(fraction, 51, 2);
      result_exp = 0;
    }
    if (sizeof(T) == sizeof(float)) {  // NOLINT(runtime/sizeof)
      uint32_t exp_bits = static_cast<uint32_t>(Bits(result_exp, 7, 0));
      uint32_t frac_bits = static_cast<uint32_t>(Bits(fraction, 51, 29));
      return float_pack(sign, exp_bits, frac_bits);
    } else {
      return double_pack(sign, Bits(result_exp, 10, 0), Bits(fraction, 51, 0));
    }
  }
}


LogicVRegister Simulator::frecpe(VectorFormat vform,
                                 LogicVRegister dst,
                                 const LogicVRegister& src,
                                 FPRounding round) {
  dst.ClearForWrite(vform);
  if (LaneSizeInBitsFromFormat(vform) == kSRegSize) {
    for (int i = 0; i < LaneCountFromFormat(vform); i++) {
      float input = src.Float<float>(i);
      dst.SetFloat(i, FPRecipEstimate<float>(input, round));
    }
  } else {
    VIXL_ASSERT(LaneSizeInBitsFromFormat(vform) == kDRegSize);
    for (int i = 0; i < LaneCountFromFormat(vform); i++) {
      double input = src.Float<double>(i);
      dst.SetFloat(i, FPRecipEstimate<double>(input, round));
    }
  }
  return dst;
}


LogicVRegister Simulator::ursqrte(VectorFormat vform,
                                  LogicVRegister dst,
                                  const LogicVRegister& src) {
  dst.ClearForWrite(vform);
  uint64_t operand;
  uint32_t result;
  double dp_operand, dp_result;
  for (int i = 0; i < LaneCountFromFormat(vform); i++) {
    operand = src.Uint(vform, i);
    if (operand <= 0x3FFFFFFF) {
      result = 0xFFFFFFFF;
    } else {
      dp_operand = operand * std::pow(2.0, -32);
      dp_result = recip_sqrt_estimate(dp_operand) * std::pow(2.0, 31);
      result = static_cast<uint32_t>(dp_result);
    }
    dst.SetUint(vform, i, result);
  }
  return dst;
}


// Based on reference C function recip_estimate from ARM ARM.
double Simulator::recip_estimate(double a) {
  int q, s;
  double r;
  q = static_cast<int>(a * 512.0);
  r = 1.0 / ((static_cast<double>(q) + 0.5) / 512.0);
  s = static_cast<int>(256.0 * r + 0.5);
  return static_cast<double>(s) / 256.0;
}


LogicVRegister Simulator::urecpe(VectorFormat vform,
                                 LogicVRegister dst,
                                 const LogicVRegister& src) {
  dst.ClearForWrite(vform);
  uint64_t operand;
  uint32_t result;
  double dp_operand, dp_result;
  for (int i = 0; i < LaneCountFromFormat(vform); i++) {
    operand = src.Uint(vform, i);
    if (operand <= 0x7FFFFFFF) {
      result = 0xFFFFFFFF;
    } else {
      dp_operand = operand * std::pow(2.0, -32);
      dp_result = recip_estimate(dp_operand) * std::pow(2.0, 31);
      result = static_cast<uint32_t>(dp_result);
    }
    dst.SetUint(vform, i, result);
  }
  return dst;
}

template <typename T>
LogicVRegister Simulator::frecpx(VectorFormat vform,
                                 LogicVRegister dst,
                                 const LogicVRegister& src) {
  dst.ClearForWrite(vform);
  for (int i = 0; i < LaneCountFromFormat(vform); i++) {
    T op = src.Float<T>(i);
    T result;
    if (std::isnan(op)) {
       result = FPProcessNaN(op);
    } else {
      int exp;
      uint32_t sign;
      if (sizeof(T) == sizeof(float)) {  // NOLINT(runtime/sizeof)
        sign = float_sign(op);
        exp = float_exp(op);
        exp = (exp == 0) ? (0xFF - 1) : static_cast<int>(Bits(~exp, 7, 0));
        result = float_pack(sign, exp, 0);
      } else {
        sign = double_sign(op);
        exp = double_exp(op);
        exp = (exp == 0) ? (0x7FF - 1) : static_cast<int>(Bits(~exp, 10, 0));
        result = double_pack(sign, exp, 0);
      }
    }
    dst.SetFloat(i, result);
  }
  return dst;
}


LogicVRegister Simulator::frecpx(VectorFormat vform,
                                 LogicVRegister dst,
                                 const LogicVRegister& src) {
  if (LaneSizeInBitsFromFormat(vform) == kSRegSize) {
    frecpx<float>(vform, dst, src);
  } else {
    VIXL_ASSERT(LaneSizeInBitsFromFormat(vform) == kDRegSize);
    frecpx<double>(vform, dst, src);
  }
  return dst;
}

LogicVRegister Simulator::scvtf(VectorFormat vform,
                                LogicVRegister dst,
                                const LogicVRegister& src,
                                int fbits,
                                FPRounding round) {
  for (int i = 0; i < LaneCountFromFormat(vform); i++) {
    if (LaneSizeInBitsFromFormat(vform) == kSRegSize) {
      float result = FixedToFloat(src.Int(kFormatS, i), fbits, round);
      dst.SetFloat<float>(i, result);
    } else {
      VIXL_ASSERT(LaneSizeInBitsFromFormat(vform) == kDRegSize);
      double result = FixedToDouble(src.Int(kFormatD, i), fbits, round);
      dst.SetFloat<double>(i, result);
    }
  }
  return dst;
}


LogicVRegister Simulator::ucvtf(VectorFormat vform,
                                LogicVRegister dst,
                                const LogicVRegister& src,
                                int fbits,
                                FPRounding round) {
  for (int i = 0; i < LaneCountFromFormat(vform); i++) {
    if (LaneSizeInBitsFromFormat(vform) == kSRegSize) {
      float result = UFixedToFloat(src.Uint(kFormatS, i), fbits, round);
      dst.SetFloat<float>(i, result);
    } else {
      VIXL_ASSERT(LaneSizeInBitsFromFormat(vform) == kDRegSize);
      double result = UFixedToDouble(src.Uint(kFormatD, i), fbits, round);
      dst.SetFloat<double>(i, result);
    }
  }
  return dst;
}


}  // namespace vixl

#endif  // JS_SIMULATOR_ARM64

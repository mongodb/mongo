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

#include "jit/arm64/vixl/Utils-vixl.h"

#include "mozilla/MathAlgorithms.h"

#include <stdio.h>

namespace vixl {

uint32_t float_to_rawbits(float value) {
  uint32_t bits = 0;
  memcpy(&bits, &value, 4);
  return bits;
}


uint64_t double_to_rawbits(double value) {
  uint64_t bits = 0;
  memcpy(&bits, &value, 8);
  return bits;
}


float rawbits_to_float(uint32_t bits) {
  float value = 0.0;
  memcpy(&value, &bits, 4);
  return value;
}


double rawbits_to_double(uint64_t bits) {
  double value = 0.0;
  memcpy(&value, &bits, 8);
  return value;
}


uint32_t float_sign(float val) {
  uint32_t rawbits = float_to_rawbits(val);
  return unsigned_bitextract_32(31, 31, rawbits);
}


uint32_t float_exp(float val) {
  uint32_t rawbits = float_to_rawbits(val);
  return unsigned_bitextract_32(30, 23, rawbits);
}


uint32_t float_mantissa(float val) {
  uint32_t rawbits = float_to_rawbits(val);
  return unsigned_bitextract_32(22, 0, rawbits);
}


uint32_t double_sign(double val) {
  uint64_t rawbits = double_to_rawbits(val);
  return static_cast<uint32_t>(unsigned_bitextract_64(63, 63, rawbits));
}


uint32_t double_exp(double val) {
  uint64_t rawbits = double_to_rawbits(val);
  return static_cast<uint32_t>(unsigned_bitextract_64(62, 52, rawbits));
}


uint64_t double_mantissa(double val) {
  uint64_t rawbits = double_to_rawbits(val);
  return unsigned_bitextract_64(51, 0, rawbits);
}


float float_pack(uint32_t sign, uint32_t exp, uint32_t mantissa) {
  uint32_t bits = (sign << 31) | (exp << 23) | mantissa;
  return rawbits_to_float(bits);
}


double double_pack(uint64_t sign, uint64_t exp, uint64_t mantissa) {
  uint64_t bits = (sign << 63) | (exp << 52) | mantissa;
  return rawbits_to_double(bits);
}


int float16classify(float16 value) {
  uint16_t exponent_max = (1 << 5) - 1;
  uint16_t exponent_mask = exponent_max << 10;
  uint16_t mantissa_mask = (1 << 10) - 1;

  uint16_t exponent = (value & exponent_mask) >> 10;
  uint16_t mantissa = value & mantissa_mask;
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

}  // namespace vixl

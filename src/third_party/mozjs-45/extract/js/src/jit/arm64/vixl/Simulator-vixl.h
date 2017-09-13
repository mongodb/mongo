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

#ifndef VIXL_A64_SIMULATOR_A64_H_
#define VIXL_A64_SIMULATOR_A64_H_

#include "js-config.h"

#ifdef JS_SIMULATOR_ARM64

#include "mozilla/Vector.h"

#include "jsalloc.h"

#include "jit/arm64/vixl/Assembler-vixl.h"
#include "jit/arm64/vixl/Disasm-vixl.h"
#include "jit/arm64/vixl/Globals-vixl.h"
#include "jit/arm64/vixl/Instructions-vixl.h"
#include "jit/arm64/vixl/Instrument-vixl.h"
#include "jit/arm64/vixl/Utils-vixl.h"

#include "jit/IonTypes.h"
#include "vm/PosixNSPR.h"

#define JS_CHECK_SIMULATOR_RECURSION_WITH_EXTRA(cx, extra, onerror)             \
    JS_BEGIN_MACRO                                                              \
        if (cx->mainThread().simulator()->overRecursedWithExtra(extra)) {       \
            js::ReportOverRecursed(cx);                                         \
            onerror;                                                            \
        }                                                                       \
    JS_END_MACRO

namespace vixl {

// Debug instructions.
//
// VIXL's macro-assembler and simulator support a few pseudo instructions to
// make debugging easier. These pseudo instructions do not exist on real
// hardware.
//
// TODO: Provide controls to prevent the macro assembler from emitting
// pseudo-instructions. This is important for ahead-of-time compilers, where the
// macro assembler is built with USE_SIMULATOR but the code will eventually be
// run on real hardware.
//
// TODO: Also consider allowing these pseudo-instructions to be disabled in the
// simulator, so that users can check that the input is a valid native code.
// (This isn't possible in all cases. Printf won't work, for example.)
//
// Each debug pseudo instruction is represented by a HLT instruction. The HLT
// immediate field is used to identify the type of debug pseudo instruction.

enum DebugHltOpcodes {
  kUnreachableOpcode = 0xdeb0,
  kPrintfOpcode,
  kTraceOpcode,
  kLogOpcode,
  // Aliases.
  kDebugHltFirstOpcode = kUnreachableOpcode,
  kDebugHltLastOpcode = kLogOpcode
};

// Each pseudo instruction uses a custom encoding for additional arguments, as
// described below.

// Unreachable - kUnreachableOpcode
//
// Instruction which should never be executed. This is used as a guard in parts
// of the code that should not be reachable, such as in data encoded inline in
// the instructions.

// Printf - kPrintfOpcode
//  - arg_count: The number of arguments.
//  - arg_pattern: A set of PrintfArgPattern values, packed into two-bit fields.
//
// Simulate a call to printf.
//
// Floating-point and integer arguments are passed in separate sets of registers
// in AAPCS64 (even for varargs functions), so it is not possible to determine
// the type of each argument without some information about the values that were
// passed in. This information could be retrieved from the printf format string,
// but the format string is not trivial to parse so we encode the relevant
// information with the HLT instruction.
//
// Also, the following registers are populated (as if for a native A64 call):
//    x0: The format string
// x1-x7: Optional arguments, if type == CPURegister::kRegister
// d0-d7: Optional arguments, if type == CPURegister::kFPRegister
const unsigned kPrintfArgCountOffset = 1 * kInstructionSize;
const unsigned kPrintfArgPatternListOffset = 2 * kInstructionSize;
const unsigned kPrintfLength = 3 * kInstructionSize;

const unsigned kPrintfMaxArgCount = 4;

// The argument pattern is a set of two-bit-fields, each with one of the
// following values:
enum PrintfArgPattern {
  kPrintfArgW = 1,
  kPrintfArgX = 2,
  // There is no kPrintfArgS because floats are always converted to doubles in C
  // varargs calls.
  kPrintfArgD = 3
};
static const unsigned kPrintfArgPatternBits = 2;

// Trace - kTraceOpcode
//  - parameter: TraceParameter stored as a uint32_t
//  - command: TraceCommand stored as a uint32_t
//
// Allow for trace management in the generated code. This enables or disables
// automatic tracing of the specified information for every simulated
// instruction.
const unsigned kTraceParamsOffset = 1 * kInstructionSize;
const unsigned kTraceCommandOffset = 2 * kInstructionSize;
const unsigned kTraceLength = 3 * kInstructionSize;

// Trace parameters.
enum TraceParameters {
  LOG_DISASM     = 1 << 0,  // Log disassembly.
  LOG_REGS       = 1 << 1,  // Log general purpose registers.
  LOG_VREGS      = 1 << 2,  // Log NEON and floating-point registers.
  LOG_SYSREGS    = 1 << 3,  // Log the flags and system registers.
  LOG_WRITE      = 1 << 4,  // Log writes to memory.

  LOG_NONE       = 0,
  LOG_STATE      = LOG_REGS | LOG_VREGS | LOG_SYSREGS,
  LOG_ALL        = LOG_DISASM | LOG_STATE | LOG_WRITE
};

// Trace commands.
enum TraceCommand {
  TRACE_ENABLE   = 1,
  TRACE_DISABLE  = 2
};

// Log - kLogOpcode
//  - parameter: TraceParameter stored as a uint32_t
//
// Print the specified information once. This mechanism is separate from Trace.
// In particular, _all_ of the specified registers are printed, rather than just
// the registers that the instruction writes.
//
// Any combination of the TraceParameters values can be used, except that
// LOG_DISASM is not supported for Log.
const unsigned kLogParamsOffset = 1 * kInstructionSize;
const unsigned kLogLength = 2 * kInstructionSize;


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
T FPRound(int64_t sign, int64_t exponent, uint64_t mantissa,
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

  if (shift > 0) {
    if (round_mode == FPTieEven) {
      // We have to shift the mantissa to the right. Some precision is lost, so
      // we need to apply rounding.
      uint64_t onebit_mantissa = (mantissa >> (shift)) & 1;
      uint64_t halfbit_mantissa = (mantissa >> (shift-1)) & 1;
      uint64_t adjustment = (halfbit_mantissa & ~onebit_mantissa);
      uint64_t adjusted = mantissa - adjustment;
      T halfbit_adjusted = (adjusted >> (shift-1)) & 1;

      T result = static_cast<T>((sign << sign_offset) |
                                (exponent << exponent_offset) |
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


// Representation of memory, with typed getters and setters for access.
class Memory {
 public:
  template <typename T>
  static T AddressUntag(T address) {
    // Cast the address using a C-style cast. A reinterpret_cast would be
    // appropriate, but it can't cast one integral type to another.
    uint64_t bits = (uint64_t)address;
    return (T)(bits & ~kAddressTagMask);
  }

  template <typename T, typename A>
  static T Read(A address) {
    T value;
    address = AddressUntag(address);
    VIXL_ASSERT((sizeof(value) == 1) || (sizeof(value) == 2) ||
                (sizeof(value) == 4) || (sizeof(value) == 8) ||
                (sizeof(value) == 16));
    memcpy(&value, reinterpret_cast<const char *>(address), sizeof(value));
    return value;
  }

  template <typename T, typename A>
  static void Write(A address, T value) {
    address = AddressUntag(address);
    VIXL_ASSERT((sizeof(value) == 1) || (sizeof(value) == 2) ||
                (sizeof(value) == 4) || (sizeof(value) == 8) ||
                (sizeof(value) == 16));
    memcpy(reinterpret_cast<char *>(address), &value, sizeof(value));
  }
};

// Represent a register (r0-r31, v0-v31).
template<int kSizeInBytes>
class SimRegisterBase {
 public:
  SimRegisterBase() : written_since_last_log_(false) {}

  // Write the specified value. The value is zero-extended if necessary.
  template<typename T>
  void Set(T new_value) {
    VIXL_STATIC_ASSERT(sizeof(new_value) <= kSizeInBytes);
    if (sizeof(new_value) < kSizeInBytes) {
      // All AArch64 registers are zero-extending.
      memset(value_ + sizeof(new_value), 0, kSizeInBytes - sizeof(new_value));
    }
    memcpy(value_, &new_value, sizeof(new_value));
    NotifyRegisterWrite();
  }

  // Insert a typed value into a register, leaving the rest of the register
  // unchanged. The lane parameter indicates where in the register the value
  // should be inserted, in the range [ 0, sizeof(value_) / sizeof(T) ), where
  // 0 represents the least significant bits.
  template<typename T>
  void Insert(int lane, T new_value) {
    VIXL_ASSERT(lane >= 0);
    VIXL_ASSERT((sizeof(new_value) +
                 (lane * sizeof(new_value))) <= kSizeInBytes);
    memcpy(&value_[lane * sizeof(new_value)], &new_value, sizeof(new_value));
    NotifyRegisterWrite();
  }

  // Read the value as the specified type. The value is truncated if necessary.
  template<typename T>
  T Get(int lane = 0) const {
    T result;
    VIXL_ASSERT(lane >= 0);
    VIXL_ASSERT((sizeof(result) + (lane * sizeof(result))) <= kSizeInBytes);
    memcpy(&result, &value_[lane * sizeof(result)], sizeof(result));
    return result;
  }

  // TODO: Make this return a map of updated bytes, so that we can highlight
  // updated lanes for load-and-insert. (That never happens for scalar code, but
  // NEON has some instructions that can update individual lanes.)
  bool WrittenSinceLastLog() const {
    return written_since_last_log_;
  }

  void NotifyRegisterLogged() {
    written_since_last_log_ = false;
  }

 protected:
  uint8_t value_[kSizeInBytes];

  // Helpers to aid with register tracing.
  bool written_since_last_log_;

  void NotifyRegisterWrite() {
    written_since_last_log_ = true;
  }
};
typedef SimRegisterBase<kXRegSizeInBytes> SimRegister;      // r0-r31
typedef SimRegisterBase<kQRegSizeInBytes> SimVRegister;     // v0-v31

// Representation of a vector register, with typed getters and setters for lanes
// and additional information to represent lane state.
class LogicVRegister {
 public:
  inline LogicVRegister(SimVRegister& other)  // NOLINT
      : register_(other) {
    for (unsigned i = 0; i < sizeof(saturated_) / sizeof(saturated_[0]); i++) {
      saturated_[i] = kNotSaturated;
    }
    for (unsigned i = 0; i < sizeof(round_) / sizeof(round_[0]); i++) {
      round_[i] = 0;
    }
  }

  int64_t Int(VectorFormat vform, int index) const {
    int64_t element;
    switch (LaneSizeInBitsFromFormat(vform)) {
      case 8: element = register_.Get<int8_t>(index); break;
      case 16: element = register_.Get<int16_t>(index); break;
      case 32: element = register_.Get<int32_t>(index); break;
      case 64: element = register_.Get<int64_t>(index); break;
      default: VIXL_UNREACHABLE(); return 0;
    }
    return element;
  }

  uint64_t Uint(VectorFormat vform, int index) const {
    uint64_t element;
    switch (LaneSizeInBitsFromFormat(vform)) {
      case 8: element = register_.Get<uint8_t>(index); break;
      case 16: element = register_.Get<uint16_t>(index); break;
      case 32: element = register_.Get<uint32_t>(index); break;
      case 64: element = register_.Get<uint64_t>(index); break;
      default: VIXL_UNREACHABLE(); return 0;
    }
    return element;
  }

  int64_t IntLeftJustified(VectorFormat vform, int index) const {
    return Int(vform, index) << (64 - LaneSizeInBitsFromFormat(vform));
  }

  uint64_t UintLeftJustified(VectorFormat vform, int index) const {
    return Uint(vform, index) << (64 - LaneSizeInBitsFromFormat(vform));
  }

  void SetInt(VectorFormat vform, int index, int64_t value) const {
    switch (LaneSizeInBitsFromFormat(vform)) {
      case 8: register_.Insert(index, static_cast<int8_t>(value)); break;
      case 16: register_.Insert(index, static_cast<int16_t>(value)); break;
      case 32: register_.Insert(index, static_cast<int32_t>(value)); break;
      case 64: register_.Insert(index, static_cast<int64_t>(value)); break;
      default: VIXL_UNREACHABLE(); return;
    }
  }

  void SetUint(VectorFormat vform, int index, uint64_t value) const {
    switch (LaneSizeInBitsFromFormat(vform)) {
      case 8: register_.Insert(index, static_cast<uint8_t>(value)); break;
      case 16: register_.Insert(index, static_cast<uint16_t>(value)); break;
      case 32: register_.Insert(index, static_cast<uint32_t>(value)); break;
      case 64: register_.Insert(index, static_cast<uint64_t>(value)); break;
      default: VIXL_UNREACHABLE(); return;
    }
  }

  void ReadUintFromMem(VectorFormat vform, int index, uint64_t addr) const {
    switch (LaneSizeInBitsFromFormat(vform)) {
      case 8: register_.Insert(index, Memory::Read<uint8_t>(addr)); break;
      case 16: register_.Insert(index, Memory::Read<uint16_t>(addr)); break;
      case 32: register_.Insert(index, Memory::Read<uint32_t>(addr)); break;
      case 64: register_.Insert(index, Memory::Read<uint64_t>(addr)); break;
      default: VIXL_UNREACHABLE(); return;
    }
  }

  void WriteUintToMem(VectorFormat vform, int index, uint64_t addr) const {
    uint64_t value = Uint(vform, index);
    switch (LaneSizeInBitsFromFormat(vform)) {
      case 8: Memory::Write(addr, static_cast<uint8_t>(value)); break;
      case 16: Memory::Write(addr, static_cast<uint16_t>(value)); break;
      case 32: Memory::Write(addr, static_cast<uint32_t>(value)); break;
      case 64: Memory::Write(addr, value); break;
    }
  }

  template <typename T>
  T Float(int index) const {
    return register_.Get<T>(index);
  }

  template <typename T>
  void SetFloat(int index, T value) const {
    register_.Insert(index, value);
  }

  // When setting a result in a register of size less than Q, the top bits of
  // the Q register must be cleared.
  void ClearForWrite(VectorFormat vform) const {
    unsigned size = RegisterSizeInBytesFromFormat(vform);
    for (unsigned i = size; i < kQRegSizeInBytes; i++) {
      SetUint(kFormat16B, i, 0);
    }
  }

  // Saturation state for each lane of a vector.
  enum Saturation {
    kNotSaturated = 0,
    kSignedSatPositive = 1 << 0,
    kSignedSatNegative = 1 << 1,
    kSignedSatMask = kSignedSatPositive | kSignedSatNegative,
    kSignedSatUndefined = kSignedSatMask,
    kUnsignedSatPositive = 1 << 2,
    kUnsignedSatNegative = 1 << 3,
    kUnsignedSatMask = kUnsignedSatPositive | kUnsignedSatNegative,
    kUnsignedSatUndefined = kUnsignedSatMask
  };

  // Getters for saturation state.
  Saturation GetSignedSaturation(int index) {
    return static_cast<Saturation>(saturated_[index] & kSignedSatMask);
  }

  Saturation GetUnsignedSaturation(int index) {
    return static_cast<Saturation>(saturated_[index] & kUnsignedSatMask);
  }

  // Setters for saturation state.
  void ClearSat(int index) {
    saturated_[index] = kNotSaturated;
  }

  void SetSignedSat(int index, bool positive) {
    SetSatFlag(index, positive ? kSignedSatPositive : kSignedSatNegative);
  }

  void SetUnsignedSat(int index, bool positive) {
    SetSatFlag(index, positive ? kUnsignedSatPositive : kUnsignedSatNegative);
  }

  void SetSatFlag(int index, Saturation sat) {
    saturated_[index] = static_cast<Saturation>(saturated_[index] | sat);
    VIXL_ASSERT((sat & kUnsignedSatMask) != kUnsignedSatUndefined);
    VIXL_ASSERT((sat & kSignedSatMask) != kSignedSatUndefined);
  }

  // Saturate lanes of a vector based on saturation state.
  LogicVRegister& SignedSaturate(VectorFormat vform) {
    for (int i = 0; i < LaneCountFromFormat(vform); i++) {
      Saturation sat = GetSignedSaturation(i);
      if (sat == kSignedSatPositive) {
        SetInt(vform, i, MaxIntFromFormat(vform));
      } else if (sat == kSignedSatNegative) {
        SetInt(vform, i, MinIntFromFormat(vform));
      }
    }
    return *this;
  }

  LogicVRegister& UnsignedSaturate(VectorFormat vform) {
    for (int i = 0; i < LaneCountFromFormat(vform); i++) {
      Saturation sat = GetUnsignedSaturation(i);
      if (sat == kUnsignedSatPositive) {
        SetUint(vform, i, MaxUintFromFormat(vform));
      } else if (sat == kUnsignedSatNegative) {
        SetUint(vform, i, 0);
      }
    }
    return *this;
  }

  // Getter for rounding state.
  bool GetRounding(int index) {
    return round_[index];
  }

  // Setter for rounding state.
  void SetRounding(int index, bool round) {
    round_[index] = round;
  }

  // Round lanes of a vector based on rounding state.
  LogicVRegister& Round(VectorFormat vform) {
    for (int i = 0; i < LaneCountFromFormat(vform); i++) {
      SetInt(vform, i, Int(vform, i) + (GetRounding(i) ? 1 : 0));
    }
    return *this;
  }

  // Unsigned halve lanes of a vector, and use the saturation state to set the
  // top bit.
  LogicVRegister& Uhalve(VectorFormat vform) {
    for (int i = 0; i < LaneCountFromFormat(vform); i++) {
      uint64_t val = Uint(vform, i);
      SetRounding(i, (val & 1) == 1);
      val >>= 1;
      if (GetUnsignedSaturation(i) != kNotSaturated) {
        // If the operation causes unsigned saturation, the bit shifted into the
        // most significant bit must be set.
        val |= (MaxUintFromFormat(vform) >> 1) + 1;
      }
      SetInt(vform, i, val);
    }
    return *this;
  }

  // Signed halve lanes of a vector, and use the carry state to set the top bit.
  LogicVRegister& Halve(VectorFormat vform) {
    for (int i = 0; i < LaneCountFromFormat(vform); i++) {
      int64_t val = Int(vform, i);
      SetRounding(i, (val & 1) == 1);
      val >>= 1;
      if (GetSignedSaturation(i) != kNotSaturated) {
        // If the operation causes signed saturation, the sign bit must be
        // inverted.
        val ^= (MaxUintFromFormat(vform) >> 1) + 1;
      }
      SetInt(vform, i, val);
    }
    return *this;
  }

 private:
  SimVRegister& register_;

  // Allocate one saturation state entry per lane; largest register is type Q,
  // and lanes can be a minimum of one byte wide.
  Saturation saturated_[kQRegSizeInBytes];

  // Allocate one rounding state entry per lane.
  bool round_[kQRegSizeInBytes];
};

// The proper way to initialize a simulated system register (such as NZCV) is as
// follows:
//  SimSystemRegister nzcv = SimSystemRegister::DefaultValueFor(NZCV);
class SimSystemRegister {
 public:
  // The default constructor represents a register which has no writable bits.
  // It is not possible to set its value to anything other than 0.
  SimSystemRegister() : value_(0), write_ignore_mask_(0xffffffff) { }

  uint32_t RawValue() const {
    return value_;
  }

  void SetRawValue(uint32_t new_value) {
    value_ = (value_ & write_ignore_mask_) | (new_value & ~write_ignore_mask_);
  }

  uint32_t Bits(int msb, int lsb) const {
    return unsigned_bitextract_32(msb, lsb, value_);
  }

  int32_t SignedBits(int msb, int lsb) const {
    return signed_bitextract_32(msb, lsb, value_);
  }

  void SetBits(int msb, int lsb, uint32_t bits);

  // Default system register values.
  static SimSystemRegister DefaultValueFor(SystemRegister id);

#define DEFINE_GETTER(Name, HighBit, LowBit, Func)                            \
  uint32_t Name() const { return Func(HighBit, LowBit); }              \
  void Set##Name(uint32_t bits) { SetBits(HighBit, LowBit, bits); }
#define DEFINE_WRITE_IGNORE_MASK(Name, Mask)                                  \
  static const uint32_t Name##WriteIgnoreMask = ~static_cast<uint32_t>(Mask);

  SYSTEM_REGISTER_FIELDS_LIST(DEFINE_GETTER, DEFINE_WRITE_IGNORE_MASK)

#undef DEFINE_ZERO_BITS
#undef DEFINE_GETTER

 protected:
  // Most system registers only implement a few of the bits in the word. Other
  // bits are "read-as-zero, write-ignored". The write_ignore_mask argument
  // describes the bits which are not modifiable.
  SimSystemRegister(uint32_t value, uint32_t write_ignore_mask)
      : value_(value), write_ignore_mask_(write_ignore_mask) { }

  uint32_t value_;
  uint32_t write_ignore_mask_;
};


class SimExclusiveLocalMonitor {
 public:
  SimExclusiveLocalMonitor() : kSkipClearProbability(8), seed_(0x87654321) {
    Clear();
  }

  // Clear the exclusive monitor (like clrex).
  void Clear() {
    address_ = 0;
    size_ = 0;
  }

  // Clear the exclusive monitor most of the time.
  void MaybeClear() {
    if ((seed_ % kSkipClearProbability) != 0) {
      Clear();
    }

    // Advance seed_ using a simple linear congruential generator.
    seed_ = (seed_ * 48271) % 2147483647;
  }

  // Mark the address range for exclusive access (like load-exclusive).
  void MarkExclusive(uint64_t address, size_t size) {
    address_ = address;
    size_ = size;
  }

  // Return true if the address range is marked (like store-exclusive).
  // This helper doesn't implicitly clear the monitor.
  bool IsExclusive(uint64_t address, size_t size) {
    VIXL_ASSERT(size > 0);
    // Be pedantic: Require both the address and the size to match.
    return (size == size_) && (address == address_);
  }

 private:
  uint64_t address_;
  size_t size_;

  const int kSkipClearProbability;
  uint32_t seed_;
};


// We can't accurate simulate the global monitor since it depends on external
// influences. Instead, this implementation occasionally causes accesses to
// fail, according to kPassProbability.
class SimExclusiveGlobalMonitor {
 public:
  SimExclusiveGlobalMonitor() : kPassProbability(8), seed_(0x87654321) {}

  bool IsExclusive(uint64_t address, size_t size) {
    USE(address, size);

    bool pass = (seed_ % kPassProbability) != 0;
    // Advance seed_ using a simple linear congruential generator.
    seed_ = (seed_ * 48271) % 2147483647;
    return pass;
  }

 private:
  const int kPassProbability;
  uint32_t seed_;
};

class Redirection;

class Simulator : public DecoderVisitor {
  friend class AutoLockSimulatorCache;

 public:
  explicit Simulator(Decoder* decoder, FILE* stream = stdout);
  ~Simulator();

  // Moz changes.
  void init(Decoder* decoder, FILE* stream);
  static Simulator* Current();
  static Simulator* Create();
  static void Destroy(Simulator* sim);
  uintptr_t stackLimit() const;
  uintptr_t* addressOfStackLimit();
  bool overRecursed(uintptr_t newsp = 0) const;
  bool overRecursedWithExtra(uint32_t extra) const;
  int64_t call(uint8_t* entry, int argument_count, ...);
  void setRedirection(Redirection* redirection);
  Redirection* redirection() const;
  static void* RedirectNativeFunction(void* nativeFunction, js::jit::ABIFunctionType type);
  void setGPR32Result(int32_t result);
  void setGPR64Result(int64_t result);
  void setFP32Result(float result);
  void setFP64Result(double result);
  void VisitCallRedirection(const Instruction* instr);
  static inline uintptr_t StackLimit() {
    return Simulator::Current()->stackLimit();
  }

  void ResetState();

  // Run the simulator.
  virtual void Run();
  void RunFrom(const Instruction* first);

  // Simulation helpers.
  const Instruction* pc() const { return pc_; }
  const Instruction* get_pc() const { return pc_; }

  template <typename T>
  T get_pc_as() const { return reinterpret_cast<T>(const_cast<Instruction*>(pc())); }

  void set_pc(const Instruction* new_pc) {
    pc_ = Memory::AddressUntag(new_pc);
    pc_modified_ = true;
  }

  void set_resume_pc(void* new_resume_pc);

  void increment_pc() {
    if (!pc_modified_) {
      pc_ = pc_->NextInstruction();
    }

    pc_modified_ = false;
  }

  void ExecuteInstruction();

  // Declare all Visitor functions.
  #define DECLARE(A) virtual void Visit##A(const Instruction* instr);
  VISITOR_LIST(DECLARE)
  #undef DECLARE

  // Integer register accessors.

  // Basic accessor: Read the register as the specified type.
  template<typename T>
  T reg(unsigned code, Reg31Mode r31mode = Reg31IsZeroRegister) const {
    VIXL_ASSERT(code < kNumberOfRegisters);
    if ((code == 31) && (r31mode == Reg31IsZeroRegister)) {
      T result;
      memset(&result, 0, sizeof(result));
      return result;
    }
    return registers_[code].Get<T>();
  }

  // Common specialized accessors for the reg() template.
  int32_t wreg(unsigned code,
               Reg31Mode r31mode = Reg31IsZeroRegister) const {
    return reg<int32_t>(code, r31mode);
  }

  int64_t xreg(unsigned code,
               Reg31Mode r31mode = Reg31IsZeroRegister) const {
    return reg<int64_t>(code, r31mode);
  }

  // As above, with parameterized size and return type. The value is
  // either zero-extended or truncated to fit, as required.
  template<typename T>
  T reg(unsigned size, unsigned code,
        Reg31Mode r31mode = Reg31IsZeroRegister) const {
    uint64_t raw;
    switch (size) {
      case kWRegSize: raw = reg<uint32_t>(code, r31mode); break;
      case kXRegSize: raw = reg<uint64_t>(code, r31mode); break;
      default:
        VIXL_UNREACHABLE();
        return 0;
    }

    T result;
    VIXL_STATIC_ASSERT(sizeof(result) <= sizeof(raw));
    // Copy the result and truncate to fit. This assumes a little-endian host.
    memcpy(&result, &raw, sizeof(result));
    return result;
  }

  // Use int64_t by default if T is not specified.
  int64_t reg(unsigned size, unsigned code,
              Reg31Mode r31mode = Reg31IsZeroRegister) const {
    return reg<int64_t>(size, code, r31mode);
  }

  enum RegLogMode {
    LogRegWrites,
    NoRegLog
  };

  // Write 'value' into an integer register. The value is zero-extended. This
  // behaviour matches AArch64 register writes.
  template<typename T>
  void set_reg(unsigned code, T value,
               RegLogMode log_mode = LogRegWrites,
               Reg31Mode r31mode = Reg31IsZeroRegister) {
    VIXL_STATIC_ASSERT((sizeof(T) == kWRegSizeInBytes) ||
                       (sizeof(T) == kXRegSizeInBytes));
    VIXL_ASSERT(code < kNumberOfRegisters);

    if ((code == 31) && (r31mode == Reg31IsZeroRegister)) {
      return;
    }

    registers_[code].Set(value);

    if (log_mode == LogRegWrites) LogRegister(code, r31mode);
  }

  // Common specialized accessors for the set_reg() template.
  void set_wreg(unsigned code, int32_t value,
                RegLogMode log_mode = LogRegWrites,
                Reg31Mode r31mode = Reg31IsZeroRegister) {
    set_reg(code, value, log_mode, r31mode);
  }

  void set_xreg(unsigned code, int64_t value,
                RegLogMode log_mode = LogRegWrites,
                Reg31Mode r31mode = Reg31IsZeroRegister) {
    set_reg(code, value, log_mode, r31mode);
  }

  // As above, with parameterized size and type. The value is either
  // zero-extended or truncated to fit, as required.
  template<typename T>
  void set_reg(unsigned size, unsigned code, T value,
               RegLogMode log_mode = LogRegWrites,
               Reg31Mode r31mode = Reg31IsZeroRegister) {
    // Zero-extend the input.
    uint64_t raw = 0;
    VIXL_STATIC_ASSERT(sizeof(value) <= sizeof(raw));
    memcpy(&raw, &value, sizeof(value));

    // Write (and possibly truncate) the value.
    switch (size) {
      case kWRegSize:
        set_reg(code, static_cast<uint32_t>(raw), log_mode, r31mode);
        break;
      case kXRegSize:
        set_reg(code, raw, log_mode, r31mode);
        break;
      default:
        VIXL_UNREACHABLE();
        return;
    }
  }

  // Common specialized accessors for the set_reg() template.

  // Commonly-used special cases.
  template<typename T>
  void set_lr(T value) {
    set_reg(kLinkRegCode, value);
  }

  template<typename T>
  void set_sp(T value) {
    set_reg(31, value, LogRegWrites, Reg31IsStackPointer);
  }

  // Vector register accessors.
  // These are equivalent to the integer register accessors, but for vector
  // registers.

  // A structure for representing a 128-bit Q register.
  struct qreg_t { uint8_t val[kQRegSizeInBytes]; };

  // Basic accessor: read the register as the specified type.
  template<typename T>
  T vreg(unsigned code) const {
    VIXL_STATIC_ASSERT((sizeof(T) == kBRegSizeInBytes) ||
                       (sizeof(T) == kHRegSizeInBytes) ||
                       (sizeof(T) == kSRegSizeInBytes) ||
                       (sizeof(T) == kDRegSizeInBytes) ||
                       (sizeof(T) == kQRegSizeInBytes));
    VIXL_ASSERT(code < kNumberOfVRegisters);

    return vregisters_[code].Get<T>();
  }

  // Common specialized accessors for the vreg() template.
  int8_t breg(unsigned code) const {
    return vreg<int8_t>(code);
  }

  int16_t hreg(unsigned code) const {
    return vreg<int16_t>(code);
  }

  float sreg(unsigned code) const {
    return vreg<float>(code);
  }

  uint32_t sreg_bits(unsigned code) const {
    return vreg<uint32_t>(code);
  }

  double dreg(unsigned code) const {
    return vreg<double>(code);
  }

  uint64_t dreg_bits(unsigned code) const {
    return vreg<uint64_t>(code);
  }

  qreg_t qreg(unsigned code)  const {
    return vreg<qreg_t>(code);
  }

  // As above, with parameterized size and return type. The value is
  // either zero-extended or truncated to fit, as required.
  template<typename T>
  T vreg(unsigned size, unsigned code) const {
    uint64_t raw = 0;
    T result;

    switch (size) {
      case kSRegSize: raw = vreg<uint32_t>(code); break;
      case kDRegSize: raw = vreg<uint64_t>(code); break;
      default:
        VIXL_UNREACHABLE();
        break;
    }

    VIXL_STATIC_ASSERT(sizeof(result) <= sizeof(raw));
    // Copy the result and truncate to fit. This assumes a little-endian host.
    memcpy(&result, &raw, sizeof(result));
    return result;
  }

  inline SimVRegister& vreg(unsigned code) {
    return vregisters_[code];
  }

  // Basic accessor: Write the specified value.
  template<typename T>
  void set_vreg(unsigned code, T value,
                RegLogMode log_mode = LogRegWrites) {
    VIXL_STATIC_ASSERT((sizeof(value) == kBRegSizeInBytes) ||
                       (sizeof(value) == kHRegSizeInBytes) ||
                       (sizeof(value) == kSRegSizeInBytes) ||
                       (sizeof(value) == kDRegSizeInBytes) ||
                       (sizeof(value) == kQRegSizeInBytes));
    VIXL_ASSERT(code < kNumberOfVRegisters);
    vregisters_[code].Set(value);

    if (log_mode == LogRegWrites) {
      LogVRegister(code, GetPrintRegisterFormat(value));
    }
  }

  // Common specialized accessors for the set_vreg() template.
  void set_breg(unsigned code, int8_t value,
                RegLogMode log_mode = LogRegWrites) {
    set_vreg(code, value, log_mode);
  }

  void set_hreg(unsigned code, int16_t value,
                RegLogMode log_mode = LogRegWrites) {
    set_vreg(code, value, log_mode);
  }

  void set_sreg(unsigned code, float value,
                RegLogMode log_mode = LogRegWrites) {
    set_vreg(code, value, log_mode);
  }

  void set_sreg_bits(unsigned code, uint32_t value,
                RegLogMode log_mode = LogRegWrites) {
    set_vreg(code, value, log_mode);
  }

  void set_dreg(unsigned code, double value,
                RegLogMode log_mode = LogRegWrites) {
    set_vreg(code, value, log_mode);
  }

  void set_dreg_bits(unsigned code, uint64_t value,
                RegLogMode log_mode = LogRegWrites) {
    set_vreg(code, value, log_mode);
  }

  void set_qreg(unsigned code, qreg_t value,
                RegLogMode log_mode = LogRegWrites) {
    set_vreg(code, value, log_mode);
  }

  bool N() const { return nzcv_.N() != 0; }
  bool Z() const { return nzcv_.Z() != 0; }
  bool C() const { return nzcv_.C() != 0; }
  bool V() const { return nzcv_.V() != 0; }
  SimSystemRegister& nzcv() { return nzcv_; }

  // TODO: Find a way to make the fpcr_ members return the proper types, so
  // these accessors are not necessary.
  FPRounding RMode() { return static_cast<FPRounding>(fpcr_.RMode()); }
  bool DN() { return fpcr_.DN() != 0; }
  SimSystemRegister& fpcr() { return fpcr_; }

  // Specify relevant register formats for Print(V)Register and related helpers.
  enum PrintRegisterFormat {
    // The lane size.
    kPrintRegLaneSizeB = 0 << 0,
    kPrintRegLaneSizeH = 1 << 0,
    kPrintRegLaneSizeS = 2 << 0,
    kPrintRegLaneSizeW = kPrintRegLaneSizeS,
    kPrintRegLaneSizeD = 3 << 0,
    kPrintRegLaneSizeX = kPrintRegLaneSizeD,
    kPrintRegLaneSizeQ = 4 << 0,

    kPrintRegLaneSizeOffset = 0,
    kPrintRegLaneSizeMask = 7 << 0,

    // The lane count.
    kPrintRegAsScalar = 0,
    kPrintRegAsDVector = 1 << 3,
    kPrintRegAsQVector = 2 << 3,

    kPrintRegAsVectorMask = 3 << 3,

    // Indicate floating-point format lanes. (This flag is only supported for S-
    // and D-sized lanes.)
    kPrintRegAsFP = 1 << 5,

    // Supported combinations.

    kPrintXReg = kPrintRegLaneSizeX | kPrintRegAsScalar,
    kPrintWReg = kPrintRegLaneSizeW | kPrintRegAsScalar,
    kPrintSReg = kPrintRegLaneSizeS | kPrintRegAsScalar | kPrintRegAsFP,
    kPrintDReg = kPrintRegLaneSizeD | kPrintRegAsScalar | kPrintRegAsFP,

    kPrintReg1B = kPrintRegLaneSizeB | kPrintRegAsScalar,
    kPrintReg8B = kPrintRegLaneSizeB | kPrintRegAsDVector,
    kPrintReg16B = kPrintRegLaneSizeB | kPrintRegAsQVector,
    kPrintReg1H = kPrintRegLaneSizeH | kPrintRegAsScalar,
    kPrintReg4H = kPrintRegLaneSizeH | kPrintRegAsDVector,
    kPrintReg8H = kPrintRegLaneSizeH | kPrintRegAsQVector,
    kPrintReg1S = kPrintRegLaneSizeS | kPrintRegAsScalar,
    kPrintReg2S = kPrintRegLaneSizeS | kPrintRegAsDVector,
    kPrintReg4S = kPrintRegLaneSizeS | kPrintRegAsQVector,
    kPrintReg1SFP = kPrintRegLaneSizeS | kPrintRegAsScalar | kPrintRegAsFP,
    kPrintReg2SFP = kPrintRegLaneSizeS | kPrintRegAsDVector | kPrintRegAsFP,
    kPrintReg4SFP = kPrintRegLaneSizeS | kPrintRegAsQVector | kPrintRegAsFP,
    kPrintReg1D = kPrintRegLaneSizeD | kPrintRegAsScalar,
    kPrintReg2D = kPrintRegLaneSizeD | kPrintRegAsQVector,
    kPrintReg1DFP = kPrintRegLaneSizeD | kPrintRegAsScalar | kPrintRegAsFP,
    kPrintReg2DFP = kPrintRegLaneSizeD | kPrintRegAsQVector | kPrintRegAsFP,
    kPrintReg1Q = kPrintRegLaneSizeQ | kPrintRegAsScalar
  };

  unsigned GetPrintRegLaneSizeInBytesLog2(PrintRegisterFormat format) {
    return (format & kPrintRegLaneSizeMask) >> kPrintRegLaneSizeOffset;
  }

  unsigned GetPrintRegLaneSizeInBytes(PrintRegisterFormat format) {
    return 1 << GetPrintRegLaneSizeInBytesLog2(format);
  }

  unsigned GetPrintRegSizeInBytesLog2(PrintRegisterFormat format) {
    if (format & kPrintRegAsDVector) return kDRegSizeInBytesLog2;
    if (format & kPrintRegAsQVector) return kQRegSizeInBytesLog2;

    // Scalar types.
    return GetPrintRegLaneSizeInBytesLog2(format);
  }

  unsigned GetPrintRegSizeInBytes(PrintRegisterFormat format) {
    return 1 << GetPrintRegSizeInBytesLog2(format);
  }

  unsigned GetPrintRegLaneCount(PrintRegisterFormat format) {
    unsigned reg_size_log2 = GetPrintRegSizeInBytesLog2(format);
    unsigned lane_size_log2 = GetPrintRegLaneSizeInBytesLog2(format);
    VIXL_ASSERT(reg_size_log2 >= lane_size_log2);
    return 1 << (reg_size_log2 - lane_size_log2);
  }

  PrintRegisterFormat GetPrintRegisterFormatForSize(unsigned reg_size,
                                                    unsigned lane_size);

  PrintRegisterFormat GetPrintRegisterFormatForSize(unsigned size) {
    return GetPrintRegisterFormatForSize(size, size);
  }

  PrintRegisterFormat GetPrintRegisterFormatForSizeFP(unsigned size) {
    switch (size) {
      default: VIXL_UNREACHABLE(); return kPrintDReg;
      case kDRegSizeInBytes: return kPrintDReg;
      case kSRegSizeInBytes: return kPrintSReg;
    }
  }

  PrintRegisterFormat GetPrintRegisterFormatTryFP(PrintRegisterFormat format) {
    if ((GetPrintRegLaneSizeInBytes(format) == kSRegSizeInBytes) ||
        (GetPrintRegLaneSizeInBytes(format) == kDRegSizeInBytes)) {
      return static_cast<PrintRegisterFormat>(format | kPrintRegAsFP);
    }
    return format;
  }

  template<typename T>
  PrintRegisterFormat GetPrintRegisterFormat(T value) {
    return GetPrintRegisterFormatForSize(sizeof(value));
  }

  PrintRegisterFormat GetPrintRegisterFormat(double value) {
    VIXL_STATIC_ASSERT(sizeof(value) == kDRegSizeInBytes);
    return GetPrintRegisterFormatForSizeFP(sizeof(value));
  }

  PrintRegisterFormat GetPrintRegisterFormat(float value) {
    VIXL_STATIC_ASSERT(sizeof(value) == kSRegSizeInBytes);
    return GetPrintRegisterFormatForSizeFP(sizeof(value));
  }

  PrintRegisterFormat GetPrintRegisterFormat(VectorFormat vform);

  // Print all registers of the specified types.
  void PrintRegisters();
  void PrintVRegisters();
  void PrintSystemRegisters();

  // As above, but only print the registers that have been updated.
  void PrintWrittenRegisters();
  void PrintWrittenVRegisters();

  // As above, but respect LOG_REG and LOG_VREG.
  void LogWrittenRegisters() {
    if (trace_parameters() & LOG_REGS) PrintWrittenRegisters();
  }
  void LogWrittenVRegisters() {
    if (trace_parameters() & LOG_VREGS) PrintWrittenVRegisters();
  }
  void LogAllWrittenRegisters() {
    LogWrittenRegisters();
    LogWrittenVRegisters();
  }

  // Print individual register values (after update).
  void PrintRegister(unsigned code, Reg31Mode r31mode = Reg31IsStackPointer);
  void PrintVRegister(unsigned code, PrintRegisterFormat format);
  void PrintSystemRegister(SystemRegister id);

  // Like Print* (above), but respect trace_parameters().
  void LogRegister(unsigned code, Reg31Mode r31mode = Reg31IsStackPointer) {
    if (trace_parameters() & LOG_REGS) PrintRegister(code, r31mode);
  }
  void LogVRegister(unsigned code, PrintRegisterFormat format) {
    if (trace_parameters() & LOG_VREGS) PrintVRegister(code, format);
  }
  void LogSystemRegister(SystemRegister id) {
    if (trace_parameters() & LOG_SYSREGS) PrintSystemRegister(id);
  }

  // Print memory accesses.
  void PrintRead(uintptr_t address, unsigned reg_code,
                 PrintRegisterFormat format);
  void PrintWrite(uintptr_t address, unsigned reg_code,
                 PrintRegisterFormat format);
  void PrintVRead(uintptr_t address, unsigned reg_code,
                  PrintRegisterFormat format, unsigned lane);
  void PrintVWrite(uintptr_t address, unsigned reg_code,
                   PrintRegisterFormat format, unsigned lane);

  // Like Print* (above), but respect trace_parameters().
  void LogRead(uintptr_t address, unsigned reg_code,
               PrintRegisterFormat format) {
    if (trace_parameters() & LOG_REGS) PrintRead(address, reg_code, format);
  }
  void LogWrite(uintptr_t address, unsigned reg_code,
                PrintRegisterFormat format) {
    if (trace_parameters() & LOG_WRITE) PrintWrite(address, reg_code, format);
  }
  void LogVRead(uintptr_t address, unsigned reg_code,
                PrintRegisterFormat format, unsigned lane = 0) {
    if (trace_parameters() & LOG_VREGS) {
      PrintVRead(address, reg_code, format, lane);
    }
  }
  void LogVWrite(uintptr_t address, unsigned reg_code,
                 PrintRegisterFormat format, unsigned lane = 0) {
    if (trace_parameters() & LOG_WRITE) {
      PrintVWrite(address, reg_code, format, lane);
    }
  }

  // Helper functions for register tracing.
  void PrintRegisterRawHelper(unsigned code, Reg31Mode r31mode,
                              int size_in_bytes = kXRegSizeInBytes);
  void PrintVRegisterRawHelper(unsigned code, int bytes = kQRegSizeInBytes,
                               int lsb = 0);
  void PrintVRegisterFPHelper(unsigned code, unsigned lane_size_in_bytes,
                              int lane_count = 1, int rightmost_lane = 0);

  void DoUnreachable(const Instruction* instr);
  void DoTrace(const Instruction* instr);
  void DoLog(const Instruction* instr);

  static const char* WRegNameForCode(unsigned code,
                                     Reg31Mode mode = Reg31IsZeroRegister);
  static const char* XRegNameForCode(unsigned code,
                                     Reg31Mode mode = Reg31IsZeroRegister);
  static const char* SRegNameForCode(unsigned code);
  static const char* DRegNameForCode(unsigned code);
  static const char* VRegNameForCode(unsigned code);

  bool coloured_trace() const { return coloured_trace_; }
  void set_coloured_trace(bool value);

  int trace_parameters() const { return trace_parameters_; }
  void set_trace_parameters(int parameters);

  void set_instruction_stats(bool value);

  // Clear the simulated local monitor to force the next store-exclusive
  // instruction to fail.
  void ClearLocalMonitor() {
    local_monitor_.Clear();
  }

  void SilenceExclusiveAccessWarning() {
    print_exclusive_access_warning_ = false;
  }

 protected:
  const char* clr_normal;
  const char* clr_flag_name;
  const char* clr_flag_value;
  const char* clr_reg_name;
  const char* clr_reg_value;
  const char* clr_vreg_name;
  const char* clr_vreg_value;
  const char* clr_memory_address;
  const char* clr_warning;
  const char* clr_warning_message;
  const char* clr_printf;

  // Simulation helpers ------------------------------------
  bool ConditionPassed(Condition cond) {
    switch (cond) {
      case eq:
        return Z();
      case ne:
        return !Z();
      case hs:
        return C();
      case lo:
        return !C();
      case mi:
        return N();
      case pl:
        return !N();
      case vs:
        return V();
      case vc:
        return !V();
      case hi:
        return C() && !Z();
      case ls:
        return !(C() && !Z());
      case ge:
        return N() == V();
      case lt:
        return N() != V();
      case gt:
        return !Z() && (N() == V());
      case le:
        return !(!Z() && (N() == V()));
      case nv:
        VIXL_FALLTHROUGH();
      case al:
        return true;
      default:
        VIXL_UNREACHABLE();
        return false;
    }
  }

  bool ConditionPassed(Instr cond) {
    return ConditionPassed(static_cast<Condition>(cond));
  }

  bool ConditionFailed(Condition cond) {
    return !ConditionPassed(cond);
  }

  void AddSubHelper(const Instruction* instr, int64_t op2);
  int64_t AddWithCarry(unsigned reg_size,
                       bool set_flags,
                       int64_t src1,
                       int64_t src2,
                       int64_t carry_in = 0);
  void LogicalHelper(const Instruction* instr, int64_t op2);
  void ConditionalCompareHelper(const Instruction* instr, int64_t op2);
  void LoadStoreHelper(const Instruction* instr,
                       int64_t offset,
                       AddrMode addrmode);
  void LoadStorePairHelper(const Instruction* instr, AddrMode addrmode);
  uintptr_t AddressModeHelper(unsigned addr_reg,
                              int64_t offset,
                              AddrMode addrmode);
  void NEONLoadStoreMultiStructHelper(const Instruction* instr,
                                      AddrMode addr_mode);
  void NEONLoadStoreSingleStructHelper(const Instruction* instr,
                                       AddrMode addr_mode);

  uint64_t AddressUntag(uint64_t address) {
    return address & ~kAddressTagMask;
  }

  template <typename T>
  T* AddressUntag(T* address) {
    uintptr_t address_raw = reinterpret_cast<uintptr_t>(address);
    return reinterpret_cast<T*>(AddressUntag(address_raw));
  }

  int64_t ShiftOperand(unsigned reg_size,
                       int64_t value,
                       Shift shift_type,
                       unsigned amount);
  int64_t Rotate(unsigned reg_width,
                 int64_t value,
                 Shift shift_type,
                 unsigned amount);
  int64_t ExtendValue(unsigned reg_width,
                      int64_t value,
                      Extend extend_type,
                      unsigned left_shift = 0);
  uint16_t PolynomialMult(uint8_t op1, uint8_t op2);

  void ld1(VectorFormat vform,
           LogicVRegister dst,
           uint64_t addr);
  void ld1(VectorFormat vform,
           LogicVRegister dst,
           int index,
           uint64_t addr);
  void ld1r(VectorFormat vform,
            LogicVRegister dst,
            uint64_t addr);
  void ld2(VectorFormat vform,
           LogicVRegister dst1,
           LogicVRegister dst2,
           uint64_t addr);
  void ld2(VectorFormat vform,
           LogicVRegister dst1,
           LogicVRegister dst2,
           int index,
           uint64_t addr);
  void ld2r(VectorFormat vform,
           LogicVRegister dst1,
           LogicVRegister dst2,
           uint64_t addr);
  void ld3(VectorFormat vform,
           LogicVRegister dst1,
           LogicVRegister dst2,
           LogicVRegister dst3,
           uint64_t addr);
  void ld3(VectorFormat vform,
           LogicVRegister dst1,
           LogicVRegister dst2,
           LogicVRegister dst3,
           int index,
           uint64_t addr);
  void ld3r(VectorFormat vform,
           LogicVRegister dst1,
           LogicVRegister dst2,
           LogicVRegister dst3,
           uint64_t addr);
  void ld4(VectorFormat vform,
           LogicVRegister dst1,
           LogicVRegister dst2,
           LogicVRegister dst3,
           LogicVRegister dst4,
           uint64_t addr);
  void ld4(VectorFormat vform,
           LogicVRegister dst1,
           LogicVRegister dst2,
           LogicVRegister dst3,
           LogicVRegister dst4,
           int index,
           uint64_t addr);
  void ld4r(VectorFormat vform,
           LogicVRegister dst1,
           LogicVRegister dst2,
           LogicVRegister dst3,
           LogicVRegister dst4,
           uint64_t addr);
  void st1(VectorFormat vform,
           LogicVRegister src,
           uint64_t addr);
  void st1(VectorFormat vform,
           LogicVRegister src,
           int index,
           uint64_t addr);
  void st2(VectorFormat vform,
           LogicVRegister src,
           LogicVRegister src2,
           uint64_t addr);
  void st2(VectorFormat vform,
           LogicVRegister src,
           LogicVRegister src2,
           int index,
           uint64_t addr);
  void st3(VectorFormat vform,
           LogicVRegister src,
           LogicVRegister src2,
           LogicVRegister src3,
           uint64_t addr);
  void st3(VectorFormat vform,
           LogicVRegister src,
           LogicVRegister src2,
           LogicVRegister src3,
           int index,
           uint64_t addr);
  void st4(VectorFormat vform,
           LogicVRegister src,
           LogicVRegister src2,
           LogicVRegister src3,
           LogicVRegister src4,
           uint64_t addr);
  void st4(VectorFormat vform,
           LogicVRegister src,
           LogicVRegister src2,
           LogicVRegister src3,
           LogicVRegister src4,
           int index,
           uint64_t addr);
  LogicVRegister cmp(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& src1,
                     const LogicVRegister& src2,
                     Condition cond);
  LogicVRegister cmp(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& src1,
                     int imm,
                     Condition cond);
  LogicVRegister cmptst(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src1,
                        const LogicVRegister& src2);
  LogicVRegister add(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& src1,
                     const LogicVRegister& src2);
  LogicVRegister addp(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src1,
                      const LogicVRegister& src2);
  LogicVRegister mla(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& src1,
                     const LogicVRegister& src2);
  LogicVRegister mls(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& src1,
                     const LogicVRegister& src2);
  LogicVRegister mul(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& src1,
                     const LogicVRegister& src2);
  LogicVRegister mul(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& src1,
                     const LogicVRegister& src2,
                     int index);
  LogicVRegister mla(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& src1,
                     const LogicVRegister& src2,
                     int index);
  LogicVRegister mls(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& src1,
                     const LogicVRegister& src2,
                     int index);
  LogicVRegister pmul(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src1,
                      const LogicVRegister& src2);

  typedef LogicVRegister (Simulator::*ByElementOp)(VectorFormat vform,
                                                   LogicVRegister dst,
                                                   const LogicVRegister& src1,
                                                   const LogicVRegister& src2,
                                                   int index);
  LogicVRegister fmul(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src1,
                      const LogicVRegister& src2,
                      int index);
  LogicVRegister fmla(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src1,
                      const LogicVRegister& src2,
                      int index);
  LogicVRegister fmls(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src1,
                      const LogicVRegister& src2,
                      int index);
  LogicVRegister fmulx(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src1,
                       const LogicVRegister& src2,
                       int index);
  LogicVRegister smull(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src1,
                       const LogicVRegister& src2,
                       int index);
  LogicVRegister smull2(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src1,
                        const LogicVRegister& src2,
                        int index);
  LogicVRegister umull(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src1,
                       const LogicVRegister& src2,
                       int index);
  LogicVRegister umull2(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src1,
                        const LogicVRegister& src2,
                        int index);
  LogicVRegister smlal(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src1,
                       const LogicVRegister& src2,
                       int index);
  LogicVRegister smlal2(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src1,
                        const LogicVRegister& src2,
                        int index);
  LogicVRegister umlal(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src1,
                       const LogicVRegister& src2,
                       int index);
  LogicVRegister umlal2(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src1,
                        const LogicVRegister& src2,
                        int index);
  LogicVRegister smlsl(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src1,
                       const LogicVRegister& src2,
                       int index);
  LogicVRegister smlsl2(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src1,
                        const LogicVRegister& src2,
                        int index);
  LogicVRegister umlsl(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src1,
                       const LogicVRegister& src2,
                       int index);
  LogicVRegister umlsl2(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src1,
                        const LogicVRegister& src2,
                        int index);
  LogicVRegister sqdmull(VectorFormat vform,
                         LogicVRegister dst,
                         const LogicVRegister& src1,
                         const LogicVRegister& src2,
                         int index);
  LogicVRegister sqdmull2(VectorFormat vform,
                          LogicVRegister dst,
                          const LogicVRegister& src1,
                          const LogicVRegister& src2,
                          int index);
  LogicVRegister sqdmlal(VectorFormat vform,
                         LogicVRegister dst,
                         const LogicVRegister& src1,
                         const LogicVRegister& src2,
                         int index);
  LogicVRegister sqdmlal2(VectorFormat vform,
                          LogicVRegister dst,
                          const LogicVRegister& src1,
                          const LogicVRegister& src2,
                          int index);
  LogicVRegister sqdmlsl(VectorFormat vform,
                         LogicVRegister dst,
                         const LogicVRegister& src1,
                         const LogicVRegister& src2,
                         int index);
  LogicVRegister sqdmlsl2(VectorFormat vform,
                          LogicVRegister dst,
                          const LogicVRegister& src1,
                          const LogicVRegister& src2,
                          int index);
  LogicVRegister sqdmulh(VectorFormat vform,
                         LogicVRegister dst,
                         const LogicVRegister& src1,
                         const LogicVRegister& src2,
                         int index);
  LogicVRegister sqrdmulh(VectorFormat vform,
                          LogicVRegister dst,
                          const LogicVRegister& src1,
                          const LogicVRegister& src2,
                          int index);
  LogicVRegister sub(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& src1,
                     const LogicVRegister& src2);
  LogicVRegister and_(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src1,
                      const LogicVRegister& src2);
  LogicVRegister orr(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& src1,
                     const LogicVRegister& src2);
  LogicVRegister orn(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& src1,
                     const LogicVRegister& src2);
  LogicVRegister eor(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& src1,
                     const LogicVRegister& src2);
  LogicVRegister bic(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& src1,
                     const LogicVRegister& src2);
  LogicVRegister bic(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& src,
                     uint64_t imm);
  LogicVRegister bif(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& src1,
                     const LogicVRegister& src2);
  LogicVRegister bit(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& src1,
                     const LogicVRegister& src2);
  LogicVRegister bsl(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& src1,
                     const LogicVRegister& src2);
  LogicVRegister cls(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& src);
  LogicVRegister clz(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& src);
  LogicVRegister cnt(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& src);
  LogicVRegister not_(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src);
  LogicVRegister rbit(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src);
  LogicVRegister rev(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& src,
                     int revSize);
  LogicVRegister rev16(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src);
  LogicVRegister rev32(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src);
  LogicVRegister rev64(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src);
  LogicVRegister addlp(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src,
                       bool is_signed,
                       bool do_accumulate);
  LogicVRegister saddlp(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src);
  LogicVRegister uaddlp(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src);
  LogicVRegister sadalp(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src);
  LogicVRegister uadalp(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src);
  LogicVRegister ext(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& src1,
                     const LogicVRegister& src2,
                     int index);
  LogicVRegister ins_element(VectorFormat vform,
                             LogicVRegister dst,
                             int dst_index,
                             const LogicVRegister& src,
                             int src_index);
  LogicVRegister ins_immediate(VectorFormat vform,
                               LogicVRegister dst,
                               int dst_index,
                               uint64_t imm);
  LogicVRegister dup_element(VectorFormat vform,
                             LogicVRegister dst,
                             const LogicVRegister& src,
                             int src_index);
  LogicVRegister dup_immediate(VectorFormat vform,
                               LogicVRegister dst,
                               uint64_t imm);
  LogicVRegister movi(VectorFormat vform,
                      LogicVRegister dst,
                      uint64_t imm);
  LogicVRegister mvni(VectorFormat vform,
                      LogicVRegister dst,
                      uint64_t imm);
  LogicVRegister orr(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& src,
                     uint64_t imm);
  LogicVRegister sshl(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src1,
                      const LogicVRegister& src2);
  LogicVRegister ushl(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src1,
                      const LogicVRegister& src2);
  LogicVRegister sminmax(VectorFormat vform,
                         LogicVRegister dst,
                         const LogicVRegister& src1,
                         const LogicVRegister& src2,
                         bool max);
  LogicVRegister smax(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& src1,
                     const LogicVRegister& src2);
  LogicVRegister smin(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& src1,
                     const LogicVRegister& src2);
  LogicVRegister sminmaxp(VectorFormat vform,
                          LogicVRegister dst,
                          int dst_index,
                          const LogicVRegister& src,
                          bool max);
  LogicVRegister smaxp(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src1,
                       const LogicVRegister& src2);
  LogicVRegister sminp(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src1,
                       const LogicVRegister& src2);
  LogicVRegister addp(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src);
  LogicVRegister addv(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src);
  LogicVRegister uaddlv(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src);
  LogicVRegister saddlv(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src);
  LogicVRegister sminmaxv(VectorFormat vform,
                          LogicVRegister dst,
                          const LogicVRegister& src,
                          bool max);
  LogicVRegister smaxv(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src);
  LogicVRegister sminv(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src);
  LogicVRegister uxtl(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src);
  LogicVRegister uxtl2(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src);
  LogicVRegister sxtl(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src);
  LogicVRegister sxtl2(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src);
  LogicVRegister tbl(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& tab,
                     const LogicVRegister& ind);
  LogicVRegister tbl(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& tab,
                     const LogicVRegister& tab2,
                     const LogicVRegister& ind);
  LogicVRegister tbl(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& tab,
                     const LogicVRegister& tab2,
                     const LogicVRegister& tab3,
                     const LogicVRegister& ind);
  LogicVRegister tbl(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& tab,
                     const LogicVRegister& tab2,
                     const LogicVRegister& tab3,
                     const LogicVRegister& tab4,
                     const LogicVRegister& ind);
  LogicVRegister tbx(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& tab,
                     const LogicVRegister& ind);
  LogicVRegister tbx(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& tab,
                     const LogicVRegister& tab2,
                     const LogicVRegister& ind);
  LogicVRegister tbx(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& tab,
                     const LogicVRegister& tab2,
                     const LogicVRegister& tab3,
                     const LogicVRegister& ind);
  LogicVRegister tbx(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& tab,
                     const LogicVRegister& tab2,
                     const LogicVRegister& tab3,
                     const LogicVRegister& tab4,
                     const LogicVRegister& ind);
  LogicVRegister uaddl(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src1,
                       const LogicVRegister& src2);
  LogicVRegister uaddl2(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src1,
                        const LogicVRegister& src2);
  LogicVRegister uaddw(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src1,
                       const LogicVRegister& src2);
  LogicVRegister uaddw2(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src1,
                        const LogicVRegister& src2);
  LogicVRegister saddl(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src1,
                       const LogicVRegister& src2);
  LogicVRegister saddl2(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src1,
                        const LogicVRegister& src2);
  LogicVRegister saddw(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src1,
                       const LogicVRegister& src2);
  LogicVRegister saddw2(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src1,
                        const LogicVRegister& src2);
  LogicVRegister usubl(VectorFormat vform,
                         LogicVRegister dst,
                         const LogicVRegister& src1,
                         const LogicVRegister& src2);
  LogicVRegister usubl2(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src1,
                        const LogicVRegister& src2);
  LogicVRegister usubw(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src1,
                       const LogicVRegister& src2);
  LogicVRegister usubw2(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src1,
                        const LogicVRegister& src2);
  LogicVRegister ssubl(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src1,
                       const LogicVRegister& src2);
  LogicVRegister ssubl2(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src1,
                        const LogicVRegister& src2);
  LogicVRegister ssubw(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src1,
                       const LogicVRegister& src2);
  LogicVRegister ssubw2(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src1,
                        const LogicVRegister& src2);
  LogicVRegister uminmax(VectorFormat vform,
                         LogicVRegister dst,
                         const LogicVRegister& src1,
                         const LogicVRegister& src2,
                         bool max);
  LogicVRegister umax(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& src1,
                     const LogicVRegister& src2);
  LogicVRegister umin(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& src1,
                     const LogicVRegister& src2);
  LogicVRegister uminmaxp(VectorFormat vform,
                          LogicVRegister dst,
                          int dst_index,
                          const LogicVRegister& src,
                          bool max);
  LogicVRegister umaxp(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src1,
                       const LogicVRegister& src2);
  LogicVRegister uminp(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src1,
                       const LogicVRegister& src2);
  LogicVRegister uminmaxv(VectorFormat vform,
                          LogicVRegister dst,
                          const LogicVRegister& src,
                          bool max);
  LogicVRegister umaxv(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src);
  LogicVRegister uminv(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src);
  LogicVRegister trn1(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src1,
                      const LogicVRegister& src2);
  LogicVRegister trn2(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src1,
                      const LogicVRegister& src2);
  LogicVRegister zip1(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src1,
                      const LogicVRegister& src2);
  LogicVRegister zip2(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src1,
                      const LogicVRegister& src2);
  LogicVRegister uzp1(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src1,
                      const LogicVRegister& src2);
  LogicVRegister uzp2(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src1,
                      const LogicVRegister& src2);
  LogicVRegister shl(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& src,
                     int shift);
  LogicVRegister scvtf(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src,
                       int fbits,
                       FPRounding rounding_mode);
  LogicVRegister ucvtf(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src,
                       int fbits,
                       FPRounding rounding_mode);
  LogicVRegister sshll(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src,
                       int shift);
  LogicVRegister sshll2(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src,
                        int shift);
  LogicVRegister shll(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src);
  LogicVRegister shll2(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src);
  LogicVRegister ushll(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src,
                       int shift);
  LogicVRegister ushll2(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src,
                        int shift);
  LogicVRegister sli(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& src,
                     int shift);
  LogicVRegister sri(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& src,
                     int shift);
  LogicVRegister sshr(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src,
                      int shift);
  LogicVRegister ushr(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src,
                      int shift);
  LogicVRegister ssra(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src,
                      int shift);
  LogicVRegister usra(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src,
                      int shift);
  LogicVRegister srsra(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src,
                       int shift);
  LogicVRegister ursra(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src,
                       int shift);
  LogicVRegister suqadd(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src);
  LogicVRegister usqadd(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src);
  LogicVRegister sqshl(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src,
                       int shift);
  LogicVRegister uqshl(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src,
                       int shift);
  LogicVRegister sqshlu(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src,
                        int shift);
  LogicVRegister abs(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& src);
  LogicVRegister neg(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& src);
  LogicVRegister extractnarrow(VectorFormat vform,
                               LogicVRegister dst,
                               bool dstIsSigned,
                               const LogicVRegister& src,
                               bool srcIsSigned);
  LogicVRegister xtn(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& src);
  LogicVRegister sqxtn(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src);
  LogicVRegister uqxtn(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src);
  LogicVRegister sqxtun(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src);
  LogicVRegister absdiff(VectorFormat vform,
                         LogicVRegister dst,
                         const LogicVRegister& src1,
                         const LogicVRegister& src2,
                         bool issigned);
  LogicVRegister saba(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src1,
                      const LogicVRegister& src2);
  LogicVRegister uaba(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src1,
                      const LogicVRegister& src2);
  LogicVRegister shrn(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src,
                      int shift);
  LogicVRegister shrn2(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src,
                      int shift);
  LogicVRegister rshrn(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src,
                       int shift);
  LogicVRegister rshrn2(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src,
                        int shift);
  LogicVRegister uqshrn(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src,
                        int shift);
  LogicVRegister uqshrn2(VectorFormat vform,
                         LogicVRegister dst,
                         const LogicVRegister& src,
                         int shift);
  LogicVRegister uqrshrn(VectorFormat vform,
                         LogicVRegister dst,
                         const LogicVRegister& src,
                         int shift);
  LogicVRegister uqrshrn2(VectorFormat vform,
                          LogicVRegister dst,
                          const LogicVRegister& src,
                          int shift);
  LogicVRegister sqshrn(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src,
                        int shift);
  LogicVRegister sqshrn2(VectorFormat vform,
                         LogicVRegister dst,
                         const LogicVRegister& src,
                         int shift);
  LogicVRegister sqrshrn(VectorFormat vform,
                         LogicVRegister dst,
                         const LogicVRegister& src,
                         int shift);
  LogicVRegister sqrshrn2(VectorFormat vform,
                          LogicVRegister dst,
                          const LogicVRegister& src,
                          int shift);
  LogicVRegister sqshrun(VectorFormat vform,
                         LogicVRegister dst,
                         const LogicVRegister& src,
                         int shift);
  LogicVRegister sqshrun2(VectorFormat vform,
                          LogicVRegister dst,
                          const LogicVRegister& src,
                          int shift);
  LogicVRegister sqrshrun(VectorFormat vform,
                          LogicVRegister dst,
                          const LogicVRegister& src,
                          int shift);
  LogicVRegister sqrshrun2(VectorFormat vform,
                           LogicVRegister dst,
                           const LogicVRegister& src,
                           int shift);
  LogicVRegister sqrdmulh(VectorFormat vform,
                          LogicVRegister dst,
                          const LogicVRegister& src1,
                          const LogicVRegister& src2,
                          bool round = true);
  LogicVRegister sqdmulh(VectorFormat vform,
                         LogicVRegister dst,
                         const LogicVRegister& src1,
                         const LogicVRegister& src2);
  #define NEON_3VREG_LOGIC_LIST(V) \
    V(addhn)                       \
    V(addhn2)                      \
    V(raddhn)                      \
    V(raddhn2)                     \
    V(subhn)                       \
    V(subhn2)                      \
    V(rsubhn)                      \
    V(rsubhn2)                     \
    V(pmull)                       \
    V(pmull2)                      \
    V(sabal)                       \
    V(sabal2)                      \
    V(uabal)                       \
    V(uabal2)                      \
    V(sabdl)                       \
    V(sabdl2)                      \
    V(uabdl)                       \
    V(uabdl2)                      \
    V(smull)                       \
    V(smull2)                      \
    V(umull)                       \
    V(umull2)                      \
    V(smlal)                       \
    V(smlal2)                      \
    V(umlal)                       \
    V(umlal2)                      \
    V(smlsl)                       \
    V(smlsl2)                      \
    V(umlsl)                       \
    V(umlsl2)                      \
    V(sqdmlal)                     \
    V(sqdmlal2)                    \
    V(sqdmlsl)                     \
    V(sqdmlsl2)                    \
    V(sqdmull)                     \
    V(sqdmull2)

  #define DEFINE_LOGIC_FUNC(FXN)                   \
    LogicVRegister FXN(VectorFormat vform,         \
                       LogicVRegister dst,         \
                       const LogicVRegister& src1, \
                       const LogicVRegister& src2);
  NEON_3VREG_LOGIC_LIST(DEFINE_LOGIC_FUNC)
  #undef DEFINE_LOGIC_FUNC

  #define NEON_FP3SAME_LIST(V)  \
    V(fadd,   FPAdd,   false)   \
    V(fsub,   FPSub,   true)    \
    V(fmul,   FPMul,   true)    \
    V(fmulx,  FPMulx,  true)    \
    V(fdiv,   FPDiv,   true)    \
    V(fmax,   FPMax,   false)   \
    V(fmin,   FPMin,   false)   \
    V(fmaxnm, FPMaxNM, false)   \
    V(fminnm, FPMinNM, false)

  #define DECLARE_NEON_FP_VECTOR_OP(FN, OP, PROCNAN) \
    template <typename T>                            \
    LogicVRegister FN(VectorFormat vform,            \
                      LogicVRegister dst,            \
                      const LogicVRegister& src1,    \
                      const LogicVRegister& src2);   \
    LogicVRegister FN(VectorFormat vform,            \
                      LogicVRegister dst,            \
                      const LogicVRegister& src1,    \
                      const LogicVRegister& src2);
  NEON_FP3SAME_LIST(DECLARE_NEON_FP_VECTOR_OP)
  #undef DECLARE_NEON_FP_VECTOR_OP

  #define NEON_FPPAIRWISE_LIST(V)         \
    V(faddp,   fadd,   FPAdd)             \
    V(fmaxp,   fmax,   FPMax)             \
    V(fmaxnmp, fmaxnm, FPMaxNM)           \
    V(fminp,   fmin,   FPMin)             \
    V(fminnmp, fminnm, FPMinNM)

  #define DECLARE_NEON_FP_PAIR_OP(FNP, FN, OP)       \
    LogicVRegister FNP(VectorFormat vform,           \
                       LogicVRegister dst,           \
                       const LogicVRegister& src1,   \
                       const LogicVRegister& src2);  \
    LogicVRegister FNP(VectorFormat vform,           \
                       LogicVRegister dst,           \
                       const LogicVRegister& src);
  NEON_FPPAIRWISE_LIST(DECLARE_NEON_FP_PAIR_OP)
  #undef DECLARE_NEON_FP_PAIR_OP

  template <typename T>
  LogicVRegister frecps(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src1,
                        const LogicVRegister& src2);
  LogicVRegister frecps(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src1,
                        const LogicVRegister& src2);
  template <typename T>
  LogicVRegister frsqrts(VectorFormat vform,
                         LogicVRegister dst,
                         const LogicVRegister& src1,
                         const LogicVRegister& src2);
  LogicVRegister frsqrts(VectorFormat vform,
                         LogicVRegister dst,
                         const LogicVRegister& src1,
                         const LogicVRegister& src2);
  template <typename T>
  LogicVRegister fmla(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src1,
                      const LogicVRegister& src2);
  LogicVRegister fmla(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src1,
                      const LogicVRegister& src2);
  template <typename T>
  LogicVRegister fmls(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src1,
                      const LogicVRegister& src2);
  LogicVRegister fmls(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src1,
                      const LogicVRegister& src2);
  LogicVRegister fnmul(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src1,
                       const LogicVRegister& src2);

  template <typename T>
  LogicVRegister fcmp(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src1,
                      const LogicVRegister& src2,
                      Condition cond);
  LogicVRegister fcmp(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src1,
                      const LogicVRegister& src2,
                      Condition cond);
  LogicVRegister fabscmp(VectorFormat vform,
                         LogicVRegister dst,
                         const LogicVRegister& src1,
                         const LogicVRegister& src2,
                         Condition cond);
  LogicVRegister fcmp_zero(VectorFormat vform,
                           LogicVRegister dst,
                           const LogicVRegister& src,
                           Condition cond);

  template <typename T>
  LogicVRegister fneg(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src);
  LogicVRegister fneg(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src);
  template <typename T>
  LogicVRegister frecpx(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src);
  LogicVRegister frecpx(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src);
  template <typename T>
  LogicVRegister fabs_(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src);
  LogicVRegister fabs_(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src);
  LogicVRegister fabd(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src1,
                      const LogicVRegister& src2);
  LogicVRegister frint(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src,
                       FPRounding rounding_mode,
                       bool inexact_exception = false);
  LogicVRegister fcvts(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src,
                       FPRounding rounding_mode,
                       int fbits = 0);
  LogicVRegister fcvtu(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src,
                       FPRounding rounding_mode,
                       int fbits = 0);
  LogicVRegister fcvtl(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src);
  LogicVRegister fcvtl2(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src);
  LogicVRegister fcvtn(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src);
  LogicVRegister fcvtn2(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src);
  LogicVRegister fcvtxn(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src);
  LogicVRegister fcvtxn2(VectorFormat vform,
                         LogicVRegister dst,
                         const LogicVRegister& src);
  LogicVRegister fsqrt(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src);
  LogicVRegister frsqrte(VectorFormat vform,
                         LogicVRegister dst,
                         const LogicVRegister& src);
  LogicVRegister frecpe(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src,
                        FPRounding rounding);
  LogicVRegister ursqrte(VectorFormat vform,
                         LogicVRegister dst,
                         const LogicVRegister& src);
  LogicVRegister urecpe(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src);

  typedef float (Simulator::*FPMinMaxOp)(float a, float b);

  LogicVRegister fminmaxv(VectorFormat vform,
                          LogicVRegister dst,
                          const LogicVRegister& src,
                          FPMinMaxOp Op);

  LogicVRegister fminv(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src);
  LogicVRegister fmaxv(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src);
  LogicVRegister fminnmv(VectorFormat vform,
                         LogicVRegister dst,
                         const LogicVRegister& src);
  LogicVRegister fmaxnmv(VectorFormat vform,
                         LogicVRegister dst,
                         const LogicVRegister& src);

  static const uint32_t CRC32_POLY  = 0x04C11DB7;
  static const uint32_t CRC32C_POLY = 0x1EDC6F41;
  uint32_t Poly32Mod2(unsigned n, uint64_t data, uint32_t poly);
  template <typename T>
  uint32_t Crc32Checksum(uint32_t acc, T val, uint32_t poly);
  uint32_t Crc32Checksum(uint32_t acc, uint64_t val, uint32_t poly);

  void SysOp_W(int op, int64_t val);

  template <typename T>
  T FPRecipSqrtEstimate(T op);
  template <typename T>
  T FPRecipEstimate(T op, FPRounding rounding);
  template <typename T, typename R>
  R FPToFixed(T op, int fbits, bool is_signed, FPRounding rounding);

  void FPCompare(double val0, double val1, FPTrapFlags trap);
  double FPRoundInt(double value, FPRounding round_mode);
  double FPToDouble(float value);
  float FPToFloat(double value, FPRounding round_mode);
  float FPToFloat(float16 value);
  float16 FPToFloat16(float value, FPRounding round_mode);
  float16 FPToFloat16(double value, FPRounding round_mode);
  double recip_sqrt_estimate(double a);
  double recip_estimate(double a);
  double FPRecipSqrtEstimate(double a);
  double FPRecipEstimate(double a);
  double FixedToDouble(int64_t src, int fbits, FPRounding round_mode);
  double UFixedToDouble(uint64_t src, int fbits, FPRounding round_mode);
  float FixedToFloat(int64_t src, int fbits, FPRounding round_mode);
  float UFixedToFloat(uint64_t src, int fbits, FPRounding round_mode);
  int32_t FPToInt32(double value, FPRounding rmode);
  int64_t FPToInt64(double value, FPRounding rmode);
  uint32_t FPToUInt32(double value, FPRounding rmode);
  uint64_t FPToUInt64(double value, FPRounding rmode);

  template <typename T>
  T FPAdd(T op1, T op2);

  template <typename T>
  T FPDiv(T op1, T op2);

  template <typename T>
  T FPMax(T a, T b);

  template <typename T>
  T FPMaxNM(T a, T b);

  template <typename T>
  T FPMin(T a, T b);

  template <typename T>
  T FPMinNM(T a, T b);

  template <typename T>
  T FPMul(T op1, T op2);

  template <typename T>
  T FPMulx(T op1, T op2);

  template <typename T>
  T FPMulAdd(T a, T op1, T op2);

  template <typename T>
  T FPSqrt(T op);

  template <typename T>
  T FPSub(T op1, T op2);

  template <typename T>
  T FPRecipStepFused(T op1, T op2);

  template <typename T>
  T FPRSqrtStepFused(T op1, T op2);

  // This doesn't do anything at the moment. We'll need it if we want support
  // for cumulative exception bits or floating-point exceptions.
  void FPProcessException() { }

  bool FPProcessNaNs(const Instruction* instr);

  // Pseudo Printf instruction
  void DoPrintf(const Instruction* instr);

  // Processor state ---------------------------------------

  // Simulated monitors for exclusive access instructions.
  SimExclusiveLocalMonitor local_monitor_;
  SimExclusiveGlobalMonitor global_monitor_;

  // Output stream.
  FILE* stream_;
  PrintDisassembler* print_disasm_;

  // Instruction statistics instrumentation.
  Instrument* instrumentation_;

  // General purpose registers. Register 31 is the stack pointer.
  SimRegister registers_[kNumberOfRegisters];

  // Vector registers
  SimVRegister vregisters_[kNumberOfVRegisters];

  // Program Status Register.
  // bits[31, 27]: Condition flags N, Z, C, and V.
  //               (Negative, Zero, Carry, Overflow)
  SimSystemRegister nzcv_;

  // Floating-Point Control Register
  SimSystemRegister fpcr_;

  // Only a subset of FPCR features are supported by the simulator. This helper
  // checks that the FPCR settings are supported.
  //
  // This is checked when floating-point instructions are executed, not when
  // FPCR is set. This allows generated code to modify FPCR for external
  // functions, or to save and restore it when entering and leaving generated
  // code.
  void AssertSupportedFPCR() {
    VIXL_ASSERT(fpcr().FZ() == 0);             // No flush-to-zero support.
    VIXL_ASSERT(fpcr().RMode() == FPTieEven);  // Ties-to-even rounding only.

    // The simulator does not support half-precision operations so fpcr().AHP()
    // is irrelevant, and is not checked here.
  }

  static int CalcNFlag(uint64_t result, unsigned reg_size) {
    return (result >> (reg_size - 1)) & 1;
  }

  static int CalcZFlag(uint64_t result) {
    return result == 0;
  }

  static const uint32_t kConditionFlagsMask = 0xf0000000;

  // Stack
  byte* stack_;
  static const int stack_protection_size_ = 128 * KBytes;
  static const int stack_size_ = (2 * MBytes) + (2 * stack_protection_size_);
  byte* stack_limit_;

  Decoder* decoder_;
  // Indicates if the pc has been modified by the instruction and should not be
  // automatically incremented.
  bool pc_modified_;
  const Instruction* pc_;
  const Instruction* resume_pc_;

  static const char* xreg_names[];
  static const char* wreg_names[];
  static const char* sreg_names[];
  static const char* dreg_names[];
  static const char* vreg_names[];

  static const Instruction* kEndOfSimAddress;

 private:
  template <typename T>
  static T FPDefaultNaN();

  // Standard NaN processing.
  template <typename T>
  T FPProcessNaN(T op) {
    VIXL_ASSERT(std::isnan(op));
    if (IsSignallingNaN(op)) {
      FPProcessException();
    }
    return DN() ? FPDefaultNaN<T>() : ToQuietNaN(op);
  }

  template <typename T>
  T FPProcessNaNs(T op1, T op2) {
    if (IsSignallingNaN(op1)) {
      return FPProcessNaN(op1);
    } else if (IsSignallingNaN(op2)) {
      return FPProcessNaN(op2);
    } else if (std::isnan(op1)) {
      VIXL_ASSERT(IsQuietNaN(op1));
      return FPProcessNaN(op1);
    } else if (std::isnan(op2)) {
      VIXL_ASSERT(IsQuietNaN(op2));
      return FPProcessNaN(op2);
    } else {
      return 0.0;
    }
  }

  template <typename T>
  T FPProcessNaNs3(T op1, T op2, T op3) {
    if (IsSignallingNaN(op1)) {
      return FPProcessNaN(op1);
    } else if (IsSignallingNaN(op2)) {
      return FPProcessNaN(op2);
    } else if (IsSignallingNaN(op3)) {
      return FPProcessNaN(op3);
    } else if (std::isnan(op1)) {
      VIXL_ASSERT(IsQuietNaN(op1));
      return FPProcessNaN(op1);
    } else if (std::isnan(op2)) {
      VIXL_ASSERT(IsQuietNaN(op2));
      return FPProcessNaN(op2);
    } else if (std::isnan(op3)) {
      VIXL_ASSERT(IsQuietNaN(op3));
      return FPProcessNaN(op3);
    } else {
      return 0.0;
    }
  }

  bool coloured_trace_;

  // A set of TraceParameters flags.
  int trace_parameters_;

  // Indicates whether the instruction instrumentation is active.
  bool instruction_stats_;

  // Indicates whether the exclusive-access warning has been printed.
  bool print_exclusive_access_warning_;
  void PrintExclusiveAccessWarning();

  // Indicates that the simulator ran out of memory at some point.
  // Data structures may not be fully allocated.
  bool oom_;

 public:
  // True if the simulator ran out of memory during or after construction.
  bool oom() const { return oom_; }

 protected:
  // Moz: Synchronizes access between main thread and compilation threads.
  PRLock* lock_;
#ifdef DEBUG
  PRThread* lockOwner_;
#endif
  Redirection* redirection_;
  mozilla::Vector<int64_t, 0, js::SystemAllocPolicy> spStack_;
};
}  // namespace vixl

#endif  // JS_SIMULATOR_ARM64
#endif  // VIXL_A64_SIMULATOR_A64_H_

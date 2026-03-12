/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/Maybe.h"

#include <algorithm>

#include "jit/AutoWritableJitCode.h"
#if defined(JS_CODEGEN_X86)
#  include "jit/x86/MacroAssembler-x86.h"
#elif defined(JS_CODEGEN_X64)
#  include "jit/x64/MacroAssembler-x64.h"
#else
#  error "Wrong architecture. Only x86 and x64 should build this file!"
#endif

#ifdef _MSC_VER
#  include <intrin.h>  // for __cpuid
#  if defined(_M_X64) && (_MSC_FULL_VER >= 160040219)
#    include <immintrin.h>  // for _xgetbv
#  endif
#endif

using namespace js;
using namespace js::jit;

void AssemblerX86Shared::copyJumpRelocationTable(uint8_t* dest) {
  if (jumpRelocations_.length()) {
    memcpy(dest, jumpRelocations_.buffer(), jumpRelocations_.length());
  }
}

void AssemblerX86Shared::copyDataRelocationTable(uint8_t* dest) {
  if (dataRelocations_.length()) {
    memcpy(dest, dataRelocations_.buffer(), dataRelocations_.length());
  }
}

/* static */
void AssemblerX86Shared::TraceDataRelocations(JSTracer* trc, JitCode* code,
                                              CompactBufferReader& reader) {
  mozilla::Maybe<AutoWritableJitCode> awjc;

  while (reader.more()) {
    size_t offset = reader.readUnsigned();
    MOZ_ASSERT(offset >= sizeof(void*) && offset <= code->instructionsSize());

    uint8_t* src = code->raw() + offset;
    void* data = X86Encoding::GetPointer(src);

#ifdef JS_PUNBOX64
    // Data relocations can be for Values or for raw pointers. If a Value is
    // zero-tagged, we can trace it as if it were a raw pointer. If a Value
    // is not zero-tagged, we have to interpret it as a Value to ensure that the
    // tag bits are masked off to recover the actual pointer.

    uintptr_t word = reinterpret_cast<uintptr_t>(data);
    if (word >> JSVAL_TAG_SHIFT) {
      // This relocation is a Value with a non-zero tag.
      Value value = Value::fromRawBits(word);
      MOZ_ASSERT_IF(value.isGCThing(),
                    gc::IsCellPointerValid(value.toGCThing()));
      TraceManuallyBarrieredEdge(trc, &value, "jit-masm-value");
      if (word != value.asRawBits()) {
        if (awjc.isNothing()) {
          awjc.emplace(code);
        }
        X86Encoding::SetPointer(src, value.bitsAsPunboxPointer());
      }
      continue;
    }
#endif

    // This relocation is a raw pointer or a Value with a zero tag.
    gc::Cell* cell = static_cast<gc::Cell*>(data);
    MOZ_ASSERT(gc::IsCellPointerValid(cell));
    TraceManuallyBarrieredGenericPointerEdge(trc, &cell, "jit-masm-ptr");
    if (cell != data) {
      if (awjc.isNothing()) {
        awjc.emplace(code);
      }
      X86Encoding::SetPointer(src, cell);
    }
  }
}

void AssemblerX86Shared::executableCopy(void* buffer) {
  masm.executableCopy(buffer);
}

void AssemblerX86Shared::processCodeLabels(uint8_t* rawCode) {
  for (const CodeLabel& label : codeLabels_) {
    Bind(rawCode, label);
  }
}

AssemblerX86Shared::Condition AssemblerX86Shared::InvertCondition(
    Condition cond) {
  switch (cond) {
    case Zero:
      return NonZero;
    case NonZero:
      return Zero;
    case LessThan:
      return GreaterThanOrEqual;
    case LessThanOrEqual:
      return GreaterThan;
    case GreaterThan:
      return LessThanOrEqual;
    case GreaterThanOrEqual:
      return LessThan;
    case Above:
      return BelowOrEqual;
    case AboveOrEqual:
      return Below;
    case Below:
      return AboveOrEqual;
    case BelowOrEqual:
      return Above;
    case Overflow:
      return NoOverflow;
    case NoOverflow:
      return Overflow;
    case Signed:
      return NotSigned;
    case NotSigned:
      return Signed;
    case Parity:
      return NoParity;
    case NoParity:
      return Parity;
  }
  MOZ_CRASH("unexpected condition");
}

AssemblerX86Shared::Condition AssemblerX86Shared::UnsignedCondition(
    Condition cond) {
  switch (cond) {
    case Zero:
    case NonZero:
      return cond;
    case LessThan:
    case Below:
      return Below;
    case LessThanOrEqual:
    case BelowOrEqual:
      return BelowOrEqual;
    case GreaterThan:
    case Above:
      return Above;
    case AboveOrEqual:
    case GreaterThanOrEqual:
      return AboveOrEqual;
    default:
      MOZ_CRASH("unexpected condition");
  }
}

AssemblerX86Shared::Condition AssemblerX86Shared::ConditionWithoutEqual(
    Condition cond) {
  switch (cond) {
    case LessThan:
    case LessThanOrEqual:
      return LessThan;
    case Below:
    case BelowOrEqual:
      return Below;
    case GreaterThan:
    case GreaterThanOrEqual:
      return GreaterThan;
    case Above:
    case AboveOrEqual:
      return Above;
    default:
      MOZ_CRASH("unexpected condition");
  }
}

AssemblerX86Shared::DoubleCondition AssemblerX86Shared::InvertCondition(
    DoubleCondition cond) {
  switch (cond) {
    case DoubleEqual:
      return DoubleNotEqualOrUnordered;
    case DoubleEqualOrUnordered:
      return DoubleNotEqual;
    case DoubleNotEqualOrUnordered:
      return DoubleEqual;
    case DoubleNotEqual:
      return DoubleEqualOrUnordered;
    case DoubleLessThan:
      return DoubleGreaterThanOrEqualOrUnordered;
    case DoubleLessThanOrUnordered:
      return DoubleGreaterThanOrEqual;
    case DoubleLessThanOrEqual:
      return DoubleGreaterThanOrUnordered;
    case DoubleLessThanOrEqualOrUnordered:
      return DoubleGreaterThan;
    case DoubleGreaterThan:
      return DoubleLessThanOrEqualOrUnordered;
    case DoubleGreaterThanOrUnordered:
      return DoubleLessThanOrEqual;
    case DoubleGreaterThanOrEqual:
      return DoubleLessThanOrUnordered;
    case DoubleGreaterThanOrEqualOrUnordered:
      return DoubleLessThan;
    default:
      MOZ_CRASH("unexpected condition");
  }
}

CPUInfo::SSEVersion CPUInfo::maxSSEVersion = UnknownSSE;
CPUInfo::SSEVersion CPUInfo::maxEnabledSSEVersion = UnknownSSE;
bool CPUInfo::avxPresent = false;
#ifdef ENABLE_WASM_AVX
bool CPUInfo::avxEnabled = true;
#else
bool CPUInfo::avxEnabled = false;
#endif
bool CPUInfo::popcntPresent = false;
bool CPUInfo::bmi1Present = false;
bool CPUInfo::bmi2Present = false;
bool CPUInfo::lzcntPresent = false;
bool CPUInfo::avx2Present = false;
bool CPUInfo::fmaPresent = false;
bool CPUInfo::f16cPresent = false;

namespace js {
namespace jit {
bool CPUFlagsHaveBeenComputed() { return CPUInfo::FlagsHaveBeenComputed(); }
}  // namespace jit
}  // namespace js

static uintptr_t ReadXGETBV() {
  // We use a variety of low-level mechanisms to get at the xgetbv
  // instruction, including spelling out the xgetbv instruction as bytes,
  // because older compilers and assemblers may not recognize the instruction
  // by name.
  size_t xcr0EAX = 0;
#if defined(_XCR_XFEATURE_ENABLED_MASK)
  xcr0EAX = _xgetbv(_XCR_XFEATURE_ENABLED_MASK);
#elif defined(__GNUC__)
  // xgetbv returns its results in %eax and %edx, and for our purposes here,
  // we're only interested in the %eax value.
  asm(".byte 0x0f, 0x01, 0xd0" : "=a"(xcr0EAX) : "c"(0) : "%edx");
#elif defined(_MSC_VER) && defined(_M_IX86)
  __asm {
        xor ecx, ecx
        _asm _emit 0x0f _asm _emit 0x01 _asm _emit 0xd0
        mov xcr0EAX, eax
  }
#endif
  return xcr0EAX;
}

static void ReadCPUInfo(int* flagsEax, int* flagsEbx, int* flagsEcx,
                        int* flagsEdx) {
#ifdef _MSC_VER
  int cpuinfo[4];
  __cpuid(cpuinfo, *flagsEax);
  *flagsEax = cpuinfo[0];
  *flagsEbx = cpuinfo[1];
  *flagsEcx = cpuinfo[2];
  *flagsEdx = cpuinfo[3];
#elif defined(__GNUC__)
  // Some older 32-bits processors don't fill the ecx register with cpuid, so
  // clobber it before calling cpuid, so that there's no risk of picking
  // random bits indicating SSE3/SSE4 are present. Also make sure that it's
  // set to 0 as an input for BMI detection on all platforms.
  *flagsEcx = 0;
#  ifdef JS_CODEGEN_X64
  asm("cpuid;"
      : "+a"(*flagsEax), "=b"(*flagsEbx), "+c"(*flagsEcx), "=d"(*flagsEdx));
#  else
  // On x86, preserve ebx. The compiler needs it for PIC mode.
  asm("mov %%ebx, %%edi;"
      "cpuid;"
      "xchg %%edi, %%ebx;"
      : "+a"(*flagsEax), "=D"(*flagsEbx), "+c"(*flagsEcx), "=d"(*flagsEdx));
#  endif
#else
#  error "Unsupported compiler"
#endif
}

void CPUInfo::ComputeFlags() {
  MOZ_ASSERT(!FlagsHaveBeenComputed());

  int flagsEax = 1;
  int flagsEbx = 0;
  int flagsEcx = 0;
  int flagsEdx = 0;
  ReadCPUInfo(&flagsEax, &flagsEbx, &flagsEcx, &flagsEdx);

  static constexpr int SSEBit = 1 << 25;
  static constexpr int SSE2Bit = 1 << 26;
  static constexpr int SSE3Bit = 1 << 0;
  static constexpr int SSSE3Bit = 1 << 9;
  static constexpr int SSE41Bit = 1 << 19;
  static constexpr int SSE42Bit = 1 << 20;

  if (flagsEcx & SSE42Bit) {
    maxSSEVersion = SSE4_2;
  } else if (flagsEcx & SSE41Bit) {
    maxSSEVersion = SSE4_1;
  } else if (flagsEcx & SSSE3Bit) {
    maxSSEVersion = SSSE3;
  } else if (flagsEcx & SSE3Bit) {
    maxSSEVersion = SSE3;
  } else if (flagsEdx & SSE2Bit) {
    maxSSEVersion = SSE2;
  } else if (flagsEdx & SSEBit) {
    maxSSEVersion = SSE;
  } else {
    maxSSEVersion = NoSSE;
  }

  if (maxEnabledSSEVersion != UnknownSSE) {
    maxSSEVersion = std::min(maxSSEVersion, maxEnabledSSEVersion);
  }

  static constexpr int AVXBit = 1 << 28;
  static constexpr int XSAVEBit = 1 << 27;
  avxPresent = (flagsEcx & AVXBit) && (flagsEcx & XSAVEBit) && avxEnabled;

  // If the hardware supports AVX, check whether the OS supports it too.
  if (avxPresent) {
    size_t xcr0EAX = ReadXGETBV();
    static constexpr int xcr0SSEBit = 1 << 1;
    static constexpr int xcr0AVXBit = 1 << 2;
    avxPresent = (xcr0EAX & xcr0SSEBit) && (xcr0EAX & xcr0AVXBit);
  }

  // CMOV instruction are supposed to be supported by all CPU which have SSE2
  // enabled. While this might be true, this is not guaranteed by any
  // documentation, nor AMD, nor Intel.
  static constexpr int CMOVBit = 1 << 15;
  MOZ_RELEASE_ASSERT(flagsEdx & CMOVBit,
                     "CMOVcc instruction is not recognized by this CPU.");

  static constexpr int POPCNTBit = 1 << 23;
  popcntPresent = (flagsEcx & POPCNTBit);

  // Use the avxEnabled flag to enable/disable FMA.
  static constexpr int FMABit = 1 << 12;
  fmaPresent = (flagsEcx & FMABit) && avxEnabled;

  // Support for the F16C instruction set. Requires AVX support.
  static constexpr int F16CBit = 1 << 29;
  f16cPresent = avxPresent && (flagsEcx & F16CBit);

  flagsEax = 0x80000001;
  ReadCPUInfo(&flagsEax, &flagsEbx, &flagsEcx, &flagsEdx);

  static constexpr int LZCNTBit = 1 << 5;
  lzcntPresent = (flagsEcx & LZCNTBit);

  flagsEax = 0x7;
  ReadCPUInfo(&flagsEax, &flagsEbx, &flagsEcx, &flagsEdx);

  static constexpr int BMI1Bit = 1 << 3;
  static constexpr int BMI2Bit = 1 << 8;
  static constexpr int AVX2Bit = 1 << 5;
  bmi1Present = (flagsEbx & BMI1Bit);
  bmi2Present = bmi1Present && (flagsEbx & BMI2Bit);
  avx2Present = avxPresent && (flagsEbx & AVX2Bit);

  MOZ_ASSERT(FlagsHaveBeenComputed());
}

/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_none_Architecture_none_h
#define jit_none_Architecture_none_h

// JitSpewer.h is included through MacroAssembler implementations for other
// platforms, so include it here to avoid inadvertent build bustage.
#include "jit/JitSpewer.h"

#include "jit/shared/Architecture-shared.h"

namespace js {
namespace jit {

static const uint32_t SimdMemoryAlignment =
    4;  // Make it 4 to avoid a bunch of div-by-zero warnings
static const uint32_t WasmStackAlignment = 8;
static const uint32_t WasmTrapInstructionLength = 0;

// See comments in wasm::GenerateFunctionPrologue.
static constexpr uint32_t WasmCheckedCallEntryOffset = 0u;
static constexpr uint32_t WasmCheckedTailEntryOffset = 1u;

class Registers {
 public:
  enum RegisterID {
    r0 = 0,
    invalid_reg,
    invalid_reg2,  // To avoid silly static_assert failures.
  };
  typedef uint8_t Code;
  typedef RegisterID Encoding;
  union RegisterContent {
    uintptr_t r;
  };

  typedef uint8_t SetType;

  static uint32_t SetSize(SetType) { MOZ_CRASH(); }
  static uint32_t FirstBit(SetType) { MOZ_CRASH(); }
  static uint32_t LastBit(SetType) { MOZ_CRASH(); }
  static const char* GetName(Code) { MOZ_CRASH(); }
  static Code FromName(const char*) { MOZ_CRASH(); }

  static const Encoding StackPointer = invalid_reg;
  static const Encoding Invalid = invalid_reg;
  static const uint32_t Total = 1;
  static const uint32_t TotalPhys = 0;
  static const uint32_t Allocatable = 0;
  static const SetType AllMask = 0;
  static const SetType ArgRegMask = 0;
  static const SetType VolatileMask = 0;
  static const SetType NonVolatileMask = 0;
  static const SetType NonAllocatableMask = 0;
  static const SetType AllocatableMask = 0;
  static const SetType JSCallMask = 0;
  static const SetType CallMask = 0;
};

typedef uint8_t PackedRegisterMask;

class FloatRegisters {
 public:
  enum FPRegisterID { f0 = 0, invalid_reg };
  typedef FPRegisterID Code;
  typedef FPRegisterID Encoding;
  union RegisterContent {
    double d;
  };

  typedef uint32_t SetType;

  static const char* GetName(Code) { MOZ_CRASH(); }
  static Code FromName(const char*) { MOZ_CRASH(); }

  static const Code Invalid = invalid_reg;
  static const uint32_t Total = 0;
  static const uint32_t TotalPhys = 0;
  static const uint32_t Allocatable = 0;
  static const SetType AllMask = 0;
  static const SetType AllDoubleMask = 0;
  static const SetType AllSingleMask = 0;
  static const SetType VolatileMask = 0;
  static const SetType NonVolatileMask = 0;
  static const SetType NonAllocatableMask = 0;
  static const SetType AllocatableMask = 0;
};

template <typename T>
class TypedRegisterSet;

struct FloatRegister {
  typedef FloatRegisters Codes;
  typedef Codes::Code Code;
  typedef Codes::Encoding Encoding;
  typedef Codes::SetType SetType;

  Code _;

  static uint32_t FirstBit(SetType) { MOZ_CRASH(); }
  static uint32_t LastBit(SetType) { MOZ_CRASH(); }
  static FloatRegister FromCode(uint32_t) { MOZ_CRASH(); }
  bool isSingle() const { MOZ_CRASH(); }
  bool isDouble() const { MOZ_CRASH(); }
  bool isSimd128() const { MOZ_CRASH(); }
  bool isInvalid() const { MOZ_CRASH(); }
  FloatRegister asSingle() const { MOZ_CRASH(); }
  FloatRegister asDouble() const { MOZ_CRASH(); }
  FloatRegister asSimd128() const { MOZ_CRASH(); }
  Code code() const { MOZ_CRASH(); }
  Encoding encoding() const { MOZ_CRASH(); }
  const char* name() const { MOZ_CRASH(); }
  bool volatile_() const { MOZ_CRASH(); }
  bool operator!=(FloatRegister) const { MOZ_CRASH(); }
  bool operator==(FloatRegister) const { MOZ_CRASH(); }
  bool aliases(FloatRegister) const { MOZ_CRASH(); }
  uint32_t numAliased() const { MOZ_CRASH(); }
  FloatRegister aliased(uint32_t) { MOZ_CRASH(); }
  bool equiv(FloatRegister) const { MOZ_CRASH(); }
  uint32_t size() const { MOZ_CRASH(); }
  uint32_t numAlignedAliased() const { MOZ_CRASH(); }
  FloatRegister alignedAliased(uint32_t) { MOZ_CRASH(); }
  SetType alignedOrDominatedAliasedSet() const { MOZ_CRASH(); }

  static constexpr RegTypeName DefaultType = RegTypeName::Float64;

  template <RegTypeName = DefaultType>
  static SetType LiveAsIndexableSet(SetType s) {
    return SetType(0);
  }

  template <RegTypeName Name = DefaultType>
  static SetType AllocatableAsIndexableSet(SetType s) {
    static_assert(Name != RegTypeName::Any, "Allocatable set are not iterable");
    return SetType(0);
  }

  template <typename T>
  static T ReduceSetForPush(T) {
    MOZ_CRASH();
  }
  uint32_t getRegisterDumpOffsetInBytes() { MOZ_CRASH(); }
  static uint32_t SetSize(SetType x) { MOZ_CRASH(); }
  static Code FromName(const char* name) { MOZ_CRASH(); }

  // This is used in static initializers, so produce a bogus value instead of
  // crashing.
  static uint32_t GetPushSizeInBytes(const TypedRegisterSet<FloatRegister>&) {
    return 0;
  }
};

inline bool hasUnaliasedDouble() { MOZ_CRASH(); }
inline bool hasMultiAlias() { MOZ_CRASH(); }

static const uint32_t ShadowStackSpace = 0;
static const uint32_t JumpImmediateRange = INT32_MAX;

#ifdef JS_NUNBOX32
static const int32_t NUNBOX32_TYPE_OFFSET = 4;
static const int32_t NUNBOX32_PAYLOAD_OFFSET = 0;
#endif

}  // namespace jit
}  // namespace js

#endif /* jit_none_Architecture_none_h */

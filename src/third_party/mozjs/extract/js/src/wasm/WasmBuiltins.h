/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 *
 * Copyright 2017 Mozilla Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef wasm_builtins_h
#define wasm_builtins_h

#include "jit/IonTypes.h"

namespace js {
namespace jit {
struct ResumeFromException;
}
namespace wasm {

class WasmFrameIter;
class CodeRange;
class FuncType;

// A wasm::SymbolicAddress represents a pointer to a well-known function/global
// that is embedded in wasm code. Since wasm code is serialized and later
// deserialized into a different address space, symbolic addresses must be used
// for *all* pointers into the address space. The MacroAssembler records a list
// of all SymbolicAddresses and the offsets of their use in the code for later
// patching during static linking.

enum class SymbolicAddress {
  ToInt32,
#if defined(JS_CODEGEN_ARM)
  aeabi_idivmod,
  aeabi_uidivmod,
#endif
  ModD,
  SinD,
  CosD,
  TanD,
  ASinD,
  ACosD,
  ATanD,
  CeilD,
  CeilF,
  FloorD,
  FloorF,
  TruncD,
  TruncF,
  NearbyIntD,
  NearbyIntF,
  ExpD,
  LogD,
  PowD,
  ATan2D,
  HandleDebugTrap,
  HandleThrow,
  HandleTrap,
  ReportV128JSCall,
  CallImport_General,
  CoerceInPlace_ToInt32,
  CoerceInPlace_ToNumber,
  CoerceInPlace_JitEntry,
  CoerceInPlace_ToBigInt,
  AllocateBigInt,
  BoxValue_Anyref,
  DivI64,
  UDivI64,
  ModI64,
  UModI64,
  TruncateDoubleToInt64,
  TruncateDoubleToUint64,
  SaturatingTruncateDoubleToInt64,
  SaturatingTruncateDoubleToUint64,
  Uint64ToFloat32,
  Uint64ToDouble,
  Int64ToFloat32,
  Int64ToDouble,
  MemoryGrow,
  MemorySize,
  WaitI32,
  WaitI64,
  Wake,
  MemCopy32,
  MemCopyShared32,
  DataDrop,
  MemFill32,
  MemFillShared32,
  MemInit32,
  TableCopy,
  ElemDrop,
  TableFill,
  TableGet,
  TableGrow,
  TableInit,
  TableSet,
  TableSize,
  RefFunc,
  RefTest,
  RttSub,
  PreBarrierFiltering,
  PostBarrier,
  PostBarrierFiltering,
  StructNew,
#if defined(ENABLE_WASM_EXCEPTIONS)
  ExceptionNew,
  ThrowException,
  GetLocalExceptionIndex,
  PushRefIntoExn,
#endif
  ArrayNew,
  InlineTypedObjectClass,
#if defined(JS_CODEGEN_MIPS32)
  js_jit_gAtomic64Lock,
#endif
#ifdef WASM_CODEGEN_DEBUG
  PrintI32,
  PrintPtr,
  PrintF32,
  PrintF64,
  PrintText,
#endif
  Limit
};

// The FailureMode indicates whether, immediately after a call to a builtin
// returns, the return value should be checked against an error condition
// (and if so, which one) which signals that the C++ calle has already
// reported an error and thus wasm needs to wasmTrap(Trap::ThrowReported).

enum class FailureMode : uint8_t {
  Infallible,
  FailOnNegI32,
  FailOnNullPtr,
  FailOnInvalidRef
};

// SymbolicAddressSignature carries type information for a function referred
// to by a SymbolicAddress.  In order that |argTypes| can be written out as a
// static initialiser, it has to have fixed length.  At present
// SymbolicAddressType is used to describe functions with at most 6 arguments,
// so |argTypes| has 7 entries in order to allow the last value to be
// MIRType::None, in the hope of catching any accidental overruns of the
// defined section of the array.

static constexpr size_t SymbolicAddressSignatureMaxArgs = 6;

struct SymbolicAddressSignature {
  // The SymbolicAddress that is described.
  const SymbolicAddress identity;
  // The return type, or MIRType::None to denote 'void'.
  const jit::MIRType retType;
  // The failure mode, which is checked by masm.wasmCallBuiltinInstanceMethod.
  const FailureMode failureMode;
  // The number of arguments, 0 .. SymbolicAddressSignatureMaxArgs only.
  const uint8_t numArgs;
  // The argument types; SymbolicAddressSignatureMaxArgs + 1 guard, which
  // should be MIRType::None.
  const jit::MIRType argTypes[SymbolicAddressSignatureMaxArgs + 1];
};

// The 16 in this assertion is derived as follows: SymbolicAddress is probably
// size-4 aligned-4, but it's at the start of the struct, so there's no
// alignment hole before it.  All other components (MIRType and uint8_t) are
// size-1 aligned-1, and there are 8 in total, so it is reasonable to assume
// that they also don't create any alignment holes.  Hence it is also
// reasonable to assume that the actual size is 1 * 4 + 8 * 1 == 12.  The
// worst-plausible-case rounding will take that up to 16.  Hence, the
// assertion uses 16.

static_assert(sizeof(SymbolicAddressSignature) <= 16,
              "SymbolicAddressSignature unexpectedly large");

// These provide argument type information for a subset of the SymbolicAddress
// targets, for which type info is needed to generate correct stackmaps.

extern const SymbolicAddressSignature SASigSinD;
extern const SymbolicAddressSignature SASigCosD;
extern const SymbolicAddressSignature SASigTanD;
extern const SymbolicAddressSignature SASigASinD;
extern const SymbolicAddressSignature SASigACosD;
extern const SymbolicAddressSignature SASigATanD;
extern const SymbolicAddressSignature SASigCeilD;
extern const SymbolicAddressSignature SASigCeilF;
extern const SymbolicAddressSignature SASigFloorD;
extern const SymbolicAddressSignature SASigFloorF;
extern const SymbolicAddressSignature SASigTruncD;
extern const SymbolicAddressSignature SASigTruncF;
extern const SymbolicAddressSignature SASigNearbyIntD;
extern const SymbolicAddressSignature SASigNearbyIntF;
extern const SymbolicAddressSignature SASigExpD;
extern const SymbolicAddressSignature SASigLogD;
extern const SymbolicAddressSignature SASigPowD;
extern const SymbolicAddressSignature SASigATan2D;
extern const SymbolicAddressSignature SASigMemoryGrow;
extern const SymbolicAddressSignature SASigMemorySize;
extern const SymbolicAddressSignature SASigWaitI32;
extern const SymbolicAddressSignature SASigWaitI64;
extern const SymbolicAddressSignature SASigWake;
extern const SymbolicAddressSignature SASigMemCopy32;
extern const SymbolicAddressSignature SASigMemCopyShared32;
extern const SymbolicAddressSignature SASigDataDrop;
extern const SymbolicAddressSignature SASigMemFill32;
extern const SymbolicAddressSignature SASigMemFillShared32;
extern const SymbolicAddressSignature SASigMemInit32;
extern const SymbolicAddressSignature SASigTableCopy;
extern const SymbolicAddressSignature SASigElemDrop;
extern const SymbolicAddressSignature SASigTableFill;
extern const SymbolicAddressSignature SASigTableGet;
extern const SymbolicAddressSignature SASigTableGrow;
extern const SymbolicAddressSignature SASigTableInit;
extern const SymbolicAddressSignature SASigTableSet;
extern const SymbolicAddressSignature SASigTableSize;
extern const SymbolicAddressSignature SASigRefFunc;
extern const SymbolicAddressSignature SASigPreBarrierFiltering;
extern const SymbolicAddressSignature SASigPostBarrier;
extern const SymbolicAddressSignature SASigPostBarrierFiltering;
extern const SymbolicAddressSignature SASigStructNew;
#ifdef ENABLE_WASM_EXCEPTIONS
extern const SymbolicAddressSignature SASigExceptionNew;
extern const SymbolicAddressSignature SASigThrowException;
extern const SymbolicAddressSignature SASigGetLocalExceptionIndex;
extern const SymbolicAddressSignature SASigPushRefIntoExn;
#endif
extern const SymbolicAddressSignature SASigArrayNew;
extern const SymbolicAddressSignature SASigRefTest;
extern const SymbolicAddressSignature SASigRttSub;

bool IsRoundingFunction(SymbolicAddress callee, jit::RoundingMode* mode);

// A SymbolicAddress that NeedsBuiltinThunk() will call through a thunk to the
// C++ function. This will be true for all normal calls from normal wasm
// function code. Only calls to C++ from other exits/thunks do not need a thunk.

bool NeedsBuiltinThunk(SymbolicAddress sym);

// This function queries whether pc is in one of the process's builtin thunks
// and, if so, returns the CodeRange and pointer to the code segment that the
// CodeRange is relative to.

bool LookupBuiltinThunk(void* pc, const CodeRange** codeRange,
                        uint8_t** codeBase);

// EnsureBuiltinThunksInitialized() must be called, and must succeed, before
// SymbolicAddressTarget() or MaybeGetBuiltinThunk(). This function creates all
// thunks for the process. ReleaseBuiltinThunks() should be called before
// ReleaseProcessExecutableMemory() so that the latter can assert that all
// executable code has been released.

bool EnsureBuiltinThunksInitialized();

bool HandleThrow(JSContext* cx, WasmFrameIter& iter,
                 jit::ResumeFromException* rfe);

void* SymbolicAddressTarget(SymbolicAddress sym);

void* ProvisionalLazyJitEntryStub();

void* MaybeGetBuiltinThunk(JSFunction* f, const FuncType& funcType);

void ReleaseBuiltinThunks();

void* AddressOf(SymbolicAddress imm, jit::ABIFunctionType* abiType);

#ifdef WASM_CODEGEN_DEBUG
void PrintI32(int32_t val);
void PrintF32(float val);
void PrintF64(double val);
void PrintPtr(uint8_t* val);
void PrintText(const char* out);
#endif

}  // namespace wasm
}  // namespace js

#endif  // wasm_builtins_h

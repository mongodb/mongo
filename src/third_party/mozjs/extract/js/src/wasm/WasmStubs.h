/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 *
 * Copyright 2015 Mozilla Foundation
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

#ifndef wasm_stubs_h
#define wasm_stubs_h

#include "wasm/WasmFrameIter.h"  // js::wasm::ExitReason
#include "wasm/WasmGenerator.h"
#include "wasm/WasmOpIter.h"

namespace js {
namespace wasm {

using jit::FloatRegister;
using jit::Register;
using jit::Register64;

// ValType and location for a single result: either in a register or on the
// stack.

class ABIResult {
  ValType type_;
  enum class Location { Gpr, Gpr64, Fpr, Stack } loc_;
  union {
    Register gpr_;
    Register64 gpr64_;
    FloatRegister fpr_;
    uint32_t stackOffset_;
  };

  void validate() {
#ifdef DEBUG
    if (onStack()) {
      return;
    }
    MOZ_ASSERT(inRegister());
    switch (type_.kind()) {
      case ValType::I32:
        MOZ_ASSERT(loc_ == Location::Gpr);
        break;
      case ValType::I64:
        MOZ_ASSERT(loc_ == Location::Gpr64);
        break;
      case ValType::F32:
      case ValType::F64:
        MOZ_ASSERT(loc_ == Location::Fpr);
        break;
      case ValType::Ref:
        MOZ_ASSERT(loc_ == Location::Gpr);
        break;
      case ValType::V128:
        MOZ_ASSERT(loc_ == Location::Fpr);
        break;
    }
#endif
  }

  friend class ABIResultIter;
  ABIResult() {}

 public:
  // Sizes of items in the stack area.
  //
  // The size values come from the implementations of Push() in
  // MacroAssembler-x86-shared.cpp and MacroAssembler-arm-shared.cpp, and from
  // VFPRegister::size() in Architecture-arm.h.
  //
  // On ARM unlike on x86 we push a single for float.

  static constexpr size_t StackSizeOfPtr = sizeof(intptr_t);
  static constexpr size_t StackSizeOfInt32 = StackSizeOfPtr;
  static constexpr size_t StackSizeOfInt64 = sizeof(int64_t);
#if defined(JS_CODEGEN_ARM)
  static constexpr size_t StackSizeOfFloat = sizeof(float);
#else
  static constexpr size_t StackSizeOfFloat = sizeof(double);
#endif
  static constexpr size_t StackSizeOfDouble = sizeof(double);
#ifdef ENABLE_WASM_SIMD
  static constexpr size_t StackSizeOfV128 = sizeof(V128);
#endif

  ABIResult(ValType type, Register gpr)
      : type_(type), loc_(Location::Gpr), gpr_(gpr) {
    validate();
  }
  ABIResult(ValType type, Register64 gpr64)
      : type_(type), loc_(Location::Gpr64), gpr64_(gpr64) {
    validate();
  }
  ABIResult(ValType type, FloatRegister fpr)
      : type_(type), loc_(Location::Fpr), fpr_(fpr) {
    validate();
  }
  ABIResult(ValType type, uint32_t stackOffset)
      : type_(type), loc_(Location::Stack), stackOffset_(stackOffset) {
    validate();
  }

  ValType type() const { return type_; }
  bool onStack() const { return loc_ == Location::Stack; }
  bool inRegister() const { return !onStack(); }
  Register gpr() const {
    MOZ_ASSERT(loc_ == Location::Gpr);
    return gpr_;
  }
  Register64 gpr64() const {
    MOZ_ASSERT(loc_ == Location::Gpr64);
    return gpr64_;
  }
  FloatRegister fpr() const {
    MOZ_ASSERT(loc_ == Location::Fpr);
    return fpr_;
  }
  // Offset from SP.
  uint32_t stackOffset() const {
    MOZ_ASSERT(loc_ == Location::Stack);
    return stackOffset_;
  }
  uint32_t size() const;
};

// Just as WebAssembly functions can take multiple arguments, they can also
// return multiple results.  As with a call, a limited number of results will be
// located in registers, and the rest will be stored in a stack area.  The
// |ABIResultIter| computes result locations, given a |ResultType|.
//
// Recall that a |ResultType| represents a sequence of value types t1..tN,
// indexed from 1 to N.  In principle it doesn't matter how we decide which
// results get to be in registers and which go to the stack.  To better
// harmonize with WebAssembly's abstract stack machine, whose properties are
// taken advantage of by the baseline compiler, our strategy is to start
// allocating result locations in "reverse" order: from result N down to 1.
//
// If a result with index I is in a register, then all results with index J > I
// are also in registers.  If a result I is on the stack, then all results with
// index K < I are also on the stack, farther away from the stack pointer than
// result I.
//
// Currently only a single result is ever stored in a register, though this may
// change in the future on register-rich platforms.
//
// NB: The baseline compiler also uses thie ABI for locations of block
// parameters and return values, within individual WebAssembly functions.

class ABIResultIter {
  ResultType type_;
  uint32_t count_;
  uint32_t index_;
  uint32_t nextStackOffset_;
  enum { Next, Prev } direction_;
  ABIResult cur_;

  void settleRegister(ValType type);
  void settleNext();
  void settlePrev();

 public:
  explicit ABIResultIter(const ResultType& type)
      : type_(type), count_(type.length()) {
    reset();
  }

  void reset() {
    index_ = nextStackOffset_ = 0;
    direction_ = Next;
    if (!done()) {
      settleNext();
    }
  }
  bool done() const { return index_ == count_; }
  uint32_t index() const { return index_; }
  uint32_t count() const { return count_; }
  uint32_t remaining() const { return count_ - index_; }
  void switchToNext() {
    MOZ_ASSERT(direction_ == Prev);
    if (!done() && cur().onStack()) {
      nextStackOffset_ += cur().size();
    }
    index_ = count_ - index_;
    direction_ = Next;
    if (!done()) {
      settleNext();
    }
  }
  void switchToPrev() {
    MOZ_ASSERT(direction_ == Next);
    if (!done() && cur().onStack()) {
      nextStackOffset_ -= cur().size();
    }
    index_ = count_ - index_;
    direction_ = Prev;
    if (!done()) settlePrev();
  }
  void next() {
    MOZ_ASSERT(direction_ == Next);
    MOZ_ASSERT(!done());
    index_++;
    if (!done()) {
      settleNext();
    }
  }
  void prev() {
    MOZ_ASSERT(direction_ == Prev);
    MOZ_ASSERT(!done());
    index_++;
    if (!done()) {
      settlePrev();
    }
  }
  const ABIResult& cur() const {
    MOZ_ASSERT(!done());
    return cur_;
  }

  uint32_t stackBytesConsumedSoFar() const { return nextStackOffset_; }

  static inline bool HasStackResults(const ResultType& type) {
    return type.length() > MaxRegisterResults;
  }

  static uint32_t MeasureStackBytes(const ResultType& type) {
    if (!HasStackResults(type)) {
      return 0;
    }
    ABIResultIter iter(type);
    while (!iter.done()) {
      iter.next();
    }
    return iter.stackBytesConsumedSoFar();
  }
};

extern bool GenerateBuiltinThunk(jit::MacroAssembler& masm,
                                 jit::ABIFunctionType abiType,
                                 ExitReason exitReason, void* funcPtr,
                                 CallableOffsets* offsets);

extern bool GenerateImportFunctions(const ModuleEnvironment& env,
                                    const FuncImportVector& imports,
                                    CompiledCode* code);

extern bool GenerateStubs(const ModuleEnvironment& env,
                          const FuncImportVector& imports,
                          const FuncExportVector& exports, CompiledCode* code);

extern bool GenerateEntryStubs(jit::MacroAssembler& masm,
                               size_t funcExportIndex, const FuncExport& fe,
                               const FuncType& funcType,
                               const Maybe<jit::ImmPtr>& callee, bool isAsmJS,
                               CodeRangeVector* codeRanges);

extern void GenerateTrapExitRegisterOffsets(jit::RegisterOffsets* offsets,
                                            size_t* numWords);

extern bool GenerateProvisionalLazyJitEntryStub(jit::MacroAssembler& masm,
                                                Offsets* offsets);

// A value that is written into the trap exit frame, which is useful for
// cross-checking during garbage collection.
static constexpr uintptr_t TrapExitDummyValue = 1337;

// And its offset, in words, down from the highest-addressed word of the trap
// exit frame.  The value is written into the frame using WasmPush.  In the
// case where WasmPush allocates more than one word, the value will therefore
// be written at the lowest-addressed word.
#ifdef JS_CODEGEN_ARM64
static constexpr size_t TrapExitDummyValueOffsetFromTop = 1;
#else
static constexpr size_t TrapExitDummyValueOffsetFromTop = 0;
#endif

// An argument that will end up on the stack according to the system ABI, to be
// passed to GenerateDirectCallFromJit. Since the direct JIT call creates its
// own frame, it is its responsibility to put stack arguments to their expected
// locations; so the caller of GenerateDirectCallFromJit can put them anywhere.

class JitCallStackArg {
 public:
  enum class Tag {
    Imm32,
    GPR,
    FPU,
    Address,
    Undefined,
  };

 private:
  Tag tag_;
  union U {
    int32_t imm32_;
    jit::Register gpr_;
    jit::FloatRegister fpu_;
    jit::Address addr_;
    U() {}
  } arg;

 public:
  JitCallStackArg() : tag_(Tag::Undefined) {}
  explicit JitCallStackArg(int32_t imm32) : tag_(Tag::Imm32) {
    arg.imm32_ = imm32;
  }
  explicit JitCallStackArg(jit::Register gpr) : tag_(Tag::GPR) {
    arg.gpr_ = gpr;
  }
  explicit JitCallStackArg(jit::FloatRegister fpu) : tag_(Tag::FPU) {
    new (&arg) jit::FloatRegister(fpu);
  }
  explicit JitCallStackArg(const jit::Address& addr) : tag_(Tag::Address) {
    new (&arg) jit::Address(addr);
  }

  Tag tag() const { return tag_; }
  int32_t imm32() const {
    MOZ_ASSERT(tag_ == Tag::Imm32);
    return arg.imm32_;
  }
  jit::Register gpr() const {
    MOZ_ASSERT(tag_ == Tag::GPR);
    return arg.gpr_;
  }
  jit::FloatRegister fpu() const {
    MOZ_ASSERT(tag_ == Tag::FPU);
    return arg.fpu_;
  }
  const jit::Address& addr() const {
    MOZ_ASSERT(tag_ == Tag::Address);
    return arg.addr_;
  }
};

using JitCallStackArgVector = Vector<JitCallStackArg, 4, SystemAllocPolicy>;

// Generates an inline wasm call (during jit compilation) to a specific wasm
// function (as specifed by the given FuncExport).
// This call doesn't go through a wasm entry, but rather creates its own
// inlined exit frame.
// Assumes:
// - all the registers have been preserved by the caller,
// - all arguments passed in registers have been set up at the expected
//   locations,
// - all arguments passed on stack slot are alive as defined by a corresponding
//   JitCallStackArg.

extern void GenerateDirectCallFromJit(jit::MacroAssembler& masm,
                                      const FuncExport& fe,
                                      const Instance& inst,
                                      const JitCallStackArgVector& stackArgs,
                                      jit::Register scratch,
                                      uint32_t* callOffset);

}  // namespace wasm
}  // namespace js

#endif  // wasm_stubs_h

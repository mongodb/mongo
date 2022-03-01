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

#include "wasm/WasmStubs.h"

#include <algorithm>
#include <iterator>

#include "jit/ABIFunctions.h"
#include "jit/JitFrames.h"
#include "jit/JitScript.h"
#include "jit/RegisterAllocator.h"
#include "js/Printf.h"
#include "util/Memory.h"
#include "wasm/WasmCode.h"
#include "wasm/WasmGenerator.h"
#include "wasm/WasmInstance.h"

#include "jit/ABIFunctionList-inl.h"
#include "jit/MacroAssembler-inl.h"

using namespace js;
using namespace js::jit;
using namespace js::wasm;

using MIRTypeVector = Vector<jit::MIRType, 8, SystemAllocPolicy>;
using ABIArgMIRTypeIter = jit::ABIArgIter<MIRTypeVector>;

/*****************************************************************************/
// ABIResultIter implementation

static uint32_t ResultStackSize(ValType type) {
  switch (type.kind()) {
    case ValType::I32:
      return ABIResult::StackSizeOfInt32;
    case ValType::I64:
      return ABIResult::StackSizeOfInt64;
    case ValType::F32:
      return ABIResult::StackSizeOfFloat;
    case ValType::F64:
      return ABIResult::StackSizeOfDouble;
#ifdef ENABLE_WASM_SIMD
    case ValType::V128:
      return ABIResult::StackSizeOfV128;
#endif
    case ValType::Ref:
      return ABIResult::StackSizeOfPtr;
    default:
      MOZ_CRASH("Unexpected result type");
  }
}

uint32_t js::wasm::MIRTypeToABIResultSize(jit::MIRType type) {
  switch (type) {
    case MIRType::Int32:
      return ABIResult::StackSizeOfInt32;
    case MIRType::Int64:
      return ABIResult::StackSizeOfInt64;
    case MIRType::Float32:
      return ABIResult::StackSizeOfFloat;
    case MIRType::Double:
      return ABIResult::StackSizeOfDouble;
#ifdef ENABLE_WASM_SIMD
    case MIRType::Simd128:
      return ABIResult::StackSizeOfV128;
#endif
    case MIRType::Pointer:
    case MIRType::RefOrNull:
      return ABIResult::StackSizeOfPtr;
    default:
      MOZ_CRASH("MIRTypeToABIResultSize - unhandled case");
  }
}

uint32_t ABIResult::size() const { return ResultStackSize(type()); }

void ABIResultIter::settleRegister(ValType type) {
  MOZ_ASSERT(!done());
  MOZ_ASSERT_IF(direction_ == Next, index() < MaxRegisterResults);
  MOZ_ASSERT_IF(direction_ == Prev, index() >= count_ - MaxRegisterResults);
  static_assert(MaxRegisterResults == 1, "expected a single register result");

  switch (type.kind()) {
    case ValType::I32:
      cur_ = ABIResult(type, ReturnReg);
      break;
    case ValType::I64:
      cur_ = ABIResult(type, ReturnReg64);
      break;
    case ValType::F32:
      cur_ = ABIResult(type, ReturnFloat32Reg);
      break;
    case ValType::F64:
      cur_ = ABIResult(type, ReturnDoubleReg);
      break;
    case ValType::Rtt:
    case ValType::Ref:
      cur_ = ABIResult(type, ReturnReg);
      break;
#ifdef ENABLE_WASM_SIMD
    case ValType::V128:
      cur_ = ABIResult(type, ReturnSimd128Reg);
      break;
#endif
    default:
      MOZ_CRASH("Unexpected result type");
  }
}

void ABIResultIter::settleNext() {
  MOZ_ASSERT(direction_ == Next);
  MOZ_ASSERT(!done());

  uint32_t typeIndex = count_ - index_ - 1;
  ValType type = type_[typeIndex];

  if (index_ < MaxRegisterResults) {
    settleRegister(type);
    return;
  }

  cur_ = ABIResult(type, nextStackOffset_);
  nextStackOffset_ += ResultStackSize(type);
}

void ABIResultIter::settlePrev() {
  MOZ_ASSERT(direction_ == Prev);
  MOZ_ASSERT(!done());
  uint32_t typeIndex = index_;
  ValType type = type_[typeIndex];

  if (count_ - index_ - 1 < MaxRegisterResults) {
    settleRegister(type);
    return;
  }

  uint32_t size = ResultStackSize(type);
  MOZ_ASSERT(nextStackOffset_ >= size);
  nextStackOffset_ -= size;
  cur_ = ABIResult(type, nextStackOffset_);
}

#ifdef WASM_CODEGEN_DEBUG
template <class Closure>
static void GenPrint(DebugChannel channel, MacroAssembler& masm,
                     const Maybe<Register>& taken, Closure passArgAndCall) {
  if (!IsCodegenDebugEnabled(channel)) {
    return;
  }

  AllocatableRegisterSet regs(RegisterSet::All());
  LiveRegisterSet save(regs.asLiveSet());
  masm.PushRegsInMask(save);

  if (taken) {
    regs.take(taken.value());
  }
  Register temp = regs.takeAnyGeneral();

  {
    MOZ_ASSERT(MaybeGetJitContext(),
               "codegen debug checks require a jit context");
    masm.setupUnalignedABICall(temp);
    passArgAndCall(IsCompilingWasm(), temp);
  }

  masm.PopRegsInMask(save);
}

static void GenPrintf(DebugChannel channel, MacroAssembler& masm,
                      const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  UniqueChars str = JS_vsmprintf(fmt, ap);
  va_end(ap);

  GenPrint(channel, masm, Nothing(), [&](bool inWasm, Register temp) {
    // If we've gone this far, it means we're actually using the debugging
    // strings. In this case, we leak them! This is only for debugging, and
    // doing the right thing is cumbersome (in Ion, it'd mean add a vec of
    // strings to the IonScript; in wasm, it'd mean add it to the current
    // Module and serialize it properly).
    const char* text = str.release();

    masm.movePtr(ImmPtr((void*)text, ImmPtr::NoCheckToken()), temp);
    masm.passABIArg(temp);
    if (inWasm) {
      masm.callDebugWithABI(SymbolicAddress::PrintText);
    } else {
      using Fn = void (*)(const char* output);
      masm.callWithABI<Fn, PrintText>(MoveOp::GENERAL,
                                      CheckUnsafeCallWithABI::DontCheckOther);
    }
  });
}

static void GenPrintIsize(DebugChannel channel, MacroAssembler& masm,
                          const Register& src) {
  GenPrint(channel, masm, Some(src), [&](bool inWasm, Register _temp) {
    masm.passABIArg(src);
    if (inWasm) {
      masm.callDebugWithABI(SymbolicAddress::PrintI32);
    } else {
      using Fn = void (*)(int32_t val);
      masm.callWithABI<Fn, PrintI32>(MoveOp::GENERAL,
                                     CheckUnsafeCallWithABI::DontCheckOther);
    }
  });
}

static void GenPrintPtr(DebugChannel channel, MacroAssembler& masm,
                        const Register& src) {
  GenPrint(channel, masm, Some(src), [&](bool inWasm, Register _temp) {
    masm.passABIArg(src);
    if (inWasm) {
      masm.callDebugWithABI(SymbolicAddress::PrintPtr);
    } else {
      using Fn = void (*)(uint8_t * val);
      masm.callWithABI<Fn, PrintPtr>(MoveOp::GENERAL,
                                     CheckUnsafeCallWithABI::DontCheckOther);
    }
  });
}

static void GenPrintI64(DebugChannel channel, MacroAssembler& masm,
                        const Register64& src) {
#  if JS_BITS_PER_WORD == 64
  GenPrintf(channel, masm, "i64 ");
  GenPrintIsize(channel, masm, src.reg);
#  else
  GenPrintf(channel, masm, "i64(");
  GenPrintIsize(channel, masm, src.low);
  GenPrintIsize(channel, masm, src.high);
  GenPrintf(channel, masm, ") ");
#  endif
}

static void GenPrintF32(DebugChannel channel, MacroAssembler& masm,
                        const FloatRegister& src) {
  GenPrint(channel, masm, Nothing(), [&](bool inWasm, Register temp) {
    masm.passABIArg(src, MoveOp::FLOAT32);
    if (inWasm) {
      masm.callDebugWithABI(SymbolicAddress::PrintF32);
    } else {
      using Fn = void (*)(float val);
      masm.callWithABI<Fn, PrintF32>(MoveOp::GENERAL,
                                     CheckUnsafeCallWithABI::DontCheckOther);
    }
  });
}

static void GenPrintF64(DebugChannel channel, MacroAssembler& masm,
                        const FloatRegister& src) {
  GenPrint(channel, masm, Nothing(), [&](bool inWasm, Register temp) {
    masm.passABIArg(src, MoveOp::DOUBLE);
    if (inWasm) {
      masm.callDebugWithABI(SymbolicAddress::PrintF64);
    } else {
      using Fn = void (*)(double val);
      masm.callWithABI<Fn, PrintF64>(MoveOp::GENERAL,
                                     CheckUnsafeCallWithABI::DontCheckOther);
    }
  });
}

#  ifdef ENABLE_WASM_SIMD
static void GenPrintV128(DebugChannel channel, MacroAssembler& masm,
                         const FloatRegister& src) {
  // TODO: We might try to do something meaningful here once SIMD data are
  // aligned and hence C++-ABI compliant.  For now, just make ourselves visible.
  GenPrintf(channel, masm, "v128");
}
#  endif
#else
static void GenPrintf(DebugChannel channel, MacroAssembler& masm,
                      const char* fmt, ...) {}
static void GenPrintIsize(DebugChannel channel, MacroAssembler& masm,
                          const Register& src) {}
static void GenPrintPtr(DebugChannel channel, MacroAssembler& masm,
                        const Register& src) {}
static void GenPrintI64(DebugChannel channel, MacroAssembler& masm,
                        const Register64& src) {}
static void GenPrintF32(DebugChannel channel, MacroAssembler& masm,
                        const FloatRegister& src) {}
static void GenPrintF64(DebugChannel channel, MacroAssembler& masm,
                        const FloatRegister& src) {}
#  ifdef ENABLE_WASM_SIMD
static void GenPrintV128(DebugChannel channel, MacroAssembler& masm,
                         const FloatRegister& src) {}
#  endif
#endif

static bool FinishOffsets(MacroAssembler& masm, Offsets* offsets) {
  // On old ARM hardware, constant pools could be inserted and they need to
  // be flushed before considering the size of the masm.
  masm.flushBuffer();
  offsets->end = masm.size();
  return !masm.oom();
}

static void AssertStackAlignment(MacroAssembler& masm, uint32_t alignment,
                                 uint32_t addBeforeAssert = 0) {
  MOZ_ASSERT(
      (sizeof(Frame) + masm.framePushed() + addBeforeAssert) % alignment == 0);
  masm.assertStackAlignment(alignment, addBeforeAssert);
}

template <class VectorT, template <class VecT> class ABIArgIterT>
static unsigned StackArgBytesHelper(const VectorT& args) {
  ABIArgIterT<VectorT> iter(args);
  while (!iter.done()) {
    iter++;
  }
  return iter.stackBytesConsumedSoFar();
}

template <class VectorT>
static unsigned StackArgBytesForNativeABI(const VectorT& args) {
  return StackArgBytesHelper<VectorT, ABIArgIter>(args);
}

template <class VectorT>
static unsigned StackArgBytesForWasmABI(const VectorT& args) {
  return StackArgBytesHelper<VectorT, WasmABIArgIter>(args);
}

static unsigned StackArgBytesForWasmABI(const FuncType& funcType) {
  ArgTypeVector args(funcType);
  return StackArgBytesForWasmABI(args);
}

static void Move64(MacroAssembler& masm, const Address& src,
                   const Address& dest, Register scratch) {
#if JS_BITS_PER_WORD == 32
  masm.load32(LowWord(src), scratch);
  masm.store32(scratch, LowWord(dest));
  masm.load32(HighWord(src), scratch);
  masm.store32(scratch, HighWord(dest));
#else
  Register64 scratch64(scratch);
  masm.load64(src, scratch64);
  masm.store64(scratch64, dest);
#endif
}

static void SetupABIArguments(MacroAssembler& masm, const FuncExport& fe,
                              Register argv, Register scratch) {
  // Copy parameters out of argv and into the registers/stack-slots specified by
  // the wasm ABI.
  //
  // SetupABIArguments are only used for C++ -> wasm calls through callExport(),
  // and V128 and Ref types (other than externref) are not currently allowed.
  ArgTypeVector args(fe.funcType());
  for (WasmABIArgIter iter(args); !iter.done(); iter++) {
    unsigned argOffset = iter.index() * sizeof(ExportArg);
    Address src(argv, argOffset);
    MIRType type = iter.mirType();
    switch (iter->kind()) {
      case ABIArg::GPR:
        if (type == MIRType::Int32) {
          masm.load32(src, iter->gpr());
        } else if (type == MIRType::Int64) {
          masm.load64(src, iter->gpr64());
        } else if (type == MIRType::RefOrNull) {
          masm.loadPtr(src, iter->gpr());
        } else if (type == MIRType::StackResults) {
          MOZ_ASSERT(args.isSyntheticStackResultPointerArg(iter.index()));
          masm.loadPtr(src, iter->gpr());
        } else {
          MOZ_CRASH("unknown GPR type");
        }
        break;
#ifdef JS_CODEGEN_REGISTER_PAIR
      case ABIArg::GPR_PAIR:
        if (type == MIRType::Int64) {
          masm.load64(src, iter->gpr64());
        } else {
          MOZ_CRASH("wasm uses hardfp for function calls.");
        }
        break;
#endif
      case ABIArg::FPU: {
        static_assert(sizeof(ExportArg) >= jit::Simd128DataSize,
                      "ExportArg must be big enough to store SIMD values");
        switch (type) {
          case MIRType::Double:
            masm.loadDouble(src, iter->fpu());
            break;
          case MIRType::Float32:
            masm.loadFloat32(src, iter->fpu());
            break;
          case MIRType::Simd128:
#ifdef ENABLE_WASM_SIMD
            // This is only used by the testing invoke path,
            // wasmLosslessInvoke, and is guarded against in normal JS-API
            // call paths.
            masm.loadUnalignedSimd128(src, iter->fpu());
            break;
#else
            MOZ_CRASH("V128 not supported in SetupABIArguments");
#endif
          default:
            MOZ_CRASH("unexpected FPU type");
            break;
        }
        break;
      }
      case ABIArg::Stack:
        switch (type) {
          case MIRType::Int32:
            masm.load32(src, scratch);
            masm.storePtr(scratch, Address(masm.getStackPointer(),
                                           iter->offsetFromArgBase()));
            break;
          case MIRType::Int64: {
            RegisterOrSP sp = masm.getStackPointer();
            Move64(masm, src, Address(sp, iter->offsetFromArgBase()), scratch);
            break;
          }
          case MIRType::RefOrNull:
            masm.loadPtr(src, scratch);
            masm.storePtr(scratch, Address(masm.getStackPointer(),
                                           iter->offsetFromArgBase()));
            break;
          case MIRType::Double: {
            ScratchDoubleScope fpscratch(masm);
            masm.loadDouble(src, fpscratch);
            masm.storeDouble(fpscratch, Address(masm.getStackPointer(),
                                                iter->offsetFromArgBase()));
            break;
          }
          case MIRType::Float32: {
            ScratchFloat32Scope fpscratch(masm);
            masm.loadFloat32(src, fpscratch);
            masm.storeFloat32(fpscratch, Address(masm.getStackPointer(),
                                                 iter->offsetFromArgBase()));
            break;
          }
          case MIRType::Simd128: {
#ifdef ENABLE_WASM_SIMD
            // This is only used by the testing invoke path,
            // wasmLosslessInvoke, and is guarded against in normal JS-API
            // call paths.
            ScratchSimd128Scope fpscratch(masm);
            masm.loadUnalignedSimd128(src, fpscratch);
            masm.storeUnalignedSimd128(
                fpscratch,
                Address(masm.getStackPointer(), iter->offsetFromArgBase()));
            break;
#else
            MOZ_CRASH("V128 not supported in SetupABIArguments");
#endif
          }
          case MIRType::StackResults: {
            MOZ_ASSERT(args.isSyntheticStackResultPointerArg(iter.index()));
            masm.loadPtr(src, scratch);
            masm.storePtr(scratch, Address(masm.getStackPointer(),
                                           iter->offsetFromArgBase()));
            break;
          }
          default:
            MOZ_CRASH("unexpected stack arg type");
        }
        break;
      case ABIArg::Uninitialized:
        MOZ_CRASH("Uninitialized ABIArg kind");
    }
  }
}

static void StoreRegisterResult(MacroAssembler& masm, const FuncExport& fe,
                                Register loc) {
  ResultType results = ResultType::Vector(fe.funcType().results());
  DebugOnly<bool> sawRegisterResult = false;
  for (ABIResultIter iter(results); !iter.done(); iter.next()) {
    const ABIResult& result = iter.cur();
    if (result.inRegister()) {
      MOZ_ASSERT(!sawRegisterResult);
      sawRegisterResult = true;
      switch (result.type().kind()) {
        case ValType::I32:
          masm.store32(result.gpr(), Address(loc, 0));
          break;
        case ValType::I64:
          masm.store64(result.gpr64(), Address(loc, 0));
          break;
        case ValType::V128:
#ifdef ENABLE_WASM_SIMD
          masm.storeUnalignedSimd128(result.fpr(), Address(loc, 0));
          break;
#else
          MOZ_CRASH("V128 not supported in StoreABIReturn");
#endif
        case ValType::F32:
          masm.canonicalizeFloat(result.fpr());
          masm.storeFloat32(result.fpr(), Address(loc, 0));
          break;
        case ValType::F64:
          masm.canonicalizeDouble(result.fpr());
          masm.storeDouble(result.fpr(), Address(loc, 0));
          break;
        case ValType::Rtt:
        case ValType::Ref:
          masm.storePtr(result.gpr(), Address(loc, 0));
          break;
      }
    }
  }
  MOZ_ASSERT(sawRegisterResult == (results.length() > 0));
}

#if defined(JS_CODEGEN_ARM)
// The ARM system ABI also includes d15 & s31 in the non volatile float
// registers. Also exclude lr (a.k.a. r14) as we preserve it manually.
static const LiveRegisterSet NonVolatileRegs = LiveRegisterSet(
    GeneralRegisterSet(Registers::NonVolatileMask &
                       ~(Registers::SetType(1) << Registers::lr)),
    FloatRegisterSet(FloatRegisters::NonVolatileMask |
                     (FloatRegisters::SetType(1) << FloatRegisters::d15) |
                     (FloatRegisters::SetType(1) << FloatRegisters::s31)));
#elif defined(JS_CODEGEN_ARM64)
// Exclude the Link Register (x30) because it is preserved manually.
//
// Include x16 (scratch) to make a 16-byte aligned amount of integer registers.
// Include d31 (scratch) to make a 16-byte aligned amount of floating registers.
static const LiveRegisterSet NonVolatileRegs = LiveRegisterSet(
    GeneralRegisterSet((Registers::NonVolatileMask &
                        ~(Registers::SetType(1) << Registers::lr)) |
                       (Registers::SetType(1) << Registers::x16)),
    FloatRegisterSet(FloatRegisters::NonVolatileMask |
                     FloatRegisters::NonAllocatableMask));
#else
static const LiveRegisterSet NonVolatileRegs =
    LiveRegisterSet(GeneralRegisterSet(Registers::NonVolatileMask),
                    FloatRegisterSet(FloatRegisters::NonVolatileMask));
#endif

static const unsigned NumExtraPushed = 2;  // tls and argv

#ifdef JS_CODEGEN_ARM64
static const unsigned WasmPushSize = 16;
#else
static const unsigned WasmPushSize = sizeof(void*);
#endif

static void AssertExpectedSP(MacroAssembler& masm) {
#ifdef JS_CODEGEN_ARM64
  MOZ_ASSERT(sp.Is(masm.GetStackPointer64()));
#  ifdef DEBUG
  // Since we're asserting that SP is the currently active stack pointer,
  // let's also in effect assert that PSP is dead -- by setting it to 1, so as
  // to cause to cause any attempts to use it to segfault in an easily
  // identifiable way.
  masm.asVIXL().Mov(PseudoStackPointer64, 1);
#  endif
#endif
}

template <class Operand>
static void WasmPush(MacroAssembler& masm, const Operand& op) {
#ifdef JS_CODEGEN_ARM64
  // Allocate a pad word so that SP can remain properly aligned.  |op| will be
  // written at the lower-addressed of the two words pushed here.
  masm.reserveStack(WasmPushSize);
  masm.storePtr(op, Address(masm.getStackPointer(), 0));
#else
  masm.Push(op);
#endif
}

static void WasmPop(MacroAssembler& masm, Register r) {
#ifdef JS_CODEGEN_ARM64
  // Also pop the pad word allocated by WasmPush.
  masm.loadPtr(Address(masm.getStackPointer(), 0), r);
  masm.freeStack(WasmPushSize);
#else
  masm.Pop(r);
#endif
}

static void MoveSPForJitABI(MacroAssembler& masm) {
#ifdef JS_CODEGEN_ARM64
  masm.moveStackPtrTo(PseudoStackPointer);
#endif
}

static void CallFuncExport(MacroAssembler& masm, const FuncExport& fe,
                           const Maybe<ImmPtr>& funcPtr) {
  MOZ_ASSERT(fe.hasEagerStubs() == !funcPtr);
  MoveSPForJitABI(masm);
  if (funcPtr) {
    masm.call(*funcPtr);
  } else {
    masm.call(CallSiteDesc(CallSiteDesc::Func), fe.funcIndex());
  }
}

STATIC_ASSERT_ANYREF_IS_JSOBJECT;  // Strings are currently boxed

// Unboxing is branchy and contorted because of Spectre mitigations - we don't
// have enough scratch registers.  Were it not for the spectre mitigations in
// branchTestObjClass, the branch nest below would be restructured significantly
// by inverting branches and using fewer registers.

// Unbox an anyref in src (clobbering src in the process) and then re-box it as
// a Value in *dst.  See the definition of AnyRef for a discussion of pointer
// representation.
static void UnboxAnyrefIntoValue(MacroAssembler& masm, Register tls,
                                 Register src, const Address& dst,
                                 Register scratch) {
  MOZ_ASSERT(src != scratch);

  // Not actually the value we're passing, but we've no way of
  // decoding anything better.
  GenPrintPtr(DebugChannel::Import, masm, src);

  Label notNull, mustUnbox, done;
  masm.branchTestPtr(Assembler::NonZero, src, src, &notNull);
  masm.storeValue(NullValue(), dst);
  masm.jump(&done);

  masm.bind(&notNull);
  // The type test will clear src if the test fails, so store early.
  masm.storeValue(JSVAL_TYPE_OBJECT, src, dst);
  // Spectre mitigations: see comment above about efficiency.
  masm.branchTestObjClass(Assembler::Equal, src,
                          Address(tls, offsetof(TlsData, valueBoxClass)),
                          scratch, src, &mustUnbox);
  masm.jump(&done);

  masm.bind(&mustUnbox);
  Move64(masm, Address(src, WasmValueBox::offsetOfValue()), dst, scratch);

  masm.bind(&done);
}

// Unbox an anyref in src and then re-box it as a Value in dst.
// See the definition of AnyRef for a discussion of pointer representation.
static void UnboxAnyrefIntoValueReg(MacroAssembler& masm, Register tls,
                                    Register src, ValueOperand dst,
                                    Register scratch) {
  MOZ_ASSERT(src != scratch);
#if JS_BITS_PER_WORD == 32
  MOZ_ASSERT(dst.typeReg() != scratch);
  MOZ_ASSERT(dst.payloadReg() != scratch);
#else
  MOZ_ASSERT(dst.valueReg() != scratch);
#endif

  // Not actually the value we're passing, but we've no way of
  // decoding anything better.
  GenPrintPtr(DebugChannel::Import, masm, src);

  Label notNull, mustUnbox, done;
  masm.branchTestPtr(Assembler::NonZero, src, src, &notNull);
  masm.moveValue(NullValue(), dst);
  masm.jump(&done);

  masm.bind(&notNull);
  // The type test will clear src if the test fails, so store early.
  masm.moveValue(TypedOrValueRegister(MIRType::Object, AnyRegister(src)), dst);
  // Spectre mitigations: see comment above about efficiency.
  masm.branchTestObjClass(Assembler::Equal, src,
                          Address(tls, offsetof(TlsData, valueBoxClass)),
                          scratch, src, &mustUnbox);
  masm.jump(&done);

  masm.bind(&mustUnbox);
  masm.loadValue(Address(src, WasmValueBox::offsetOfValue()), dst);

  masm.bind(&done);
}

// Box the Value in src as an anyref in dest.  src and dest must not overlap.
// See the definition of AnyRef for a discussion of pointer representation.
static void BoxValueIntoAnyref(MacroAssembler& masm, ValueOperand src,
                               Register dest, Label* oolConvert) {
  Label nullValue, objectValue, done;
  {
    ScratchTagScope tag(masm, src);
    masm.splitTagForTest(src, tag);
    masm.branchTestObject(Assembler::Equal, tag, &objectValue);
    masm.branchTestNull(Assembler::Equal, tag, &nullValue);
    masm.jump(oolConvert);
  }

  masm.bind(&nullValue);
  masm.xorPtr(dest, dest);
  masm.jump(&done);

  masm.bind(&objectValue);
  masm.unboxObject(src, dest);

  masm.bind(&done);
}

// Generate a stub that enters wasm from a C++ caller via the native ABI. The
// signature of the entry point is Module::ExportFuncPtr. The exported wasm
// function has an ABI derived from its specific signature, so this function
// must map from the ABI of ExportFuncPtr to the export's signature's ABI.
static bool GenerateInterpEntry(MacroAssembler& masm, const FuncExport& fe,
                                const Maybe<ImmPtr>& funcPtr,
                                Offsets* offsets) {
  AssertExpectedSP(masm);
  masm.haltingAlign(CodeAlignment);

  offsets->begin = masm.currentOffset();

  // Save the return address if it wasn't already saved by the call insn.
#ifdef JS_USE_LINK_REGISTER
#  if defined(JS_CODEGEN_ARM) || defined(JS_CODEGEN_MIPS32) || \
      defined(JS_CODEGEN_MIPS64)
  masm.pushReturnAddress();
#  elif defined(JS_CODEGEN_ARM64)
  // WasmPush updates framePushed() unlike pushReturnAddress(), but that's
  // cancelled by the setFramePushed() below.
  WasmPush(masm, lr);
#  else
  MOZ_CRASH("Implement this");
#  endif
#endif

  // Save all caller non-volatile registers before we clobber them here and in
  // the wasm callee (which does not preserve non-volatile registers).
  masm.setFramePushed(0);
  masm.PushRegsInMask(NonVolatileRegs);

  const unsigned nonVolatileRegsPushSize =
      masm.PushRegsInMaskSizeInBytes(NonVolatileRegs);

  MOZ_ASSERT(masm.framePushed() == nonVolatileRegsPushSize);

  // Put the 'argv' argument into a non-argument/return/TLS register so that
  // we can use 'argv' while we fill in the arguments for the wasm callee.
  // Use a second non-argument/return register as temporary scratch.
  Register argv = ABINonArgReturnReg0;
  Register scratch = ABINonArgReturnReg1;

  // Read the arguments of wasm::ExportFuncPtr according to the native ABI.
  // The entry stub's frame is 1 word.
  const unsigned argBase = sizeof(void*) + masm.framePushed();
  ABIArgGenerator abi;
  ABIArg arg;

  // arg 1: ExportArg*
  arg = abi.next(MIRType::Pointer);
  if (arg.kind() == ABIArg::GPR) {
    masm.movePtr(arg.gpr(), argv);
  } else {
    masm.loadPtr(
        Address(masm.getStackPointer(), argBase + arg.offsetFromArgBase()),
        argv);
  }

  // Arg 2: TlsData*
  arg = abi.next(MIRType::Pointer);
  if (arg.kind() == ABIArg::GPR) {
    masm.movePtr(arg.gpr(), WasmTlsReg);
  } else {
    masm.loadPtr(
        Address(masm.getStackPointer(), argBase + arg.offsetFromArgBase()),
        WasmTlsReg);
  }

  WasmPush(masm, WasmTlsReg);

  // Save 'argv' on the stack so that we can recover it after the call.
  WasmPush(masm, argv);

  // Since we're about to dynamically align the stack, reset the frame depth
  // so we can still assert static stack depth balancing.
  const unsigned framePushedBeforeAlign =
      nonVolatileRegsPushSize + NumExtraPushed * WasmPushSize;

  MOZ_ASSERT(masm.framePushed() == framePushedBeforeAlign);
  masm.setFramePushed(0);

  // Dynamically align the stack since ABIStackAlignment is not necessarily
  // WasmStackAlignment. Preserve SP so it can be restored after the call.
#ifdef JS_CODEGEN_ARM64
  static_assert(WasmStackAlignment == 16, "ARM64 SP alignment");
#else
  masm.moveStackPtrTo(scratch);
  masm.andToStackPtr(Imm32(~(WasmStackAlignment - 1)));
  masm.Push(scratch);
#endif

  // Reserve stack space for the wasm call.
  unsigned argDecrement =
      StackDecrementForCall(WasmStackAlignment, masm.framePushed(),
                            StackArgBytesForWasmABI(fe.funcType()));
  masm.reserveStack(argDecrement);

  // Copy parameters out of argv and into the wasm ABI registers/stack-slots.
  SetupABIArguments(masm, fe, argv, scratch);

  // Setup wasm register state. The nullness of the frame pointer is used to
  // determine whether the call ended in success or failure.
  masm.movePtr(ImmWord(0), FramePointer);
  masm.loadWasmPinnedRegsFromTls();

  masm.storePtr(WasmTlsReg,
                Address(masm.getStackPointer(), WasmCalleeTLSOffsetBeforeCall));

  // Call into the real function. Note that, due to the throw stub, fp, tls
  // and pinned registers may be clobbered.
  masm.assertStackAlignment(WasmStackAlignment);
  CallFuncExport(masm, fe, funcPtr);
  masm.assertStackAlignment(WasmStackAlignment);

  // Pop the arguments pushed after the dynamic alignment.
  masm.freeStack(argDecrement);

  // Pop the stack pointer to its value right before dynamic alignment.
#ifdef JS_CODEGEN_ARM64
  static_assert(WasmStackAlignment == 16, "ARM64 SP alignment");
#else
  masm.PopStackPtr();
#endif
  MOZ_ASSERT(masm.framePushed() == 0);
  masm.setFramePushed(framePushedBeforeAlign);

  // Recover the 'argv' pointer which was saved before aligning the stack.
  WasmPop(masm, argv);

  WasmPop(masm, WasmTlsReg);

  // Store the register result, if any, in argv[0].
  // No spectre.index_masking is required, as the value leaves ReturnReg.
  StoreRegisterResult(masm, fe, argv);

  // After the ReturnReg is stored into argv[0] but before fp is clobbered by
  // the PopRegsInMask(NonVolatileRegs) below, set the return value based on
  // whether fp is null (which is the case for successful returns) or the
  // FailFP magic value (set by the throw stub);
  Label success, join;
  masm.branchTestPtr(Assembler::Zero, FramePointer, FramePointer, &success);
#ifdef DEBUG
  Label ok;
  masm.branchPtr(Assembler::Equal, FramePointer, Imm32(FailFP), &ok);
  masm.breakpoint();
  masm.bind(&ok);
#endif
  masm.move32(Imm32(false), ReturnReg);
  masm.jump(&join);
  masm.bind(&success);
  masm.move32(Imm32(true), ReturnReg);
  masm.bind(&join);

  // Restore clobbered non-volatile registers of the caller.
  masm.PopRegsInMask(NonVolatileRegs);
  MOZ_ASSERT(masm.framePushed() == 0);

#if defined(JS_CODEGEN_ARM64)
  masm.setFramePushed(WasmPushSize);
  WasmPop(masm, lr);
  masm.abiret();
#else
  masm.ret();
#endif

  return FinishOffsets(masm, offsets);
}

#ifdef JS_PUNBOX64
static const ValueOperand ScratchValIonEntry = ValueOperand(ABINonArgReg0);
#else
static const ValueOperand ScratchValIonEntry =
    ValueOperand(ABINonArgReg0, ABINonArgReg1);
#endif
static const Register ScratchIonEntry = ABINonArgReg2;

static void CallSymbolicAddress(MacroAssembler& masm, bool isAbsolute,
                                SymbolicAddress sym) {
  if (isAbsolute) {
    masm.call(ImmPtr(SymbolicAddressTarget(sym), ImmPtr::NoCheckToken()));
  } else {
    masm.call(sym);
  }
}

// Load instance's TLS from the callee.
static void GenerateJitEntryLoadTls(MacroAssembler& masm, unsigned frameSize) {
  AssertExpectedSP(masm);

  // ScratchIonEntry := callee => JSFunction*
  unsigned offset = frameSize + JitFrameLayout::offsetOfCalleeToken();
  masm.loadFunctionFromCalleeToken(Address(masm.getStackPointer(), offset),
                                   ScratchIonEntry);

  // ScratchIonEntry := callee->getExtendedSlot(WASM_TLSDATA_SLOT)->toPrivate()
  //                 => TlsData*
  offset = FunctionExtended::offsetOfExtendedSlot(
      FunctionExtended::WASM_TLSDATA_SLOT);
  masm.loadPrivate(Address(ScratchIonEntry, offset), WasmTlsReg);
}

// Creates a JS fake exit frame for wasm, so the frame iterators just use
// JSJit frame iteration.
static void GenerateJitEntryThrow(MacroAssembler& masm, unsigned frameSize) {
  AssertExpectedSP(masm);

  MOZ_ASSERT(masm.framePushed() == frameSize);

  GenerateJitEntryLoadTls(masm, frameSize);

  masm.freeStack(frameSize);
  MoveSPForJitABI(masm);

  masm.loadPtr(Address(WasmTlsReg, offsetof(TlsData, cx)), ScratchIonEntry);
  masm.enterFakeExitFrameForWasm(ScratchIonEntry, ScratchIonEntry,
                                 ExitFrameType::WasmGenericJitEntry);

  masm.loadPtr(Address(WasmTlsReg, offsetof(TlsData, instance)),
               ScratchIonEntry);
  masm.loadPtr(
      Address(ScratchIonEntry, Instance::offsetOfJSJitExceptionHandler()),
      ScratchIonEntry);
  masm.jump(ScratchIonEntry);
}

// Helper function for allocating a BigInt and initializing it from an I64
// in GenerateJitEntry and GenerateImportInterpExit. The return result is
// written to scratch.
static void GenerateBigIntInitialization(MacroAssembler& masm,
                                         unsigned bytesPushedByPrologue,
                                         Register64 input, Register scratch,
                                         const FuncExport* fe, Label* fail) {
#if JS_BITS_PER_WORD == 32
  MOZ_ASSERT(input.low != scratch);
  MOZ_ASSERT(input.high != scratch);
#else
  MOZ_ASSERT(input.reg != scratch);
#endif

  // We need to avoid clobbering other argument registers and the input.
  AllocatableRegisterSet regs(RegisterSet::Volatile());
  LiveRegisterSet save(regs.asLiveSet());
  masm.PushRegsInMask(save);

  unsigned frameSize = StackDecrementForCall(
      ABIStackAlignment, masm.framePushed() + bytesPushedByPrologue, 0);
  masm.reserveStack(frameSize);
  masm.assertStackAlignment(ABIStackAlignment);

  // Needs to use a different call type depending on stub it's used from.
  if (fe) {
    CallSymbolicAddress(masm, !fe->hasEagerStubs(),
                        SymbolicAddress::AllocateBigInt);
  } else {
    masm.call(SymbolicAddress::AllocateBigInt);
  }
  masm.storeCallPointerResult(scratch);

  masm.assertStackAlignment(ABIStackAlignment);
  masm.freeStack(frameSize);

  LiveRegisterSet ignore;
  ignore.add(scratch);
  masm.PopRegsInMaskIgnore(save, ignore);

  masm.branchTest32(Assembler::Zero, scratch, scratch, fail);
  masm.initializeBigInt64(Scalar::BigInt64, scratch, input);
}

// Generate a stub that enters wasm from a jit code caller via the jit ABI.
//
// ARM64 note: This does not save the PseudoStackPointer so we must be sure to
// recompute it on every return path, be it normal return or exception return.
// The JIT code we return to assumes it is correct.

static bool GenerateJitEntry(MacroAssembler& masm, size_t funcExportIndex,
                             const FuncExport& fe, const Maybe<ImmPtr>& funcPtr,
                             Offsets* offsets) {
  AssertExpectedSP(masm);

  RegisterOrSP sp = masm.getStackPointer();

  GenerateJitEntryPrologue(masm, offsets);

  // The jit caller has set up the following stack layout (sp grows to the
  // left):
  // <-- retAddr | descriptor | callee | argc | this | arg1..N

  unsigned normalBytesNeeded = StackArgBytesForWasmABI(fe.funcType());

  MIRTypeVector coerceArgTypes;
  MOZ_ALWAYS_TRUE(coerceArgTypes.append(MIRType::Int32));
  MOZ_ALWAYS_TRUE(coerceArgTypes.append(MIRType::Pointer));
  MOZ_ALWAYS_TRUE(coerceArgTypes.append(MIRType::Pointer));
  unsigned oolBytesNeeded = StackArgBytesForWasmABI(coerceArgTypes);

  unsigned bytesNeeded = std::max(normalBytesNeeded, oolBytesNeeded);

  // Note the jit caller ensures the stack is aligned *after* the call
  // instruction.
  unsigned frameSize = StackDecrementForCall(WasmStackAlignment,
                                             masm.framePushed(), bytesNeeded);

  // Reserve stack space for wasm ABI arguments, set up like this:
  // <-- ABI args | padding
  masm.reserveStack(frameSize);

  GenerateJitEntryLoadTls(masm, frameSize);

  if (fe.funcType().hasUnexposableArgOrRet()) {
    CallSymbolicAddress(masm, !fe.hasEagerStubs(),
                        SymbolicAddress::ReportV128JSCall);
    GenerateJitEntryThrow(masm, frameSize);
    return FinishOffsets(masm, offsets);
  }

  FloatRegister scratchF = ABINonArgDoubleReg;
  Register scratchG = ScratchIonEntry;
  ValueOperand scratchV = ScratchValIonEntry;

  GenPrintf(DebugChannel::Function, masm, "wasm-function[%d]; arguments ",
            fe.funcIndex());

  // We do two loops:
  // - one loop up-front will make sure that all the Value tags fit the
  // expected signature argument types. If at least one inline conversion
  // fails, we just jump to the OOL path which will call into C++. Inline
  // conversions are ordered in the way we expect them to happen the most.
  // - the second loop will unbox the arguments into the right registers.
  Label oolCall;
  for (size_t i = 0; i < fe.funcType().args().length(); i++) {
    unsigned jitArgOffset = frameSize + JitFrameLayout::offsetOfActualArg(i);
    Address jitArgAddr(sp, jitArgOffset);
    masm.loadValue(jitArgAddr, scratchV);

    Label next;
    switch (fe.funcType().args()[i].kind()) {
      case ValType::I32: {
        ScratchTagScope tag(masm, scratchV);
        masm.splitTagForTest(scratchV, tag);

        // For int32 inputs, just skip.
        masm.branchTestInt32(Assembler::Equal, tag, &next);

        // For double inputs, unbox, truncate and store back.
        Label storeBack, notDouble;
        masm.branchTestDouble(Assembler::NotEqual, tag, &notDouble);
        {
          ScratchTagScopeRelease _(&tag);
          masm.unboxDouble(scratchV, scratchF);
          masm.branchTruncateDoubleMaybeModUint32(scratchF, scratchG, &oolCall);
          masm.jump(&storeBack);
        }
        masm.bind(&notDouble);

        // For null or undefined, store 0.
        Label nullOrUndefined, notNullOrUndefined;
        masm.branchTestUndefined(Assembler::Equal, tag, &nullOrUndefined);
        masm.branchTestNull(Assembler::NotEqual, tag, &notNullOrUndefined);
        masm.bind(&nullOrUndefined);
        {
          ScratchTagScopeRelease _(&tag);
          masm.storeValue(Int32Value(0), jitArgAddr);
        }
        masm.jump(&next);
        masm.bind(&notNullOrUndefined);

        // For booleans, store the number value back. Other types (symbol,
        // object, strings) go to the C++ call.
        masm.branchTestBoolean(Assembler::NotEqual, tag, &oolCall);
        masm.unboxBoolean(scratchV, scratchG);
        // fallthrough:

        masm.bind(&storeBack);
        {
          ScratchTagScopeRelease _(&tag);
          masm.storeValue(JSVAL_TYPE_INT32, scratchG, jitArgAddr);
        }
        break;
      }
      case ValType::I64: {
        ScratchTagScope tag(masm, scratchV);
        masm.splitTagForTest(scratchV, tag);

        // For BigInt inputs, just skip. Otherwise go to C++ for other
        // types that require creating a new BigInt or erroring.
        masm.branchTestBigInt(Assembler::NotEqual, tag, &oolCall);
        masm.jump(&next);
        break;
      }
      case ValType::F32:
      case ValType::F64: {
        // Note we can reuse the same code for f32/f64 here, since for the
        // case of f32, the conversion of f64 to f32 will happen in the
        // second loop.
        ScratchTagScope tag(masm, scratchV);
        masm.splitTagForTest(scratchV, tag);

        // For double inputs, just skip.
        masm.branchTestDouble(Assembler::Equal, tag, &next);

        // For int32 inputs, convert and rebox.
        Label storeBack, notInt32;
        {
          ScratchTagScopeRelease _(&tag);
          masm.branchTestInt32(Assembler::NotEqual, scratchV, &notInt32);
          masm.int32ValueToDouble(scratchV, scratchF);
          masm.jump(&storeBack);
        }
        masm.bind(&notInt32);

        // For undefined (missing argument), store NaN.
        Label notUndefined;
        masm.branchTestUndefined(Assembler::NotEqual, tag, &notUndefined);
        {
          ScratchTagScopeRelease _(&tag);
          masm.storeValue(DoubleValue(JS::GenericNaN()), jitArgAddr);
          masm.jump(&next);
        }
        masm.bind(&notUndefined);

        // +null is 0.
        Label notNull;
        masm.branchTestNull(Assembler::NotEqual, tag, &notNull);
        {
          ScratchTagScopeRelease _(&tag);
          masm.storeValue(DoubleValue(0.), jitArgAddr);
        }
        masm.jump(&next);
        masm.bind(&notNull);

        // For booleans, store the number value back. Other types (symbol,
        // object, strings) go to the C++ call.
        masm.branchTestBoolean(Assembler::NotEqual, tag, &oolCall);
        masm.boolValueToDouble(scratchV, scratchF);
        // fallthrough:

        masm.bind(&storeBack);
        {
          ScratchTagScopeRelease _(&tag);
          masm.boxDouble(scratchF, jitArgAddr);
        }
        break;
      }
      case ValType::Ref: {
        switch (fe.funcType().args()[i].refTypeKind()) {
          case RefType::Extern: {
            ScratchTagScope tag(masm, scratchV);
            masm.splitTagForTest(scratchV, tag);

            // For object inputs, we handle object and null inline, everything
            // else requires an actual box and we go out of line to allocate
            // that.
            masm.branchTestObject(Assembler::Equal, tag, &next);
            masm.branchTestNull(Assembler::Equal, tag, &next);
            masm.jump(&oolCall);
            break;
          }
          case RefType::Func:
          case RefType::Eq:
          case RefType::TypeIndex: {
            // Guarded against by temporarilyUnsupportedReftypeForEntry()
            MOZ_CRASH("unexpected argument type when calling from the jit");
          }
        }
        break;
      }
      case ValType::V128: {
        // Guarded against by hasUnexposableArgOrRet()
        MOZ_CRASH("unexpected argument type when calling from the jit");
      }
      default: {
        MOZ_CRASH("unexpected argument type when calling from the jit");
      }
    }
    masm.nopAlign(CodeAlignment);
    masm.bind(&next);
  }

  Label rejoinBeforeCall;
  masm.bind(&rejoinBeforeCall);

  // Convert all the expected values to unboxed values on the stack.
  ArgTypeVector args(fe.funcType());
  for (WasmABIArgIter iter(args); !iter.done(); iter++) {
    unsigned jitArgOffset =
        frameSize + JitFrameLayout::offsetOfActualArg(iter.index());
    Address argv(sp, jitArgOffset);
    bool isStackArg = iter->kind() == ABIArg::Stack;
    switch (iter.mirType()) {
      case MIRType::Int32: {
        Register target = isStackArg ? ScratchIonEntry : iter->gpr();
        masm.unboxInt32(argv, target);
        GenPrintIsize(DebugChannel::Function, masm, target);
        if (isStackArg) {
          masm.storePtr(target, Address(sp, iter->offsetFromArgBase()));
        }
        break;
      }
      case MIRType::Int64: {
        // The coercion has provided a BigInt value by this point, which
        // we need to convert to an I64 here.
        if (isStackArg) {
          Address dst(sp, iter->offsetFromArgBase());
          Register src = scratchV.payloadOrValueReg();
#if JS_BITS_PER_WORD == 64
          Register64 scratch64(scratchG);
#else
          Register64 scratch64(scratchG, ABINonArgReg3);
#endif
          masm.unboxBigInt(argv, src);
          masm.loadBigInt64(src, scratch64);
          GenPrintI64(DebugChannel::Function, masm, scratch64);
          masm.store64(scratch64, dst);
        } else {
          Register src = scratchG;
          Register64 target = iter->gpr64();
          masm.unboxBigInt(argv, src);
          masm.loadBigInt64(src, target);
          GenPrintI64(DebugChannel::Function, masm, target);
        }
        break;
      }
      case MIRType::Float32: {
        FloatRegister target = isStackArg ? ABINonArgDoubleReg : iter->fpu();
        masm.unboxDouble(argv, ABINonArgDoubleReg);
        masm.convertDoubleToFloat32(ABINonArgDoubleReg, target);
        GenPrintF32(DebugChannel::Function, masm, target.asSingle());
        if (isStackArg) {
          masm.storeFloat32(target, Address(sp, iter->offsetFromArgBase()));
        }
        break;
      }
      case MIRType::Double: {
        FloatRegister target = isStackArg ? ABINonArgDoubleReg : iter->fpu();
        masm.unboxDouble(argv, target);
        GenPrintF64(DebugChannel::Function, masm, target);
        if (isStackArg) {
          masm.storeDouble(target, Address(sp, iter->offsetFromArgBase()));
        }
        break;
      }
      case MIRType::RefOrNull: {
        Register target = isStackArg ? ScratchIonEntry : iter->gpr();
        masm.unboxObjectOrNull(argv, target);
        GenPrintPtr(DebugChannel::Function, masm, target);
        if (isStackArg) {
          masm.storePtr(target, Address(sp, iter->offsetFromArgBase()));
        }
        break;
      }
      default: {
        MOZ_CRASH("unexpected input argument when calling from jit");
      }
    }
  }

  GenPrintf(DebugChannel::Function, masm, "\n");

  // Setup wasm register state.
  masm.loadWasmPinnedRegsFromTls();

  masm.storePtr(WasmTlsReg,
                Address(masm.getStackPointer(), WasmCalleeTLSOffsetBeforeCall));

  // Call into the real function. Note that, due to the throw stub, fp, tls
  // and pinned registers may be clobbered.
  masm.assertStackAlignment(WasmStackAlignment);
  CallFuncExport(masm, fe, funcPtr);
  masm.assertStackAlignment(WasmStackAlignment);

  // If fp is equal to the FailFP magic value (set by the throw stub), then
  // report the exception to the JIT caller by jumping into the exception
  // stub; otherwise the FP value is still set to the parent ion frame value.
  Label exception;
  masm.branchPtr(Assembler::Equal, FramePointer, Imm32(FailFP), &exception);

  // Pop arguments.
  masm.freeStack(frameSize);

  GenPrintf(DebugChannel::Function, masm, "wasm-function[%d]; returns ",
            fe.funcIndex());

  // Store the return value in the JSReturnOperand.
  const ValTypeVector& results = fe.funcType().results();
  if (results.length() == 0) {
    GenPrintf(DebugChannel::Function, masm, "void");
    masm.moveValue(UndefinedValue(), JSReturnOperand);
  } else {
    MOZ_ASSERT(results.length() == 1, "multi-value return to JS unimplemented");
    switch (results[0].kind()) {
      case ValType::I32:
        GenPrintIsize(DebugChannel::Function, masm, ReturnReg);
        // No spectre.index_masking is required, as the value is boxed.
        masm.boxNonDouble(JSVAL_TYPE_INT32, ReturnReg, JSReturnOperand);
        break;
      case ValType::F32: {
        masm.canonicalizeFloat(ReturnFloat32Reg);
        masm.convertFloat32ToDouble(ReturnFloat32Reg, ReturnDoubleReg);
        GenPrintF64(DebugChannel::Function, masm, ReturnDoubleReg);
        ScratchDoubleScope fpscratch(masm);
        masm.boxDouble(ReturnDoubleReg, JSReturnOperand, fpscratch);
        break;
      }
      case ValType::F64: {
        masm.canonicalizeDouble(ReturnDoubleReg);
        GenPrintF64(DebugChannel::Function, masm, ReturnDoubleReg);
        ScratchDoubleScope fpscratch(masm);
        masm.boxDouble(ReturnDoubleReg, JSReturnOperand, fpscratch);
        break;
      }
      case ValType::I64: {
        Label fail, done;
        GenPrintI64(DebugChannel::Function, masm, ReturnReg64);
        GenerateBigIntInitialization(masm, 0, ReturnReg64, scratchG, &fe,
                                     &fail);
        masm.boxNonDouble(JSVAL_TYPE_BIGINT, scratchG, JSReturnOperand);
        masm.jump(&done);
        masm.bind(&fail);
        // Fixup the stack for the exception tail so that we can share it.
        masm.reserveStack(frameSize);
        masm.jump(&exception);
        masm.bind(&done);
        // Un-fixup the stack for the benefit of the assertion below.
        masm.setFramePushed(0);
        break;
      }
      case ValType::Rtt:
      case ValType::V128: {
        MOZ_CRASH("unexpected return type when calling from ion to wasm");
      }
      case ValType::Ref: {
        switch (results[0].refTypeKind()) {
          case RefType::Func:
          case RefType::Eq:
            // For FuncRef and EqRef use the AnyRef path for now, since that
            // will work.
          case RefType::Extern:
            // Per comment above, the call may have clobbered the Tls register,
            // so reload since unboxing will need it.
            GenerateJitEntryLoadTls(masm, /* frameSize */ 0);
            UnboxAnyrefIntoValueReg(masm, WasmTlsReg, ReturnReg,
                                    JSReturnOperand, WasmJitEntryReturnScratch);
            break;
          case RefType::TypeIndex:
            MOZ_CRASH("unexpected return type when calling from ion to wasm");
        }
        break;
      }
    }
  }

  GenPrintf(DebugChannel::Function, masm, "\n");

  MOZ_ASSERT(masm.framePushed() == 0);
#ifdef JS_CODEGEN_ARM64
  AssertExpectedSP(masm);
  masm.loadPtr(Address(sp, 0), lr);
  masm.addToStackPtr(Imm32(8));
  // Copy SP into PSP to enforce return-point invariants (SP == PSP).
  // `addToStackPtr` won't sync them because SP is the active pointer here.
  // For the same reason, we can't use initPseudoStackPtr to do the sync, so
  // we have to do it "by hand".  Omitting this causes many tests to segfault.
  masm.moveStackPtrTo(PseudoStackPointer);
  masm.abiret();
#else
  masm.ret();
#endif

  // Generate an OOL call to the C++ conversion path.
  if (fe.funcType().args().length()) {
    masm.bind(&oolCall);
    masm.setFramePushed(frameSize);

    // Baseline and Ion call C++ runtime via BuiltinThunk with wasm abi, so to
    // unify the BuiltinThunk's interface we call it here with wasm abi.
    jit::WasmABIArgIter<MIRTypeVector> argsIter(coerceArgTypes);

    // argument 0: function export index.
    if (argsIter->kind() == ABIArg::GPR) {
      masm.movePtr(ImmWord(funcExportIndex), argsIter->gpr());
    } else {
      masm.storePtr(ImmWord(funcExportIndex),
                    Address(sp, argsIter->offsetFromArgBase()));
    }
    argsIter++;

    // argument 1: tlsData
    if (argsIter->kind() == ABIArg::GPR) {
      masm.movePtr(WasmTlsReg, argsIter->gpr());
    } else {
      masm.storePtr(WasmTlsReg, Address(sp, argsIter->offsetFromArgBase()));
    }
    argsIter++;

    // argument 2: effective address of start of argv
    Address argv(sp, masm.framePushed() + JitFrameLayout::offsetOfActualArg(0));
    if (argsIter->kind() == ABIArg::GPR) {
      masm.computeEffectiveAddress(argv, argsIter->gpr());
    } else {
      masm.computeEffectiveAddress(argv, ScratchIonEntry);
      masm.storePtr(ScratchIonEntry,
                    Address(sp, argsIter->offsetFromArgBase()));
    }
    argsIter++;
    MOZ_ASSERT(argsIter.done());

    masm.assertStackAlignment(ABIStackAlignment);
    CallSymbolicAddress(masm, !fe.hasEagerStubs(),
                        SymbolicAddress::CoerceInPlace_JitEntry);
    masm.assertStackAlignment(ABIStackAlignment);

    // No spectre.index_masking is required, as the return value is used as a
    // bool.
    masm.branchTest32(Assembler::NonZero, ReturnReg, ReturnReg,
                      &rejoinBeforeCall);
  }

  // Prepare to throw: reload WasmTlsReg from the frame.
  masm.bind(&exception);
  masm.setFramePushed(frameSize);
  GenerateJitEntryThrow(masm, frameSize);

  return FinishOffsets(masm, offsets);
}

void wasm::GenerateDirectCallFromJit(MacroAssembler& masm, const FuncExport& fe,
                                     const Instance& inst,
                                     const JitCallStackArgVector& stackArgs,
                                     bool profilingEnabled, Register scratch,
                                     uint32_t* callOffset) {
  MOZ_ASSERT(!IsCompilingWasm());

  size_t framePushedAtStart = masm.framePushed();

  if (profilingEnabled) {
    // FramePointer isn't volatile, manually preserve it because it will be
    // clobbered below.
    masm.Push(FramePointer);
  } else {
#ifdef DEBUG
    // Ensure that the FramePointer is actually Ion-volatile. This might
    // assert when bug 1426134 lands.
    AllocatableRegisterSet set(RegisterSet::All());
    TakeJitRegisters(/* profiling */ false, &set);
    MOZ_ASSERT(set.has(FramePointer),
               "replace the whole if branch by the then body when this fails");
#endif
  }

  // Note, if code here pushes a reference value into the frame for its own
  // purposes (and not just as an argument to the callee) then the frame must be
  // traced in TraceJitExitFrame, see the case there for DirectWasmJitCall.  The
  // callee will trace values that are pushed as arguments, however.

  // Push a special frame descriptor that indicates the frame size so we can
  // directly iterate from the current JIT frame without an extra call.
  *callOffset = masm.buildFakeExitFrame(scratch);
  masm.loadJSContext(scratch);

  masm.moveStackPtrTo(FramePointer);
  masm.enterFakeExitFrame(scratch, scratch, ExitFrameType::DirectWasmJitCall);
  masm.orPtr(Imm32(ExitOrJitEntryFPTag), FramePointer);

  // Move stack arguments to their final locations.
  unsigned bytesNeeded = StackArgBytesForWasmABI(fe.funcType());
  bytesNeeded = StackDecrementForCall(WasmStackAlignment, masm.framePushed(),
                                      bytesNeeded);
  if (bytesNeeded) {
    masm.reserveStack(bytesNeeded);
  }

  GenPrintf(DebugChannel::Function, masm, "wasm-function[%d]; arguments ",
            fe.funcIndex());

  ArgTypeVector args(fe.funcType());
  for (WasmABIArgIter iter(args); !iter.done(); iter++) {
    MOZ_ASSERT_IF(iter->kind() == ABIArg::GPR, iter->gpr() != scratch);
    MOZ_ASSERT_IF(iter->kind() == ABIArg::GPR, iter->gpr() != FramePointer);
    if (iter->kind() != ABIArg::Stack) {
      switch (iter.mirType()) {
        case MIRType::Int32:
          GenPrintIsize(DebugChannel::Function, masm, iter->gpr());
          break;
        case MIRType::Int64:
          GenPrintI64(DebugChannel::Function, masm, iter->gpr64());
          break;
        case MIRType::Float32:
          GenPrintF32(DebugChannel::Function, masm, iter->fpu());
          break;
        case MIRType::Double:
          GenPrintF64(DebugChannel::Function, masm, iter->fpu());
          break;
        case MIRType::RefOrNull:
          GenPrintPtr(DebugChannel::Function, masm, iter->gpr());
          break;
        case MIRType::StackResults:
          MOZ_ASSERT(args.isSyntheticStackResultPointerArg(iter.index()));
          GenPrintPtr(DebugChannel::Function, masm, iter->gpr());
          break;
        default:
          MOZ_CRASH("ion to wasm fast path can only handle i32/f32/f64");
      }
      continue;
    }

    Address dst(masm.getStackPointer(), iter->offsetFromArgBase());

    const JitCallStackArg& stackArg = stackArgs[iter.index()];
    switch (stackArg.tag()) {
      case JitCallStackArg::Tag::Imm32:
        GenPrintf(DebugChannel::Function, masm, "%d ", stackArg.imm32());
        masm.storePtr(ImmWord(stackArg.imm32()), dst);
        break;
      case JitCallStackArg::Tag::GPR:
        MOZ_ASSERT(stackArg.gpr() != scratch);
        MOZ_ASSERT(stackArg.gpr() != FramePointer);
        GenPrintIsize(DebugChannel::Function, masm, stackArg.gpr());
        masm.storePtr(stackArg.gpr(), dst);
        break;
      case JitCallStackArg::Tag::FPU:
        switch (iter.mirType()) {
          case MIRType::Double:
            GenPrintF64(DebugChannel::Function, masm, stackArg.fpu());
            masm.storeDouble(stackArg.fpu(), dst);
            break;
          case MIRType::Float32:
            GenPrintF32(DebugChannel::Function, masm, stackArg.fpu());
            masm.storeFloat32(stackArg.fpu(), dst);
            break;
          default:
            MOZ_CRASH(
                "unexpected MIR type for a float register in wasm fast call");
        }
        break;
      case JitCallStackArg::Tag::Address: {
        // The address offsets were valid *before* we pushed our frame.
        Address src = stackArg.addr();
        src.offset += masm.framePushed() - framePushedAtStart;
        switch (iter.mirType()) {
          case MIRType::Double: {
            ScratchDoubleScope fpscratch(masm);
            GenPrintF64(DebugChannel::Function, masm, fpscratch);
            masm.loadDouble(src, fpscratch);
            masm.storeDouble(fpscratch, dst);
            break;
          }
          case MIRType::Float32: {
            ScratchFloat32Scope fpscratch(masm);
            masm.loadFloat32(src, fpscratch);
            GenPrintF32(DebugChannel::Function, masm, fpscratch);
            masm.storeFloat32(fpscratch, dst);
            break;
          }
          case MIRType::Int32: {
            masm.loadPtr(src, scratch);
            GenPrintIsize(DebugChannel::Function, masm, scratch);
            masm.storePtr(scratch, dst);
            break;
          }
          case MIRType::RefOrNull: {
            masm.loadPtr(src, scratch);
            GenPrintPtr(DebugChannel::Function, masm, scratch);
            masm.storePtr(scratch, dst);
            break;
          }
          case MIRType::StackResults: {
            MOZ_CRASH("multi-value in ion to wasm fast path unimplemented");
          }
          default: {
            MOZ_CRASH("unexpected MIR type for a stack slot in wasm fast call");
          }
        }
        break;
      }
      case JitCallStackArg::Tag::Undefined: {
        MOZ_CRASH("can't happen because of arg.kind() check");
      }
    }
  }

  GenPrintf(DebugChannel::Function, masm, "\n");

  // Load tls; from now on, WasmTlsReg is live.
  masm.movePtr(ImmPtr(inst.tlsData()), WasmTlsReg);
  masm.storePtr(WasmTlsReg,
                Address(masm.getStackPointer(), WasmCalleeTLSOffsetBeforeCall));
  masm.loadWasmPinnedRegsFromTls();

  // Actual call.
  const CodeTier& codeTier = inst.code().codeTier(inst.code().bestTier());
  const MetadataTier& metadata = codeTier.metadata();
  const CodeRange& codeRange = metadata.codeRange(fe);
  void* callee = codeTier.segment().base() + codeRange.funcUncheckedCallEntry();

  masm.assertStackAlignment(WasmStackAlignment);
  MoveSPForJitABI(masm);
  masm.callJit(ImmPtr(callee));
#ifdef JS_CODEGEN_ARM64
  // WASM does not always keep PSP in sync with SP.  So reinitialize it as it
  // might be clobbered either by WASM or by any C++ calls within.
  masm.initPseudoStackPtr();
#endif
  masm.assertStackAlignment(WasmStackAlignment);

  masm.branchPtr(Assembler::Equal, FramePointer, Imm32(wasm::FailFP),
                 masm.exceptionLabel());

  // Store the return value in the appropriate place.
  GenPrintf(DebugChannel::Function, masm, "wasm-function[%d]; returns ",
            fe.funcIndex());
  const ValTypeVector& results = fe.funcType().results();
  if (results.length() == 0) {
    masm.moveValue(UndefinedValue(), JSReturnOperand);
    GenPrintf(DebugChannel::Function, masm, "void");
  } else {
    MOZ_ASSERT(results.length() == 1, "multi-value return to JS unimplemented");
    switch (results[0].kind()) {
      case wasm::ValType::I32:
        // The return value is in ReturnReg, which is what Ion expects.
        GenPrintIsize(DebugChannel::Function, masm, ReturnReg);
#if defined(JS_CODEGEN_X64)
        if (JitOptions.spectreIndexMasking) {
          masm.movl(ReturnReg, ReturnReg);
        }
#endif
        break;
      case wasm::ValType::I64:
        // The return value is in ReturnReg64, which is what Ion expects.
        GenPrintI64(DebugChannel::Function, masm, ReturnReg64);
        break;
      case wasm::ValType::F32:
        masm.canonicalizeFloat(ReturnFloat32Reg);
        GenPrintF32(DebugChannel::Function, masm, ReturnFloat32Reg);
        break;
      case wasm::ValType::F64:
        masm.canonicalizeDouble(ReturnDoubleReg);
        GenPrintF64(DebugChannel::Function, masm, ReturnDoubleReg);
        break;
      case wasm::ValType::Ref:
        switch (results[0].refTypeKind()) {
          case wasm::RefType::Func:
          case wasm::RefType::Eq:
            // For FuncRef and EqRef, use the AnyRef path for now, since that
            // will work.
          case wasm::RefType::Extern:
            // The call to wasm above preserves the WasmTlsReg, we don't need to
            // reload it here.
            UnboxAnyrefIntoValueReg(masm, WasmTlsReg, ReturnReg,
                                    JSReturnOperand, WasmJitEntryReturnScratch);
            break;
          case wasm::RefType::TypeIndex:
            MOZ_CRASH("unexpected return type when calling from ion to wasm");
        }
        break;
      case wasm::ValType::Rtt:
      case wasm::ValType::V128:
        MOZ_CRASH("unexpected return type when calling from ion to wasm");
    }
  }

  GenPrintf(DebugChannel::Function, masm, "\n");

  // Free args + frame descriptor.
  masm.leaveExitFrame(bytesNeeded + ExitFrameLayout::Size());

  // If we pushed it, free FramePointer.
  if (profilingEnabled) {
    masm.Pop(FramePointer);
  }

  MOZ_ASSERT(framePushedAtStart == masm.framePushed());
}

static void StackCopy(MacroAssembler& masm, MIRType type, Register scratch,
                      Address src, Address dst) {
  if (type == MIRType::Int32) {
    masm.load32(src, scratch);
    GenPrintIsize(DebugChannel::Import, masm, scratch);
    masm.store32(scratch, dst);
  } else if (type == MIRType::Int64) {
#if JS_BITS_PER_WORD == 32
    GenPrintf(DebugChannel::Import, masm, "i64(");
    masm.load32(LowWord(src), scratch);
    GenPrintIsize(DebugChannel::Import, masm, scratch);
    masm.store32(scratch, LowWord(dst));
    masm.load32(HighWord(src), scratch);
    GenPrintIsize(DebugChannel::Import, masm, scratch);
    masm.store32(scratch, HighWord(dst));
    GenPrintf(DebugChannel::Import, masm, ") ");
#else
    Register64 scratch64(scratch);
    masm.load64(src, scratch64);
    GenPrintIsize(DebugChannel::Import, masm, scratch);
    masm.store64(scratch64, dst);
#endif
  } else if (type == MIRType::RefOrNull || type == MIRType::Pointer ||
             type == MIRType::StackResults) {
    masm.loadPtr(src, scratch);
    GenPrintPtr(DebugChannel::Import, masm, scratch);
    masm.storePtr(scratch, dst);
  } else if (type == MIRType::Float32) {
    ScratchFloat32Scope fpscratch(masm);
    masm.loadFloat32(src, fpscratch);
    GenPrintF32(DebugChannel::Import, masm, fpscratch);
    masm.storeFloat32(fpscratch, dst);
  } else if (type == MIRType::Double) {
    ScratchDoubleScope fpscratch(masm);
    masm.loadDouble(src, fpscratch);
    GenPrintF64(DebugChannel::Import, masm, fpscratch);
    masm.storeDouble(fpscratch, dst);
#ifdef ENABLE_WASM_SIMD
  } else if (type == MIRType::Simd128) {
    ScratchSimd128Scope fpscratch(masm);
    masm.loadUnalignedSimd128(src, fpscratch);
    GenPrintV128(DebugChannel::Import, masm, fpscratch);
    masm.storeUnalignedSimd128(fpscratch, dst);
#endif
  } else {
    MOZ_CRASH("StackCopy: unexpected type");
  }
}

using ToValue = bool;

// Note, when toValue is true then this may destroy the values in incoming
// argument registers as a result of Spectre mitigation.
static void FillArgumentArrayForExit(
    MacroAssembler& masm, Register tls, unsigned funcImportIndex,
    const FuncType& funcType, unsigned argOffset,
    unsigned offsetFromFPToCallerStackArgs, Register scratch, Register scratch2,
    Register scratch3, ToValue toValue, Label* throwLabel) {
  MOZ_ASSERT(scratch != scratch2);
  MOZ_ASSERT(scratch != scratch3);
  MOZ_ASSERT(scratch2 != scratch3);

  // This loop does not root the values that are being constructed in
  // for the arguments. Allocations that are generated by code either
  // in the loop or called from it should be NoGC allocations.
  GenPrintf(DebugChannel::Import, masm, "wasm-import[%u]; arguments ",
            funcImportIndex);

  ArgTypeVector args(funcType);
  for (ABIArgIter i(args); !i.done(); i++) {
    Address dst(masm.getStackPointer(), argOffset + i.index() * sizeof(Value));

    MIRType type = i.mirType();
    MOZ_ASSERT(args.isSyntheticStackResultPointerArg(i.index()) ==
               (type == MIRType::StackResults));
    switch (i->kind()) {
      case ABIArg::GPR:
        if (type == MIRType::Int32) {
          GenPrintIsize(DebugChannel::Import, masm, i->gpr());
          if (toValue) {
            masm.storeValue(JSVAL_TYPE_INT32, i->gpr(), dst);
          } else {
            masm.store32(i->gpr(), dst);
          }
        } else if (type == MIRType::Int64) {
          GenPrintI64(DebugChannel::Import, masm, i->gpr64());

          if (toValue) {
            GenerateBigIntInitialization(masm, offsetFromFPToCallerStackArgs,
                                         i->gpr64(), scratch, nullptr,
                                         throwLabel);
            masm.storeValue(JSVAL_TYPE_BIGINT, scratch, dst);
          } else {
            masm.store64(i->gpr64(), dst);
          }
        } else if (type == MIRType::RefOrNull) {
          if (toValue) {
            // This works also for FuncRef because it is distinguishable from
            // a boxed AnyRef.
            masm.movePtr(i->gpr(), scratch2);
            UnboxAnyrefIntoValue(masm, tls, scratch2, dst, scratch);
          } else {
            GenPrintPtr(DebugChannel::Import, masm, i->gpr());
            masm.storePtr(i->gpr(), dst);
          }
        } else if (type == MIRType::StackResults) {
          MOZ_ASSERT(!toValue, "Multi-result exit to JIT unimplemented");
          GenPrintPtr(DebugChannel::Import, masm, i->gpr());
          masm.storePtr(i->gpr(), dst);
        } else {
          MOZ_CRASH("FillArgumentArrayForExit, ABIArg::GPR: unexpected type");
        }
        break;
#ifdef JS_CODEGEN_REGISTER_PAIR
      case ABIArg::GPR_PAIR:
        if (type == MIRType::Int64) {
          GenPrintI64(DebugChannel::Import, masm, i->gpr64());

          if (toValue) {
            GenerateBigIntInitialization(masm, offsetFromFPToCallerStackArgs,
                                         i->gpr64(), scratch, nullptr,
                                         throwLabel);
            masm.storeValue(JSVAL_TYPE_BIGINT, scratch, dst);
          } else {
            masm.store64(i->gpr64(), dst);
          }
        } else {
          MOZ_CRASH("wasm uses hardfp for function calls.");
        }
        break;
#endif
      case ABIArg::FPU: {
        FloatRegister srcReg = i->fpu();
        if (type == MIRType::Double) {
          if (toValue) {
            // Preserve the NaN pattern in the input.
            ScratchDoubleScope fpscratch(masm);
            masm.moveDouble(srcReg, fpscratch);
            masm.canonicalizeDouble(fpscratch);
            GenPrintF64(DebugChannel::Import, masm, fpscratch);
            masm.boxDouble(fpscratch, dst);
          } else {
            GenPrintF64(DebugChannel::Import, masm, srcReg);
            masm.storeDouble(srcReg, dst);
          }
        } else if (type == MIRType::Float32) {
          if (toValue) {
            // JS::Values can't store Float32, so convert to a Double.
            ScratchDoubleScope fpscratch(masm);
            masm.convertFloat32ToDouble(srcReg, fpscratch);
            masm.canonicalizeDouble(fpscratch);
            GenPrintF64(DebugChannel::Import, masm, fpscratch);
            masm.boxDouble(fpscratch, dst);
          } else {
            // Preserve the NaN pattern in the input.
            GenPrintF32(DebugChannel::Import, masm, srcReg);
            masm.storeFloat32(srcReg, dst);
          }
        } else if (type == MIRType::Simd128) {
          // The value should never escape; the call will be stopped later as
          // the import is being called.  But we should generate something sane
          // here for the boxed case since a debugger or the stack walker may
          // observe something.
          ScratchDoubleScope dscratch(masm);
          masm.loadConstantDouble(0, dscratch);
          GenPrintF64(DebugChannel::Import, masm, dscratch);
          if (toValue) {
            masm.boxDouble(dscratch, dst);
          } else {
            masm.storeDouble(dscratch, dst);
          }
        } else {
          MOZ_CRASH("Unknown MIRType in wasm exit stub");
        }
        break;
      }
      case ABIArg::Stack: {
        Address src(FramePointer,
                    offsetFromFPToCallerStackArgs + i->offsetFromArgBase());
        if (toValue) {
          if (type == MIRType::Int32) {
            masm.load32(src, scratch);
            GenPrintIsize(DebugChannel::Import, masm, scratch);
            masm.storeValue(JSVAL_TYPE_INT32, scratch, dst);
          } else if (type == MIRType::Int64) {
#if JS_BITS_PER_WORD == 64
            Register64 scratch64(scratch2);
#else
            Register64 scratch64(scratch2, scratch3);
#endif
            masm.load64(src, scratch64);
            GenPrintI64(DebugChannel::Import, masm, scratch64);
            GenerateBigIntInitialization(masm, sizeof(Frame), scratch64,
                                         scratch, nullptr, throwLabel);
            masm.storeValue(JSVAL_TYPE_BIGINT, scratch, dst);
          } else if (type == MIRType::RefOrNull) {
            // This works also for FuncRef because it is distinguishable from a
            // boxed AnyRef.
            masm.loadPtr(src, scratch);
            UnboxAnyrefIntoValue(masm, tls, scratch, dst, scratch2);
          } else if (IsFloatingPointType(type)) {
            ScratchDoubleScope dscratch(masm);
            FloatRegister fscratch = dscratch.asSingle();
            if (type == MIRType::Float32) {
              masm.loadFloat32(src, fscratch);
              masm.convertFloat32ToDouble(fscratch, dscratch);
            } else {
              masm.loadDouble(src, dscratch);
            }
            masm.canonicalizeDouble(dscratch);
            GenPrintF64(DebugChannel::Import, masm, dscratch);
            masm.boxDouble(dscratch, dst);
          } else if (type == MIRType::Simd128) {
            // The value should never escape; the call will be stopped later as
            // the import is being called.  But we should generate something
            // sane here for the boxed case since a debugger or the stack walker
            // may observe something.
            ScratchDoubleScope dscratch(masm);
            masm.loadConstantDouble(0, dscratch);
            GenPrintF64(DebugChannel::Import, masm, dscratch);
            masm.boxDouble(dscratch, dst);
          } else {
            MOZ_CRASH(
                "FillArgumentArrayForExit, ABIArg::Stack: unexpected type");
          }
        } else {
          if (type == MIRType::Simd128) {
            // As above.  StackCopy does not know this trick.
            ScratchDoubleScope dscratch(masm);
            masm.loadConstantDouble(0, dscratch);
            GenPrintF64(DebugChannel::Import, masm, dscratch);
            masm.storeDouble(dscratch, dst);
          } else {
            StackCopy(masm, type, scratch, src, dst);
          }
        }
        break;
      }
      case ABIArg::Uninitialized:
        MOZ_CRASH("Uninitialized ABIArg kind");
    }
  }
  GenPrintf(DebugChannel::Import, masm, "\n");
}

// Generate a wrapper function with the standard intra-wasm call ABI which
// simply calls an import. This wrapper function allows any import to be treated
// like a normal wasm function for the purposes of exports and table calls. In
// particular, the wrapper function provides:
//  - a table entry, so JS imports can be put into tables
//  - normal entries, so that, if the import is re-exported, an entry stub can
//    be generated and called without any special cases
static bool GenerateImportFunction(jit::MacroAssembler& masm,
                                   const FuncImport& fi, TypeIdDesc funcTypeId,
                                   FuncOffsets* offsets) {
  AssertExpectedSP(masm);

  GenerateFunctionPrologue(masm, funcTypeId, Nothing(), offsets);

  MOZ_ASSERT(masm.framePushed() == 0);
  const unsigned sizeOfTlsSlot = sizeof(void*);
  unsigned framePushed = StackDecrementForCall(
      WasmStackAlignment,
      sizeof(Frame),  // pushed by prologue
      StackArgBytesForWasmABI(fi.funcType()) + sizeOfTlsSlot);
  masm.wasmReserveStackChecked(framePushed, BytecodeOffset(0));
  MOZ_ASSERT(masm.framePushed() == framePushed);

  masm.storePtr(WasmTlsReg,
                Address(masm.getStackPointer(), framePushed - sizeOfTlsSlot));

  // The argument register state is already setup by our caller. We just need
  // to be sure not to clobber it before the call.
  Register scratch = ABINonArgReg0;

  // Copy our frame's stack arguments to the callee frame's stack argument.
  unsigned offsetFromFPToCallerStackArgs = sizeof(Frame);
  ArgTypeVector args(fi.funcType());
  for (WasmABIArgIter i(args); !i.done(); i++) {
    if (i->kind() != ABIArg::Stack) {
      continue;
    }

    Address src(FramePointer,
                offsetFromFPToCallerStackArgs + i->offsetFromArgBase());
    Address dst(masm.getStackPointer(), i->offsetFromArgBase());
    GenPrintf(DebugChannel::Import, masm,
              "calling exotic import function with arguments: ");
    StackCopy(masm, i.mirType(), scratch, src, dst);
    GenPrintf(DebugChannel::Import, masm, "\n");
  }

  // Call the import exit stub.
  CallSiteDesc desc(CallSiteDesc::Dynamic);
  MoveSPForJitABI(masm);
  masm.wasmCallImport(desc, CalleeDesc::import(fi.tlsDataOffset()));

  // Restore the TLS register and pinned regs, per wasm function ABI.
  masm.loadPtr(Address(masm.getStackPointer(), framePushed - sizeOfTlsSlot),
               WasmTlsReg);
  masm.loadWasmPinnedRegsFromTls();

  // Restore cx->realm.
  masm.switchToWasmTlsRealm(ABINonArgReturnReg0, ABINonArgReturnReg1);

  GenerateFunctionEpilogue(masm, framePushed, offsets);
  return FinishOffsets(masm, offsets);
}

static const unsigned STUBS_LIFO_DEFAULT_CHUNK_SIZE = 4 * 1024;

bool wasm::GenerateImportFunctions(const ModuleEnvironment& env,
                                   const FuncImportVector& imports,
                                   CompiledCode* code) {
  LifoAlloc lifo(STUBS_LIFO_DEFAULT_CHUNK_SIZE);
  TempAllocator alloc(&lifo);
  WasmMacroAssembler masm(alloc, env);

  for (uint32_t funcIndex = 0; funcIndex < imports.length(); funcIndex++) {
    const FuncImport& fi = imports[funcIndex];

    FuncOffsets offsets;
    if (!GenerateImportFunction(masm, fi, *env.funcs[funcIndex].typeId,
                                &offsets)) {
      return false;
    }
    if (!code->codeRanges.emplaceBack(funcIndex, /* bytecodeOffset = */ 0,
                                      offsets)) {
      return false;
    }
  }

  masm.finish();
  if (masm.oom()) {
    return false;
  }

  return code->swap(masm);
}

// Generate a stub that is called via the internal ABI derived from the
// signature of the import and calls into an appropriate callImport C++
// function, having boxed all the ABI arguments into a homogeneous Value array.
static bool GenerateImportInterpExit(MacroAssembler& masm, const FuncImport& fi,
                                     uint32_t funcImportIndex,
                                     Label* throwLabel,
                                     CallableOffsets* offsets) {
  AssertExpectedSP(masm);
  masm.setFramePushed(0);

  // Argument types for Instance::callImport_*:
  static const MIRType typeArray[] = {MIRType::Pointer,   // Instance*
                                      MIRType::Pointer,   // funcImportIndex
                                      MIRType::Int32,     // argc
                                      MIRType::Pointer};  // argv
  MIRTypeVector invokeArgTypes;
  MOZ_ALWAYS_TRUE(invokeArgTypes.append(typeArray, std::size(typeArray)));

  // At the point of the call, the stack layout shall be (sp grows to the left):
  //  | stack args | padding | argv[] | padding | retaddr | caller stack args |
  // The padding between stack args and argv ensures that argv is aligned. The
  // padding between argv and retaddr ensures that sp is aligned.
  unsigned argOffset =
      AlignBytes(StackArgBytesForNativeABI(invokeArgTypes), sizeof(double));
  // The abiArgCount includes a stack result pointer argument if needed.
  unsigned abiArgCount = ArgTypeVector(fi.funcType()).lengthWithStackResults();
  unsigned argBytes = std::max<size_t>(1, abiArgCount) * sizeof(Value);
  unsigned framePushed =
      StackDecrementForCall(ABIStackAlignment,
                            sizeof(Frame),  // pushed by prologue
                            argOffset + argBytes);

  GenerateExitPrologue(masm, framePushed, ExitReason::Fixed::ImportInterp,
                       offsets);

  // Fill the argument array.
  unsigned offsetFromFPToCallerStackArgs = sizeof(FrameWithTls);
  Register scratch = ABINonArgReturnReg0;
  Register scratch2 = ABINonArgReturnReg1;
  // The scratch3 reg does not need to be non-volatile, but has to be
  // distinct from scratch & scratch2.
  Register scratch3 = ABINonVolatileReg;
  FillArgumentArrayForExit(masm, WasmTlsReg, funcImportIndex, fi.funcType(),
                           argOffset, offsetFromFPToCallerStackArgs, scratch,
                           scratch2, scratch3, ToValue(false), throwLabel);

  // Prepare the arguments for the call to Instance::callImport_*.
  ABIArgMIRTypeIter i(invokeArgTypes);

  // argument 0: Instance*
  Address instancePtr(WasmTlsReg, offsetof(TlsData, instance));
  if (i->kind() == ABIArg::GPR) {
    masm.loadPtr(instancePtr, i->gpr());
  } else {
    masm.loadPtr(instancePtr, scratch);
    masm.storePtr(scratch,
                  Address(masm.getStackPointer(), i->offsetFromArgBase()));
  }
  i++;

  // argument 1: funcImportIndex
  if (i->kind() == ABIArg::GPR) {
    masm.mov(ImmWord(funcImportIndex), i->gpr());
  } else {
    masm.store32(Imm32(funcImportIndex),
                 Address(masm.getStackPointer(), i->offsetFromArgBase()));
  }
  i++;

  // argument 2: argc
  unsigned argc = abiArgCount;
  if (i->kind() == ABIArg::GPR) {
    masm.mov(ImmWord(argc), i->gpr());
  } else {
    masm.store32(Imm32(argc),
                 Address(masm.getStackPointer(), i->offsetFromArgBase()));
  }
  i++;

  // argument 3: argv
  Address argv(masm.getStackPointer(), argOffset);
  if (i->kind() == ABIArg::GPR) {
    masm.computeEffectiveAddress(argv, i->gpr());
  } else {
    masm.computeEffectiveAddress(argv, scratch);
    masm.storePtr(scratch,
                  Address(masm.getStackPointer(), i->offsetFromArgBase()));
  }
  i++;
  MOZ_ASSERT(i.done());

  // Make the call, test whether it succeeded, and extract the return value.
  AssertStackAlignment(masm, ABIStackAlignment);
  masm.call(SymbolicAddress::CallImport_General);
  masm.branchTest32(Assembler::Zero, ReturnReg, ReturnReg, throwLabel);

  ResultType resultType = ResultType::Vector(fi.funcType().results());
  ValType registerResultType;
  for (ABIResultIter iter(resultType); !iter.done(); iter.next()) {
    if (iter.cur().inRegister()) {
      MOZ_ASSERT(!registerResultType.isValid());
      registerResultType = iter.cur().type();
    }
  }
  if (!registerResultType.isValid()) {
    GenPrintf(DebugChannel::Import, masm, "wasm-import[%u]; returns ",
              funcImportIndex);
    GenPrintf(DebugChannel::Import, masm, "void");
  } else {
    switch (registerResultType.kind()) {
      case ValType::I32:
        masm.load32(argv, ReturnReg);
        // No spectre.index_masking is required, as we know the value comes from
        // an i32 load.
        GenPrintf(DebugChannel::Import, masm, "wasm-import[%u]; returns ",
                  funcImportIndex);
        GenPrintIsize(DebugChannel::Import, masm, ReturnReg);
        break;
      case ValType::I64:
        masm.load64(argv, ReturnReg64);
        GenPrintf(DebugChannel::Import, masm, "wasm-import[%u]; returns ",
                  funcImportIndex);
        GenPrintI64(DebugChannel::Import, masm, ReturnReg64);
        break;
      case ValType::Rtt:
      case ValType::V128:
        // Note, CallImport_Rtt/V128 currently always throws, so we should never
        // reach this point.
        masm.breakpoint();
        break;
      case ValType::F32:
        masm.loadFloat32(argv, ReturnFloat32Reg);
        GenPrintf(DebugChannel::Import, masm, "wasm-import[%u]; returns ",
                  funcImportIndex);
        GenPrintF32(DebugChannel::Import, masm, ReturnFloat32Reg);
        break;
      case ValType::F64:
        masm.loadDouble(argv, ReturnDoubleReg);
        GenPrintf(DebugChannel::Import, masm, "wasm-import[%u]; returns ",
                  funcImportIndex);
        GenPrintF64(DebugChannel::Import, masm, ReturnDoubleReg);
        break;
      case ValType::Ref:
        switch (registerResultType.refTypeKind()) {
          case RefType::Func:
            masm.loadPtr(argv, ReturnReg);
            GenPrintf(DebugChannel::Import, masm, "wasm-import[%u]; returns ",
                      funcImportIndex);
            GenPrintPtr(DebugChannel::Import, masm, ReturnReg);
            break;
          case RefType::Extern:
          case RefType::Eq:
            masm.loadPtr(argv, ReturnReg);
            GenPrintf(DebugChannel::Import, masm, "wasm-import[%u]; returns ",
                      funcImportIndex);
            GenPrintPtr(DebugChannel::Import, masm, ReturnReg);
            break;
          case RefType::TypeIndex:
            MOZ_CRASH("No Ref support here yet");
        }
        break;
    }
  }

  GenPrintf(DebugChannel::Import, masm, "\n");

  // The native ABI preserves the TLS, heap and global registers since they
  // are non-volatile.
  MOZ_ASSERT(NonVolatileRegs.has(WasmTlsReg));
#if defined(JS_CODEGEN_X64) || defined(JS_CODEGEN_ARM) ||      \
    defined(JS_CODEGEN_ARM64) || defined(JS_CODEGEN_MIPS32) || \
    defined(JS_CODEGEN_MIPS64)
  MOZ_ASSERT(NonVolatileRegs.has(HeapReg));
#endif

  GenerateExitEpilogue(masm, framePushed, ExitReason::Fixed::ImportInterp,
                       offsets);

  return FinishOffsets(masm, offsets);
}

// Generate a stub that is called via the internal ABI derived from the
// signature of the import and calls into a compatible JIT function,
// having boxed all the ABI arguments into the JIT stack frame layout.
static bool GenerateImportJitExit(MacroAssembler& masm, const FuncImport& fi,
                                  unsigned funcImportIndex, Label* throwLabel,
                                  JitExitOffsets* offsets) {
  AssertExpectedSP(masm);
  masm.setFramePushed(0);

  // JIT calls use the following stack layout (sp grows to the left):
  //   | WasmToJSJitFrameLayout | this | arg1..N | saved Tls |
  // Unlike most ABIs, the JIT ABI requires that sp be JitStackAlignment-
  // aligned *after* pushing the return address.
  static_assert(WasmStackAlignment >= JitStackAlignment, "subsumes");
  const unsigned sizeOfTlsSlot = sizeof(void*);
  const unsigned sizeOfRetAddr = sizeof(void*);
  const unsigned sizeOfPreFrame =
      WasmToJSJitFrameLayout::Size() - sizeOfRetAddr;
  const unsigned sizeOfThisAndArgs =
      (1 + fi.funcType().args().length()) * sizeof(Value);
  const unsigned totalJitFrameBytes =
      sizeOfRetAddr + sizeOfPreFrame + sizeOfThisAndArgs + sizeOfTlsSlot;
  const unsigned jitFramePushed =
      StackDecrementForCall(JitStackAlignment,
                            sizeof(Frame),  // pushed by prologue
                            totalJitFrameBytes) -
      sizeOfRetAddr;
  const unsigned sizeOfThisAndArgsAndPadding = jitFramePushed - sizeOfPreFrame;

  // On ARM64 we must align the SP to a 16-byte boundary.
#ifdef JS_CODEGEN_ARM64
  const unsigned frameAlignExtra = sizeof(void*);
#else
  const unsigned frameAlignExtra = 0;
#endif

  GenerateJitExitPrologue(masm, jitFramePushed + frameAlignExtra, offsets);

  // 1. Descriptor.
  size_t argOffset = frameAlignExtra;
  uint32_t descriptor =
      MakeFrameDescriptor(sizeOfThisAndArgsAndPadding, FrameType::WasmToJSJit,
                          WasmToJSJitFrameLayout::Size());
  masm.storePtr(ImmWord(uintptr_t(descriptor)),
                Address(masm.getStackPointer(), argOffset));
  argOffset += sizeof(size_t);

  // 2. Callee, part 1 -- need the callee register for argument filling, so
  // record offset here and set up callee later.
  size_t calleeArgOffset = argOffset;
  argOffset += sizeof(size_t);

  // 3. Argc.
  unsigned argc = fi.funcType().args().length();
  masm.storePtr(ImmWord(uintptr_t(argc)),
                Address(masm.getStackPointer(), argOffset));
  argOffset += sizeof(size_t);
  MOZ_ASSERT(argOffset == sizeOfPreFrame + frameAlignExtra);

  // 4. |this| value.
  masm.storeValue(UndefinedValue(), Address(masm.getStackPointer(), argOffset));
  argOffset += sizeof(Value);

  // 5. Fill the arguments.
  const uint32_t offsetFromFPToCallerStackArgs = sizeof(FrameWithTls);
  Register scratch = ABINonArgReturnReg1;   // Repeatedly clobbered
  Register scratch2 = ABINonArgReturnReg0;  // Reused as callee below
  // The scratch3 reg does not need to be non-volatile, but has to be
  // distinct from scratch & scratch2.
  Register scratch3 = ABINonVolatileReg;
  FillArgumentArrayForExit(masm, WasmTlsReg, funcImportIndex, fi.funcType(),
                           argOffset, offsetFromFPToCallerStackArgs, scratch,
                           scratch2, scratch3, ToValue(true), throwLabel);
  argOffset += fi.funcType().args().length() * sizeof(Value);
  MOZ_ASSERT(argOffset == sizeOfThisAndArgs + sizeOfPreFrame + frameAlignExtra);

  // Preserve Tls because the JIT callee clobbers it.
  const size_t savedTlsOffset = argOffset;
  masm.storePtr(WasmTlsReg, Address(masm.getStackPointer(), savedTlsOffset));

  // 2. Callee, part 2 -- now that the register is free, set up the callee.
  Register callee = ABINonArgReturnReg0;  // Live until call

  // 2.1. Get JSFunction callee.
  masm.loadWasmGlobalPtr(fi.tlsDataOffset() + offsetof(FuncImportTls, fun),
                         callee);

  // 2.2. Save callee.
  masm.storePtr(callee, Address(masm.getStackPointer(), calleeArgOffset));

  // 6. Check if we need to rectify arguments.
  masm.load16ZeroExtend(Address(callee, JSFunction::offsetOfNargs()), scratch);

  Label rectify;
  masm.branch32(Assembler::Above, scratch, Imm32(fi.funcType().args().length()),
                &rectify);

  // 7. If we haven't rectified arguments, load callee executable entry point.

  masm.loadJitCodeRaw(callee, callee);

  Label rejoinBeforeCall;
  masm.bind(&rejoinBeforeCall);

  AssertStackAlignment(masm, JitStackAlignment,
                       sizeOfRetAddr + frameAlignExtra);
#ifdef JS_CODEGEN_ARM64
  AssertExpectedSP(masm);
  // Conform to JIT ABI.  Note this doesn't update PSP since SP is the active
  // pointer.
  masm.addToStackPtr(Imm32(8));
  // Manually resync PSP.  Omitting this causes eg tests/wasm/import-export.js
  // to segfault.
  masm.moveStackPtrTo(PseudoStackPointer);
#endif
  masm.callJitNoProfiler(callee);
#ifdef JS_CODEGEN_ARM64
  // Conform to platform conventions - align the SP.
  masm.subFromStackPtr(Imm32(8));
#endif

  // Note that there might be a GC thing in the JSReturnOperand now.
  // In all the code paths from here:
  // - either the value is unboxed because it was a primitive and we don't
  //   need to worry about rooting anymore.
  // - or the value needs to be rooted, but nothing can cause a GC between
  //   here and CoerceInPlace, which roots before coercing to a primitive.

  // The JIT callee clobbers all registers, including WasmTlsReg and
  // FramePointer, so restore those here. During this sequence of
  // instructions, FP can't be trusted by the profiling frame iterator.
  offsets->untrustedFPStart = masm.currentOffset();
  AssertStackAlignment(masm, JitStackAlignment,
                       sizeOfRetAddr + frameAlignExtra);

  masm.loadPtr(Address(masm.getStackPointer(), savedTlsOffset), WasmTlsReg);
  masm.moveStackPtrTo(FramePointer);
  masm.addPtr(Imm32(masm.framePushed()), FramePointer);
  offsets->untrustedFPEnd = masm.currentOffset();

  // As explained above, the frame was aligned for the JIT ABI such that
  //   (sp + sizeof(void*)) % JitStackAlignment == 0
  // But now we possibly want to call one of several different C++ functions,
  // so subtract the sizeof(void*) so that sp is aligned for an ABI call.
  static_assert(ABIStackAlignment <= JitStackAlignment, "subsumes");
#ifdef JS_CODEGEN_ARM64
  // We've already allocated the extra space for frame alignment.
  static_assert(sizeOfRetAddr == frameAlignExtra, "ARM64 SP alignment");
#else
  masm.reserveStack(sizeOfRetAddr);
#endif
  unsigned nativeFramePushed = masm.framePushed();
  AssertStackAlignment(masm, ABIStackAlignment);

#ifdef DEBUG
  {
    Label ok;
    masm.branchTestMagic(Assembler::NotEqual, JSReturnOperand, &ok);
    masm.breakpoint();
    masm.bind(&ok);
  }
#endif

  GenPrintf(DebugChannel::Import, masm, "wasm-import[%u]; returns ",
            funcImportIndex);

  Label oolConvert;
  const ValTypeVector& results = fi.funcType().results();
  if (results.length() == 0) {
    GenPrintf(DebugChannel::Import, masm, "void");
  } else {
    MOZ_ASSERT(results.length() == 1, "multi-value return unimplemented");
    switch (results[0].kind()) {
      case ValType::I32:
        // No spectre.index_masking required, as the return value does not come
        // to us in ReturnReg.
        masm.truncateValueToInt32(JSReturnOperand, ReturnDoubleReg, ReturnReg,
                                  &oolConvert);
        GenPrintIsize(DebugChannel::Import, masm, ReturnReg);
        break;
      case ValType::I64:
        // No fastpath for now, go immediately to ool case
        masm.jump(&oolConvert);
        break;
      case ValType::Rtt:
      case ValType::V128:
        // Unreachable as callImport should not call the stub.
        masm.breakpoint();
        break;
      case ValType::F32:
        masm.convertValueToFloat(JSReturnOperand, ReturnFloat32Reg,
                                 &oolConvert);
        GenPrintF32(DebugChannel::Import, masm, ReturnFloat32Reg);
        break;
      case ValType::F64:
        masm.convertValueToDouble(JSReturnOperand, ReturnDoubleReg,
                                  &oolConvert);
        GenPrintF64(DebugChannel::Import, masm, ReturnDoubleReg);
        break;
      case ValType::Ref:
        switch (results[0].refTypeKind()) {
          case RefType::Extern:
            BoxValueIntoAnyref(masm, JSReturnOperand, ReturnReg, &oolConvert);
            GenPrintPtr(DebugChannel::Import, masm, ReturnReg);
            break;
          case RefType::Func:
          case RefType::Eq:
          case RefType::TypeIndex:
            MOZ_CRASH("typed reference returned by import (jit exit) NYI");
        }
        break;
    }
  }

  GenPrintf(DebugChannel::Import, masm, "\n");

  Label done;
  masm.bind(&done);

  GenerateJitExitEpilogue(masm, masm.framePushed(), offsets);

  {
    // Call the arguments rectifier.
    masm.bind(&rectify);
    masm.loadPtr(Address(WasmTlsReg, offsetof(TlsData, instance)), callee);
    masm.loadPtr(Address(callee, Instance::offsetOfJSJitArgsRectifier()),
                 callee);
    masm.jump(&rejoinBeforeCall);
  }

  if (oolConvert.used()) {
    masm.bind(&oolConvert);
    masm.setFramePushed(nativeFramePushed);

    // Coercion calls use the following stack layout (sp grows to the left):
    //   | args | padding | Value argv[1] | padding | exit Frame |
    MIRTypeVector coerceArgTypes;
    MOZ_ALWAYS_TRUE(coerceArgTypes.append(MIRType::Pointer));
    unsigned offsetToCoerceArgv =
        AlignBytes(StackArgBytesForNativeABI(coerceArgTypes), sizeof(Value));
    MOZ_ASSERT(nativeFramePushed >= offsetToCoerceArgv + sizeof(Value));
    AssertStackAlignment(masm, ABIStackAlignment);

    // Store return value into argv[0].
    masm.storeValue(JSReturnOperand,
                    Address(masm.getStackPointer(), offsetToCoerceArgv));

    // From this point, it's safe to reuse the scratch register (which
    // might be part of the JSReturnOperand).

    // The JIT might have clobbered exitFP at this point. Since there's
    // going to be a CoerceInPlace call, pretend we're still doing the JIT
    // call by restoring our tagged exitFP.
    SetExitFP(masm, ExitReason::Fixed::ImportJit, scratch);

    // argument 0: argv
    ABIArgMIRTypeIter i(coerceArgTypes);
    Address argv(masm.getStackPointer(), offsetToCoerceArgv);
    if (i->kind() == ABIArg::GPR) {
      masm.computeEffectiveAddress(argv, i->gpr());
    } else {
      masm.computeEffectiveAddress(argv, scratch);
      masm.storePtr(scratch,
                    Address(masm.getStackPointer(), i->offsetFromArgBase()));
    }
    i++;
    MOZ_ASSERT(i.done());

    // Call coercion function. Note that right after the call, the value of
    // FP is correct because FP is non-volatile in the native ABI.
    AssertStackAlignment(masm, ABIStackAlignment);
    const ValTypeVector& results = fi.funcType().results();
    if (results.length() > 0) {
      // NOTE that once there can be more than one result and we can box some of
      // the results (as we must for AnyRef), pointer and already-boxed results
      // must be rooted while subsequent results are boxed.
      MOZ_ASSERT(results.length() == 1, "multi-value return unimplemented");
      switch (results[0].kind()) {
        case ValType::I32:
          masm.call(SymbolicAddress::CoerceInPlace_ToInt32);
          masm.branchTest32(Assembler::Zero, ReturnReg, ReturnReg, throwLabel);
          masm.unboxInt32(Address(masm.getStackPointer(), offsetToCoerceArgv),
                          ReturnReg);
          // No spectre.index_masking required, as we generate a known-good
          // value in a safe way here.
          break;
        case ValType::I64: {
          masm.call(SymbolicAddress::CoerceInPlace_ToBigInt);
          masm.branchTest32(Assembler::Zero, ReturnReg, ReturnReg, throwLabel);
          Address argv(masm.getStackPointer(), offsetToCoerceArgv);
          masm.unboxBigInt(argv, scratch);
          masm.loadBigInt64(scratch, ReturnReg64);
          break;
        }
        case ValType::F64:
        case ValType::F32:
          masm.call(SymbolicAddress::CoerceInPlace_ToNumber);
          masm.branchTest32(Assembler::Zero, ReturnReg, ReturnReg, throwLabel);
          masm.unboxDouble(Address(masm.getStackPointer(), offsetToCoerceArgv),
                           ReturnDoubleReg);
          if (results[0].kind() == ValType::F32) {
            masm.convertDoubleToFloat32(ReturnDoubleReg, ReturnFloat32Reg);
          }
          break;
        case ValType::Ref:
          switch (results[0].refTypeKind()) {
            case RefType::Extern:
              masm.call(SymbolicAddress::BoxValue_Anyref);
              masm.branchTest32(Assembler::Zero, ReturnReg, ReturnReg,
                                throwLabel);
              break;
            case RefType::Func:
            case RefType::Eq:
            case RefType::TypeIndex:
              MOZ_CRASH("Unsupported convert type");
          }
          break;
        default:
          MOZ_CRASH("Unsupported convert type");
      }
    }

    // Maintain the invariant that exitFP is either unset or not set to a
    // wasm tagged exitFP, per the jit exit contract.
    ClearExitFP(masm, scratch);

    masm.jump(&done);
    masm.setFramePushed(0);
  }

  MOZ_ASSERT(masm.framePushed() == 0);

  return FinishOffsets(masm, offsets);
}

struct ABIFunctionArgs {
  ABIFunctionType abiType;
  size_t len;

  explicit ABIFunctionArgs(ABIFunctionType sig)
      : abiType(ABIFunctionType(sig >> ArgType_Shift)) {
    len = 0;
    uint32_t i = uint32_t(abiType);
    while (i) {
      i = i >> ArgType_Shift;
      len++;
    }
  }

  size_t length() const { return len; }

  MIRType operator[](size_t i) const {
    MOZ_ASSERT(i < len);
    uint32_t abi = uint32_t(abiType);
    while (i--) {
      abi = abi >> ArgType_Shift;
    }
    return ToMIRType(ABIArgType(abi & ArgType_Mask));
  }
};

bool wasm::GenerateBuiltinThunk(MacroAssembler& masm, ABIFunctionType abiType,
                                ExitReason exitReason, void* funcPtr,
                                CallableOffsets* offsets) {
  AssertExpectedSP(masm);
  masm.setFramePushed(0);

  ABIFunctionArgs args(abiType);
  uint32_t framePushed =
      StackDecrementForCall(ABIStackAlignment,
                            sizeof(Frame),  // pushed by prologue
                            StackArgBytesForNativeABI(args));

  GenerateExitPrologue(masm, framePushed, exitReason, offsets);

  // Copy out and convert caller arguments, if needed.
  unsigned offsetFromFPToCallerStackArgs = sizeof(FrameWithTls);
  Register scratch = ABINonArgReturnReg0;
  for (ABIArgIter i(args); !i.done(); i++) {
    if (i->argInRegister()) {
#ifdef JS_CODEGEN_ARM
      // Non hard-fp passes the args values in GPRs.
      if (!UseHardFpABI() && IsFloatingPointType(i.mirType())) {
        FloatRegister input = i->fpu();
        if (i.mirType() == MIRType::Float32) {
          masm.ma_vxfer(input, Register::FromCode(input.id()));
        } else if (i.mirType() == MIRType::Double) {
          uint32_t regId = input.singleOverlay().id();
          masm.ma_vxfer(input, Register::FromCode(regId),
                        Register::FromCode(regId + 1));
        }
      }
#endif
      continue;
    }

    Address src(FramePointer,
                offsetFromFPToCallerStackArgs + i->offsetFromArgBase());
    Address dst(masm.getStackPointer(), i->offsetFromArgBase());
    StackCopy(masm, i.mirType(), scratch, src, dst);
  }

  AssertStackAlignment(masm, ABIStackAlignment);
  MoveSPForJitABI(masm);
  masm.call(ImmPtr(funcPtr, ImmPtr::NoCheckToken()));

#if defined(JS_CODEGEN_X64)
  // No spectre.index_masking is required, as the caller will mask.
#elif defined(JS_CODEGEN_X86)
  // x86 passes the return value on the x87 FP stack.
  Operand op(esp, 0);
  MIRType retType = ToMIRType(ABIArgType(abiType & ArgType_Mask));
  if (retType == MIRType::Float32) {
    masm.fstp32(op);
    masm.loadFloat32(op, ReturnFloat32Reg);
  } else if (retType == MIRType::Double) {
    masm.fstp(op);
    masm.loadDouble(op, ReturnDoubleReg);
  }
#elif defined(JS_CODEGEN_ARM)
  // Non hard-fp passes the return values in GPRs.
  MIRType retType = ToMIRType(ABIArgType(abiType & ArgType_Mask));
  if (!UseHardFpABI() && IsFloatingPointType(retType)) {
    masm.ma_vxfer(r0, r1, d0);
  }
#endif

  GenerateExitEpilogue(masm, framePushed, exitReason, offsets);
  return FinishOffsets(masm, offsets);
}

#if defined(JS_CODEGEN_ARM)
static const LiveRegisterSet RegsToPreserve(
    GeneralRegisterSet(Registers::AllMask &
                       ~((Registers::SetType(1) << Registers::sp) |
                         (Registers::SetType(1) << Registers::pc))),
    FloatRegisterSet(FloatRegisters::AllDoubleMask));
#  ifdef ENABLE_WASM_SIMD
#    error "high lanes of SIMD registers need to be saved too."
#  endif
#elif defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64)
static const LiveRegisterSet RegsToPreserve(
    GeneralRegisterSet(Registers::AllMask &
                       ~((Registers::SetType(1) << Registers::k0) |
                         (Registers::SetType(1) << Registers::k1) |
                         (Registers::SetType(1) << Registers::sp) |
                         (Registers::SetType(1) << Registers::zero))),
    FloatRegisterSet(FloatRegisters::AllDoubleMask));
#  ifdef ENABLE_WASM_SIMD
#    error "high lanes of SIMD registers need to be saved too."
#  endif
#elif defined(JS_CODEGEN_ARM64)
// We assume that traps do not happen while lr is live. This both ensures that
// the size of RegsToPreserve is a multiple of 2 (preserving WasmStackAlignment)
// and gives us a register to clobber in the return path.
static const LiveRegisterSet RegsToPreserve(
    GeneralRegisterSet(Registers::AllMask &
                       ~((Registers::SetType(1) << RealStackPointer.code()) |
                         (Registers::SetType(1) << Registers::lr))),
#  ifdef ENABLE_WASM_SIMD
    FloatRegisterSet(FloatRegisters::AllSimd128Mask));
#  else
    // If SIMD is not enabled, it's pointless to save/restore the upper 64
    // bits of each vector register.
    FloatRegisterSet(FloatRegisters::AllDoubleMask));
#  endif
#elif defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64)
// It's correct to use FloatRegisters::AllMask even when SIMD is not enabled;
// PushRegsInMask strips out the high lanes of the XMM registers in this case,
// while the singles will be stripped as they are aliased by the larger doubles.
static const LiveRegisterSet RegsToPreserve(
    GeneralRegisterSet(Registers::AllMask &
                       ~(Registers::SetType(1) << Registers::StackPointer)),
    FloatRegisterSet(FloatRegisters::AllMask));
#else
static const LiveRegisterSet RegsToPreserve(
    GeneralRegisterSet(0), FloatRegisterSet(FloatRegisters::AllDoubleMask));
#  ifdef ENABLE_WASM_SIMD
#    error "no SIMD support"
#  endif
#endif

// Generate a MachineState which describes the locations of the GPRs as saved
// by GenerateTrapExit.  FP registers are ignored.  Note that the values
// stored in the MachineState are offsets in words downwards from the top of
// the save area.  That is, a higher value implies a lower address.
void wasm::GenerateTrapExitMachineState(MachineState* machine,
                                        size_t* numWords) {
  // This is the number of words pushed by the initial WasmPush().
  *numWords = WasmPushSize / sizeof(void*);
  MOZ_ASSERT(*numWords == TrapExitDummyValueOffsetFromTop + 1);

  // And these correspond to the PushRegsInMask() that immediately follows.
  for (GeneralRegisterBackwardIterator iter(RegsToPreserve.gprs()); iter.more();
       ++iter) {
    machine->setRegisterLocation(*iter,
                                 reinterpret_cast<uintptr_t*>(*numWords));
    (*numWords)++;
  }
}

// Generate a stub which calls WasmReportTrap() and can be executed by having
// the signal handler redirect PC from any trapping instruction.
static bool GenerateTrapExit(MacroAssembler& masm, Label* throwLabel,
                             Offsets* offsets) {
  AssertExpectedSP(masm);
  masm.haltingAlign(CodeAlignment);

  masm.setFramePushed(0);

  offsets->begin = masm.currentOffset();

  // Traps can only happen at well-defined program points. However, since
  // traps may resume and the optimal assumption for the surrounding code is
  // that registers are not clobbered, we need to preserve all registers in
  // the trap exit. One simplifying assumption is that flags may be clobbered.
  // Push a dummy word to use as return address below.
  WasmPush(masm, ImmWord(TrapExitDummyValue));
  unsigned framePushedBeforePreserve = masm.framePushed();
  masm.PushRegsInMask(RegsToPreserve);
  unsigned offsetOfReturnWord = masm.framePushed() - framePushedBeforePreserve;

  // We know that StackPointer is word-aligned, but not necessarily
  // stack-aligned, so we need to align it dynamically.
  Register preAlignStackPointer = ABINonVolatileReg;
  masm.moveStackPtrTo(preAlignStackPointer);
  masm.andToStackPtr(Imm32(~(ABIStackAlignment - 1)));
  if (ShadowStackSpace) {
    masm.subFromStackPtr(Imm32(ShadowStackSpace));
  }

  masm.assertStackAlignment(ABIStackAlignment);
  masm.call(SymbolicAddress::HandleTrap);

  // WasmHandleTrap returns null if control should transfer to the throw stub.
  masm.branchTestPtr(Assembler::Zero, ReturnReg, ReturnReg, throwLabel);

  // Otherwise, the return value is the TrapData::resumePC we must jump to.
  // We must restore register state before jumping, which will clobber
  // ReturnReg, so store ReturnReg in the above-reserved stack slot which we
  // use to jump to via ret.
  masm.moveToStackPtr(preAlignStackPointer);
  masm.storePtr(ReturnReg, Address(masm.getStackPointer(), offsetOfReturnWord));
  masm.PopRegsInMask(RegsToPreserve);
#ifdef JS_CODEGEN_ARM64
  WasmPop(masm, lr);
  masm.abiret();
#else
  masm.ret();
#endif

  return FinishOffsets(masm, offsets);
}

// Generate a stub that restores the stack pointer to what it was on entry to
// the wasm activation, sets the return register to 'false' and then executes a
// return which will return from this wasm activation to the caller. This stub
// should only be called after the caller has reported an error.
static bool GenerateThrowStub(MacroAssembler& masm, Label* throwLabel,
                              Offsets* offsets) {
  Register scratch = ABINonArgReturnReg0;
  Register scratch2 = ABINonArgReturnReg1;

  AssertExpectedSP(masm);
  masm.haltingAlign(CodeAlignment);
  masm.setFramePushed(0);

  masm.bind(throwLabel);

  offsets->begin = masm.currentOffset();

  // Conservatively, the stack pointer can be unaligned and we must align it
  // dynamically.
  masm.andToStackPtr(Imm32(~(ABIStackAlignment - 1)));
  if (ShadowStackSpace) {
    masm.subFromStackPtr(Imm32(ShadowStackSpace));
  }

  // Allocate space for exception or regular resume information.
  masm.reserveStack(sizeof(jit::ResumeFromException));
  masm.moveStackPtrTo(scratch);

  MIRTypeVector handleThrowTypes;
  MOZ_ALWAYS_TRUE(handleThrowTypes.append(MIRType::Pointer));

  unsigned frameSize =
      StackDecrementForCall(ABIStackAlignment, masm.framePushed(),
                            StackArgBytesForNativeABI(handleThrowTypes));
  masm.reserveStack(frameSize);
  masm.assertStackAlignment(ABIStackAlignment);

  ABIArgMIRTypeIter i(handleThrowTypes);
  if (i->kind() == ABIArg::GPR) {
    masm.movePtr(scratch, i->gpr());
  } else {
    masm.storePtr(scratch,
                  Address(masm.getStackPointer(), i->offsetFromArgBase()));
  }
  i++;
  MOZ_ASSERT(i.done());

  // WasmHandleThrow unwinds JitActivation::wasmExitFP() and returns the
  // address of the return address on the stack this stub should return to.
  // Set the FramePointer to a magic value to indicate a return by throw.
  //
  // If there is a Wasm catch handler present, it will instead return the
  // address of the handler to jump to and the FP/SP values to restore.
  masm.call(SymbolicAddress::HandleThrow);

  Label resumeCatch, leaveWasm;

  masm.load32(Address(ReturnReg, offsetof(jit::ResumeFromException, kind)),
              scratch);

  masm.branch32(Assembler::Equal, scratch,
                Imm32(jit::ResumeFromException::RESUME_WASM_CATCH),
                &resumeCatch);
  masm.branch32(Assembler::Equal, scratch,
                Imm32(jit::ResumeFromException::RESUME_WASM), &leaveWasm);

  masm.breakpoint();

  // The case where a Wasm catch handler was found while unwinding the stack.
  masm.bind(&resumeCatch);
  masm.loadPtr(Address(ReturnReg, offsetof(ResumeFromException, framePointer)),
               FramePointer);
  // Defer reloading stackPointer until just before the jump, so as to
  // protect other live data on the stack.

  // When there is a catch handler, HandleThrow passes it the Value needed for
  // the handler's argument as well.
#ifdef JS_64BIT
  ValueOperand val(scratch);
#else
  ValueOperand val(scratch, scratch2);
#endif
  masm.loadValue(Address(ReturnReg, offsetof(ResumeFromException, exception)),
                 val);
  Register obj = masm.extractObject(val, scratch2);
  masm.loadPtr(Address(ReturnReg, offsetof(ResumeFromException, target)),
               scratch);
  // Now it's safe to reload stackPointer.
  masm.loadStackPtr(
      Address(ReturnReg, offsetof(ResumeFromException, stackPointer)));
  // This move must come after the SP is reloaded because WasmExceptionReg may
  // alias ReturnReg.
  masm.movePtr(obj, WasmExceptionReg);
  masm.jump(scratch);

  // No catch handler was found, so we will just return out.
  masm.bind(&leaveWasm);
  masm.loadPtr(Address(ReturnReg, offsetof(ResumeFromException, framePointer)),
               FramePointer);
  masm.loadPtr(Address(ReturnReg, offsetof(ResumeFromException, stackPointer)),
               scratch);
  masm.moveToStackPtr(scratch);
#ifdef JS_CODEGEN_ARM64
  masm.loadPtr(Address(scratch, 0), lr);
  masm.addToStackPtr(Imm32(8));
  masm.abiret();
#else
  masm.ret();
#endif

  return FinishOffsets(masm, offsets);
}

static const LiveRegisterSet AllAllocatableRegs =
    LiveRegisterSet(GeneralRegisterSet(Registers::AllocatableMask),
                    FloatRegisterSet(FloatRegisters::AllMask));

// Generate a stub that handle toggable enter/leave frame traps or breakpoints.
// The trap records frame pointer (via GenerateExitPrologue) and saves most of
// registers to not affect the code generated by WasmBaselineCompile.
static bool GenerateDebugTrapStub(MacroAssembler& masm, Label* throwLabel,
                                  CallableOffsets* offsets) {
  AssertExpectedSP(masm);
  masm.haltingAlign(CodeAlignment);
  masm.setFramePushed(0);

  GenerateExitPrologue(masm, 0, ExitReason::Fixed::DebugTrap, offsets);

  // Save all registers used between baseline compiler operations.
  masm.PushRegsInMask(AllAllocatableRegs);

  uint32_t framePushed = masm.framePushed();

  // This method might be called with unaligned stack -- aligning and
  // saving old stack pointer at the top.
#ifdef JS_CODEGEN_ARM64
  // On ARM64 however the stack is always aligned.
  static_assert(ABIStackAlignment == 16, "ARM64 SP alignment");
#else
  Register scratch = ABINonArgReturnReg0;
  masm.moveStackPtrTo(scratch);
  masm.subFromStackPtr(Imm32(sizeof(intptr_t)));
  masm.andToStackPtr(Imm32(~(ABIStackAlignment - 1)));
  masm.storePtr(scratch, Address(masm.getStackPointer(), 0));
#endif

  if (ShadowStackSpace) {
    masm.subFromStackPtr(Imm32(ShadowStackSpace));
  }
  masm.assertStackAlignment(ABIStackAlignment);
  masm.call(SymbolicAddress::HandleDebugTrap);

  masm.branchIfFalseBool(ReturnReg, throwLabel);

  if (ShadowStackSpace) {
    masm.addToStackPtr(Imm32(ShadowStackSpace));
  }
#ifndef JS_CODEGEN_ARM64
  masm.Pop(scratch);
  masm.moveToStackPtr(scratch);
#endif

  masm.setFramePushed(framePushed);
  masm.PopRegsInMask(AllAllocatableRegs);

  GenerateExitEpilogue(masm, 0, ExitReason::Fixed::DebugTrap, offsets);

  return FinishOffsets(masm, offsets);
}

bool wasm::GenerateEntryStubs(MacroAssembler& masm, size_t funcExportIndex,
                              const FuncExport& fe, const Maybe<ImmPtr>& callee,
                              bool isAsmJS, CodeRangeVector* codeRanges) {
  MOZ_ASSERT(!callee == fe.hasEagerStubs());
  MOZ_ASSERT_IF(isAsmJS, fe.hasEagerStubs());

  Offsets offsets;
  if (!GenerateInterpEntry(masm, fe, callee, &offsets)) {
    return false;
  }
  if (!codeRanges->emplaceBack(CodeRange::InterpEntry, fe.funcIndex(),
                               offsets)) {
    return false;
  }

  if (isAsmJS || !fe.canHaveJitEntry()) {
    return true;
  }

  if (!GenerateJitEntry(masm, funcExportIndex, fe, callee, &offsets)) {
    return false;
  }
  if (!codeRanges->emplaceBack(CodeRange::JitEntry, fe.funcIndex(), offsets)) {
    return false;
  }

  return true;
}

bool wasm::GenerateProvisionalLazyJitEntryStub(MacroAssembler& masm,
                                               Offsets* offsets) {
  AssertExpectedSP(masm);
  masm.setFramePushed(0);
  offsets->begin = masm.currentOffset();

#ifdef JS_CODEGEN_ARM64
  // Unaligned ABI calls require SP+PSP, but our mode here is SP-only
  masm.SetStackPointer64(PseudoStackPointer64);
  masm.Mov(PseudoStackPointer64, sp);
#endif

#ifdef JS_USE_LINK_REGISTER
  masm.pushReturnAddress();
#endif

  AllocatableGeneralRegisterSet regs(GeneralRegisterSet::Volatile());
  Register temp = regs.takeAny();

  using Fn = void* (*)();
  masm.setupUnalignedABICall(temp);
  masm.callWithABI<Fn, GetContextSensitiveInterpreterStub>(
      MoveOp::GENERAL, CheckUnsafeCallWithABI::DontCheckHasExitFrame);

#ifdef JS_USE_LINK_REGISTER
  masm.popReturnAddress();
#endif

  masm.jump(ReturnReg);

#ifdef JS_CODEGEN_ARM64
  // Undo the SP+PSP mode
  masm.SetStackPointer64(sp);
#endif

  return FinishOffsets(masm, offsets);
}

bool wasm::GenerateStubs(const ModuleEnvironment& env,
                         const FuncImportVector& imports,
                         const FuncExportVector& exports, CompiledCode* code) {
  LifoAlloc lifo(STUBS_LIFO_DEFAULT_CHUNK_SIZE);
  TempAllocator alloc(&lifo);
  WasmMacroAssembler masm(alloc, env);

  // Swap in already-allocated empty vectors to avoid malloc/free.
  if (!code->swap(masm)) {
    return false;
  }

  Label throwLabel;

  JitSpew(JitSpew_Codegen, "# Emitting wasm import stubs");

  for (uint32_t funcIndex = 0; funcIndex < imports.length(); funcIndex++) {
    const FuncImport& fi = imports[funcIndex];

    CallableOffsets interpOffsets;
    if (!GenerateImportInterpExit(masm, fi, funcIndex, &throwLabel,
                                  &interpOffsets)) {
      return false;
    }
    if (!code->codeRanges.emplaceBack(CodeRange::ImportInterpExit, funcIndex,
                                      interpOffsets)) {
      return false;
    }

    // Skip if the function does not have a signature that allows for a JIT
    // exit.
    if (!fi.canHaveJitExit()) {
      continue;
    }

    JitExitOffsets jitOffsets;
    if (!GenerateImportJitExit(masm, fi, funcIndex, &throwLabel, &jitOffsets)) {
      return false;
    }
    if (!code->codeRanges.emplaceBack(funcIndex, jitOffsets)) {
      return false;
    }
  }

  JitSpew(JitSpew_Codegen, "# Emitting wasm export stubs");

  Maybe<ImmPtr> noAbsolute;
  for (size_t i = 0; i < exports.length(); i++) {
    const FuncExport& fe = exports[i];
    if (!fe.hasEagerStubs()) {
      continue;
    }
    if (!GenerateEntryStubs(masm, i, fe, noAbsolute, env.isAsmJS(),
                            &code->codeRanges)) {
      return false;
    }
  }

  JitSpew(JitSpew_Codegen, "# Emitting wasm exit stubs");

  Offsets offsets;

  if (!GenerateTrapExit(masm, &throwLabel, &offsets)) {
    return false;
  }
  if (!code->codeRanges.emplaceBack(CodeRange::TrapExit, offsets)) {
    return false;
  }

  CallableOffsets callableOffsets;
  if (!GenerateDebugTrapStub(masm, &throwLabel, &callableOffsets)) {
    return false;
  }
  if (!code->codeRanges.emplaceBack(CodeRange::DebugTrap, callableOffsets)) {
    return false;
  }

  if (!GenerateThrowStub(masm, &throwLabel, &offsets)) {
    return false;
  }
  if (!code->codeRanges.emplaceBack(CodeRange::Throw, offsets)) {
    return false;
  }

  masm.finish();
  if (masm.oom()) {
    return false;
  }

  return code->swap(masm);
}

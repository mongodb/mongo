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

#include "wasm/WasmIonCompile.h"

#include "mozilla/MathAlgorithms.h"

#include <algorithm>

#include "jit/CodeGenerator.h"
#include "jit/CompileInfo.h"
#include "jit/Ion.h"
#include "jit/IonOptimizationLevels.h"
#include "js/ScalarType.h"  // js::Scalar::Type
#include "wasm/WasmBaselineCompile.h"
#include "wasm/WasmBuiltins.h"
#include "wasm/WasmGC.h"
#include "wasm/WasmGenerator.h"
#include "wasm/WasmOpIter.h"
#include "wasm/WasmSignalHandlers.h"
#include "wasm/WasmStubs.h"
#include "wasm/WasmValidate.h"

using namespace js;
using namespace js::jit;
using namespace js::wasm;

using mozilla::IsPowerOfTwo;
using mozilla::Maybe;
using mozilla::Nothing;
using mozilla::Some;

namespace {

using BlockVector = Vector<MBasicBlock*, 8, SystemAllocPolicy>;
using DefVector = Vector<MDefinition*, 8, SystemAllocPolicy>;

struct IonCompilePolicy {
  // We store SSA definitions in the value stack.
  using Value = MDefinition*;
  using ValueVector = DefVector;

  // We store loop headers and then/else blocks in the control flow stack.
  using ControlItem = MBasicBlock*;
};

using IonOpIter = OpIter<IonCompilePolicy>;

class FunctionCompiler;

// CallCompileState describes a call that is being compiled.

class CallCompileState {
  // A generator object that is passed each argument as it is compiled.
  WasmABIArgGenerator abi_;

  // Accumulates the register arguments while compiling arguments.
  MWasmCall::Args regArgs_;

  // Reserved argument for passing Instance* to builtin instance method calls.
  ABIArg instanceArg_;

  // The stack area in which the callee will write stack return values, or
  // nullptr if no stack results.
  MWasmStackResultArea* stackResultArea_ = nullptr;

  // Only FunctionCompiler should be directly manipulating CallCompileState.
  friend class FunctionCompiler;
};

// Encapsulates the compilation of a single function in an asm.js module. The
// function compiler handles the creation and final backend compilation of the
// MIR graph.
class FunctionCompiler {
  struct ControlFlowPatch {
    MControlInstruction* ins;
    uint32_t index;
    ControlFlowPatch(MControlInstruction* ins, uint32_t index)
        : ins(ins), index(index) {}
  };

  using ControlFlowPatchVector = Vector<ControlFlowPatch, 0, SystemAllocPolicy>;
  using ControlFlowPatchVectorVector =
      Vector<ControlFlowPatchVector, 0, SystemAllocPolicy>;

  const ModuleEnvironment& moduleEnv_;
  IonOpIter iter_;
  const FuncCompileInput& func_;
  const ValTypeVector& locals_;
  size_t lastReadCallSite_;

  TempAllocator& alloc_;
  MIRGraph& graph_;
  const CompileInfo& info_;
  MIRGenerator& mirGen_;

  MBasicBlock* curBlock_;
  uint32_t maxStackArgBytes_;

  uint32_t loopDepth_;
  uint32_t blockDepth_;
  ControlFlowPatchVectorVector blockPatches_;

  // TLS pointer argument to the current function.
  MWasmParameter* tlsPointer_;
  MWasmParameter* stackResultPointer_;

 public:
  FunctionCompiler(const ModuleEnvironment& moduleEnv, Decoder& decoder,
                   const FuncCompileInput& func, const ValTypeVector& locals,
                   MIRGenerator& mirGen)
      : moduleEnv_(moduleEnv),
        iter_(moduleEnv, decoder),
        func_(func),
        locals_(locals),
        lastReadCallSite_(0),
        alloc_(mirGen.alloc()),
        graph_(mirGen.graph()),
        info_(mirGen.outerInfo()),
        mirGen_(mirGen),
        curBlock_(nullptr),
        maxStackArgBytes_(0),
        loopDepth_(0),
        blockDepth_(0),
        tlsPointer_(nullptr),
        stackResultPointer_(nullptr) {}

  const ModuleEnvironment& moduleEnv() const { return moduleEnv_; }

  IonOpIter& iter() { return iter_; }
  TempAllocator& alloc() const { return alloc_; }
  // FIXME(1401675): Replace with BlockType.
  uint32_t funcIndex() const { return func_.index; }
  const FuncType& funcType() const {
    return *moduleEnv_.funcs[func_.index].type;
  }

  BytecodeOffset bytecodeOffset() const { return iter_.bytecodeOffset(); }
  BytecodeOffset bytecodeIfNotAsmJS() const {
    return moduleEnv_.isAsmJS() ? BytecodeOffset() : iter_.bytecodeOffset();
  }

  bool init() {
    // Prepare the entry block for MIR generation:

    const ArgTypeVector args(funcType());

    if (!mirGen_.ensureBallast()) {
      return false;
    }
    if (!newBlock(/* prev */ nullptr, &curBlock_)) {
      return false;
    }

    for (WasmABIArgIter i(args); !i.done(); i++) {
      MWasmParameter* ins = MWasmParameter::New(alloc(), *i, i.mirType());
      curBlock_->add(ins);
      if (args.isSyntheticStackResultPointerArg(i.index())) {
        MOZ_ASSERT(stackResultPointer_ == nullptr);
        stackResultPointer_ = ins;
      } else {
        curBlock_->initSlot(info().localSlot(args.naturalIndex(i.index())),
                            ins);
      }
      if (!mirGen_.ensureBallast()) {
        return false;
      }
    }

    // Set up a parameter that receives the hidden TLS pointer argument.
    tlsPointer_ =
        MWasmParameter::New(alloc(), ABIArg(WasmTlsReg), MIRType::Pointer);
    curBlock_->add(tlsPointer_);
    if (!mirGen_.ensureBallast()) {
      return false;
    }

    for (size_t i = args.lengthWithoutStackResults(); i < locals_.length();
         i++) {
      MInstruction* ins = nullptr;
      switch (locals_[i].kind()) {
        case ValType::I32:
          ins = MConstant::New(alloc(), Int32Value(0), MIRType::Int32);
          break;
        case ValType::I64:
          ins = MConstant::NewInt64(alloc(), 0);
          break;
        case ValType::V128:
#ifdef ENABLE_WASM_SIMD
          ins =
              MWasmFloatConstant::NewSimd128(alloc(), SimdConstant::SplatX4(0));
          break;
#else
          return iter().fail("Ion has no SIMD support yet");
#endif
        case ValType::F32:
          ins = MConstant::New(alloc(), Float32Value(0.f), MIRType::Float32);
          break;
        case ValType::F64:
          ins = MConstant::New(alloc(), DoubleValue(0.0), MIRType::Double);
          break;
        case ValType::Rtt:
        case ValType::Ref:
          ins = MWasmNullConstant::New(alloc());
          break;
      }

      curBlock_->add(ins);
      curBlock_->initSlot(info().localSlot(i), ins);
      if (!mirGen_.ensureBallast()) {
        return false;
      }
    }

    return true;
  }

  void finish() {
    mirGen().initWasmMaxStackArgBytes(maxStackArgBytes_);

    MOZ_ASSERT(loopDepth_ == 0);
    MOZ_ASSERT(blockDepth_ == 0);
#ifdef DEBUG
    for (ControlFlowPatchVector& patches : blockPatches_) {
      MOZ_ASSERT(patches.empty());
    }
#endif
    MOZ_ASSERT(inDeadCode());
    MOZ_ASSERT(done(), "all bytes must be consumed");
    MOZ_ASSERT(func_.callSiteLineNums.length() == lastReadCallSite_);
  }

  /************************* Read-only interface (after local scope setup) */

  MIRGenerator& mirGen() const { return mirGen_; }
  MIRGraph& mirGraph() const { return graph_; }
  const CompileInfo& info() const { return info_; }

  MDefinition* getLocalDef(unsigned slot) {
    if (inDeadCode()) {
      return nullptr;
    }
    return curBlock_->getSlot(info().localSlot(slot));
  }

  const ValTypeVector& locals() const { return locals_; }

  /***************************** Code generation (after local scope setup) */

  MDefinition* constant(const Value& v, MIRType type) {
    if (inDeadCode()) {
      return nullptr;
    }
    MConstant* constant = MConstant::New(alloc(), v, type);
    curBlock_->add(constant);
    return constant;
  }

  MDefinition* constant(float f) {
    if (inDeadCode()) {
      return nullptr;
    }
    auto* cst = MWasmFloatConstant::NewFloat32(alloc(), f);
    curBlock_->add(cst);
    return cst;
  }

  MDefinition* constant(double d) {
    if (inDeadCode()) {
      return nullptr;
    }
    auto* cst = MWasmFloatConstant::NewDouble(alloc(), d);
    curBlock_->add(cst);
    return cst;
  }

  MDefinition* constant(int64_t i) {
    if (inDeadCode()) {
      return nullptr;
    }
    MConstant* constant = MConstant::NewInt64(alloc(), i);
    curBlock_->add(constant);
    return constant;
  }

#ifdef ENABLE_WASM_SIMD
  MDefinition* constant(V128 v) {
    if (inDeadCode()) {
      return nullptr;
    }
    MWasmFloatConstant* constant = MWasmFloatConstant::NewSimd128(
        alloc(), SimdConstant::CreateSimd128((int8_t*)v.bytes));
    curBlock_->add(constant);
    return constant;
  }
#endif

  MDefinition* nullRefConstant() {
    if (inDeadCode()) {
      return nullptr;
    }
    // MConstant has a lot of baggage so we don't use that here.
    MWasmNullConstant* constant = MWasmNullConstant::New(alloc());
    curBlock_->add(constant);
    return constant;
  }

  void fence() {
    if (inDeadCode()) {
      return;
    }
    MWasmFence* ins = MWasmFence::New(alloc());
    curBlock_->add(ins);
  }

  template <class T>
  MDefinition* unary(MDefinition* op) {
    if (inDeadCode()) {
      return nullptr;
    }
    T* ins = T::New(alloc(), op);
    curBlock_->add(ins);
    return ins;
  }

  template <class T>
  MDefinition* unary(MDefinition* op, MIRType type) {
    if (inDeadCode()) {
      return nullptr;
    }
    T* ins = T::New(alloc(), op, type);
    curBlock_->add(ins);
    return ins;
  }

  template <class T>
  MDefinition* binary(MDefinition* lhs, MDefinition* rhs) {
    if (inDeadCode()) {
      return nullptr;
    }
    T* ins = T::New(alloc(), lhs, rhs);
    curBlock_->add(ins);
    return ins;
  }

  template <class T>
  MDefinition* binary(MDefinition* lhs, MDefinition* rhs, MIRType type) {
    if (inDeadCode()) {
      return nullptr;
    }
    T* ins = T::New(alloc(), lhs, rhs, type);
    curBlock_->add(ins);
    return ins;
  }

  MDefinition* ursh(MDefinition* lhs, MDefinition* rhs, MIRType type) {
    if (inDeadCode()) {
      return nullptr;
    }
    auto* ins = MUrsh::NewWasm(alloc(), lhs, rhs, type);
    curBlock_->add(ins);
    return ins;
  }

  MDefinition* add(MDefinition* lhs, MDefinition* rhs, MIRType type) {
    if (inDeadCode()) {
      return nullptr;
    }
    auto* ins = MAdd::NewWasm(alloc(), lhs, rhs, type);
    curBlock_->add(ins);
    return ins;
  }

  bool mustPreserveNaN(MIRType type) {
    return IsFloatingPointType(type) && !moduleEnv().isAsmJS();
  }

  MDefinition* sub(MDefinition* lhs, MDefinition* rhs, MIRType type) {
    if (inDeadCode()) {
      return nullptr;
    }

    // wasm can't fold x - 0.0 because of NaN with custom payloads.
    MSub* ins = MSub::NewWasm(alloc(), lhs, rhs, type, mustPreserveNaN(type));
    curBlock_->add(ins);
    return ins;
  }

  MDefinition* nearbyInt(MDefinition* input, RoundingMode roundingMode) {
    if (inDeadCode()) {
      return nullptr;
    }

    auto* ins = MNearbyInt::New(alloc(), input, input->type(), roundingMode);
    curBlock_->add(ins);
    return ins;
  }

  MDefinition* minMax(MDefinition* lhs, MDefinition* rhs, MIRType type,
                      bool isMax) {
    if (inDeadCode()) {
      return nullptr;
    }

    if (mustPreserveNaN(type)) {
      // Convert signaling NaN to quiet NaNs.
      MDefinition* zero = constant(DoubleValue(0.0), type);
      lhs = sub(lhs, zero, type);
      rhs = sub(rhs, zero, type);
    }

    MMinMax* ins = MMinMax::NewWasm(alloc(), lhs, rhs, type, isMax);
    curBlock_->add(ins);
    return ins;
  }

  MDefinition* mul(MDefinition* lhs, MDefinition* rhs, MIRType type,
                   MMul::Mode mode) {
    if (inDeadCode()) {
      return nullptr;
    }

    // wasm can't fold x * 1.0 because of NaN with custom payloads.
    auto* ins =
        MMul::NewWasm(alloc(), lhs, rhs, type, mode, mustPreserveNaN(type));
    curBlock_->add(ins);
    return ins;
  }

  MDefinition* div(MDefinition* lhs, MDefinition* rhs, MIRType type,
                   bool unsignd) {
    if (inDeadCode()) {
      return nullptr;
    }
    bool trapOnError = !moduleEnv().isAsmJS();
    if (!unsignd && type == MIRType::Int32) {
      // Enforce the signedness of the operation by coercing the operands
      // to signed.  Otherwise, operands that "look" unsigned to Ion but
      // are not unsigned to Baldr (eg, unsigned right shifts) may lead to
      // the operation being executed unsigned.  Applies to mod() as well.
      //
      // Do this for Int32 only since Int64 is not subject to the same
      // issues.
      //
      // Note the offsets passed to MWasmBuiltinTruncateToInt32 are wrong here,
      // but it doesn't matter: they're not codegen'd to calls since inputs
      // already are int32.
      auto* lhs2 = createTruncateToInt32(lhs);
      curBlock_->add(lhs2);
      lhs = lhs2;
      auto* rhs2 = createTruncateToInt32(rhs);
      curBlock_->add(rhs2);
      rhs = rhs2;
    }

    // For x86 and arm we implement i64 div via c++ builtin.
    // A call to c++ builtin requires tls pointer.
#if defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_ARM)
    if (type == MIRType::Int64) {
      auto* ins =
          MWasmBuiltinDivI64::New(alloc(), lhs, rhs, tlsPointer_, unsignd,
                                  trapOnError, bytecodeOffset());
      curBlock_->add(ins);
      return ins;
    }
#endif

    auto* ins = MDiv::New(alloc(), lhs, rhs, type, unsignd, trapOnError,
                          bytecodeOffset(), mustPreserveNaN(type));
    curBlock_->add(ins);
    return ins;
  }

  MInstruction* createTruncateToInt32(MDefinition* op) {
    if (op->type() == MIRType::Double || op->type() == MIRType::Float32) {
      return MWasmBuiltinTruncateToInt32::New(alloc(), op, tlsPointer_);
    }

    return MTruncateToInt32::New(alloc(), op);
  }

  MDefinition* mod(MDefinition* lhs, MDefinition* rhs, MIRType type,
                   bool unsignd) {
    if (inDeadCode()) {
      return nullptr;
    }
    bool trapOnError = !moduleEnv().isAsmJS();
    if (!unsignd && type == MIRType::Int32) {
      // See block comment in div().
      auto* lhs2 = createTruncateToInt32(lhs);
      curBlock_->add(lhs2);
      lhs = lhs2;
      auto* rhs2 = createTruncateToInt32(rhs);
      curBlock_->add(rhs2);
      rhs = rhs2;
    }

    // For x86 and arm we implement i64 mod via c++ builtin.
    // A call to c++ builtin requires tls pointer.
#if defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_ARM)
    if (type == MIRType::Int64) {
      auto* ins =
          MWasmBuiltinModI64::New(alloc(), lhs, rhs, tlsPointer_, unsignd,
                                  trapOnError, bytecodeOffset());
      curBlock_->add(ins);
      return ins;
    }
#endif

    // Should be handled separately because we call BuiltinThunk for this case
    // and so, need to add the dependency from tlsPointer.
    if (type == MIRType::Double) {
      auto* ins = MWasmBuiltinModD::New(alloc(), lhs, rhs, tlsPointer_, type,
                                        bytecodeOffset());
      curBlock_->add(ins);
      return ins;
    }

    auto* ins = MMod::New(alloc(), lhs, rhs, type, unsignd, trapOnError,
                          bytecodeOffset());
    curBlock_->add(ins);
    return ins;
  }

  MDefinition* bitnot(MDefinition* op) {
    if (inDeadCode()) {
      return nullptr;
    }
    auto* ins = MBitNot::New(alloc(), op);
    curBlock_->add(ins);
    return ins;
  }

  MDefinition* select(MDefinition* trueExpr, MDefinition* falseExpr,
                      MDefinition* condExpr) {
    if (inDeadCode()) {
      return nullptr;
    }
    auto* ins = MWasmSelect::New(alloc(), trueExpr, falseExpr, condExpr);
    curBlock_->add(ins);
    return ins;
  }

  MDefinition* extendI32(MDefinition* op, bool isUnsigned) {
    if (inDeadCode()) {
      return nullptr;
    }
    auto* ins = MExtendInt32ToInt64::New(alloc(), op, isUnsigned);
    curBlock_->add(ins);
    return ins;
  }

  MDefinition* signExtend(MDefinition* op, uint32_t srcSize,
                          uint32_t targetSize) {
    if (inDeadCode()) {
      return nullptr;
    }
    MInstruction* ins;
    switch (targetSize) {
      case 4: {
        MSignExtendInt32::Mode mode;
        switch (srcSize) {
          case 1:
            mode = MSignExtendInt32::Byte;
            break;
          case 2:
            mode = MSignExtendInt32::Half;
            break;
          default:
            MOZ_CRASH("Bad sign extension");
        }
        ins = MSignExtendInt32::New(alloc(), op, mode);
        break;
      }
      case 8: {
        MSignExtendInt64::Mode mode;
        switch (srcSize) {
          case 1:
            mode = MSignExtendInt64::Byte;
            break;
          case 2:
            mode = MSignExtendInt64::Half;
            break;
          case 4:
            mode = MSignExtendInt64::Word;
            break;
          default:
            MOZ_CRASH("Bad sign extension");
        }
        ins = MSignExtendInt64::New(alloc(), op, mode);
        break;
      }
      default: {
        MOZ_CRASH("Bad sign extension");
      }
    }
    curBlock_->add(ins);
    return ins;
  }

  MDefinition* convertI64ToFloatingPoint(MDefinition* op, MIRType type,
                                         bool isUnsigned) {
    if (inDeadCode()) {
      return nullptr;
    }
#if defined(JS_CODEGEN_ARM)
    auto* ins = MBuiltinInt64ToFloatingPoint::New(
        alloc(), op, tlsPointer_, type, bytecodeOffset(), isUnsigned);
#else
    auto* ins = MInt64ToFloatingPoint::New(alloc(), op, type, bytecodeOffset(),
                                           isUnsigned);
#endif
    curBlock_->add(ins);
    return ins;
  }

  MDefinition* rotate(MDefinition* input, MDefinition* count, MIRType type,
                      bool left) {
    if (inDeadCode()) {
      return nullptr;
    }
    auto* ins = MRotate::New(alloc(), input, count, type, left);
    curBlock_->add(ins);
    return ins;
  }

  template <class T>
  MDefinition* truncate(MDefinition* op, TruncFlags flags) {
    if (inDeadCode()) {
      return nullptr;
    }
    auto* ins = T::New(alloc(), op, flags, bytecodeOffset());
    curBlock_->add(ins);
    return ins;
  }

#if defined(JS_CODEGEN_ARM)
  MDefinition* truncateWithTls(MDefinition* op, TruncFlags flags) {
    if (inDeadCode()) {
      return nullptr;
    }
    auto* ins = MWasmBuiltinTruncateToInt64::New(alloc(), op, tlsPointer_,
                                                 flags, bytecodeOffset());
    curBlock_->add(ins);
    return ins;
  }
#endif

  MDefinition* compare(MDefinition* lhs, MDefinition* rhs, JSOp op,
                       MCompare::CompareType type) {
    if (inDeadCode()) {
      return nullptr;
    }
    auto* ins = MCompare::NewWasm(alloc(), lhs, rhs, op, type);
    curBlock_->add(ins);
    return ins;
  }

  void assign(unsigned slot, MDefinition* def) {
    if (inDeadCode()) {
      return;
    }
    curBlock_->setSlot(info().localSlot(slot), def);
  }

#ifdef ENABLE_WASM_SIMD
  // About Wasm SIMD as supported by Ion:
  //
  // The expectation is that Ion will only ever support SIMD on x86 and x64,
  // since Cranelift will be the optimizing compiler for Arm64, ARMv7 will cease
  // to be a tier-1 platform soon, and MIPS32 and MIPS64 will never implement
  // SIMD.
  //
  // The division of the operations into MIR nodes reflects that expectation,
  // and is a good fit for x86/x64.  Should the expectation change we'll
  // possibly want to re-architect the SIMD support to be a little more general.
  //
  // Most SIMD operations map directly to a single MIR node that ultimately ends
  // up being expanded in the macroassembler.
  //
  // Some SIMD operations that do have a complete macroassembler expansion are
  // open-coded into multiple MIR nodes here; in some cases that's just
  // convenience, in other cases it may also allow them to benefit from Ion
  // optimizations.  The reason for the expansions will be documented by a
  // comment.

  // (v128,v128) -> v128 effect-free binary operations
  MDefinition* binarySimd128(MDefinition* lhs, MDefinition* rhs,
                             bool commutative, SimdOp op) {
    if (inDeadCode()) {
      return nullptr;
    }

    MOZ_ASSERT(lhs->type() == MIRType::Simd128 &&
               rhs->type() == MIRType::Simd128);

    auto* ins = MWasmBinarySimd128::New(alloc(), lhs, rhs, commutative, op);
    curBlock_->add(ins);
    return ins;
  }

  // (v128,i32) -> v128 effect-free shift operations
  MDefinition* shiftSimd128(MDefinition* lhs, MDefinition* rhs, SimdOp op) {
    if (inDeadCode()) {
      return nullptr;
    }

    MOZ_ASSERT(lhs->type() == MIRType::Simd128 &&
               rhs->type() == MIRType::Int32);

    auto* ins = MWasmShiftSimd128::New(alloc(), lhs, rhs, op);
    curBlock_->add(ins);
    return ins;
  }

  // (v128,scalar,imm) -> v128
  MDefinition* replaceLaneSimd128(MDefinition* lhs, MDefinition* rhs,
                                  uint32_t laneIndex, SimdOp op) {
    if (inDeadCode()) {
      return nullptr;
    }

    MOZ_ASSERT(lhs->type() == MIRType::Simd128);

    auto* ins = MWasmReplaceLaneSimd128::New(alloc(), lhs, rhs, laneIndex, op);
    curBlock_->add(ins);
    return ins;
  }

  // (scalar) -> v128 effect-free unary operations
  MDefinition* scalarToSimd128(MDefinition* src, SimdOp op) {
    if (inDeadCode()) {
      return nullptr;
    }

    auto* ins = MWasmScalarToSimd128::New(alloc(), src, op);
    curBlock_->add(ins);
    return ins;
  }

  // (v128) -> v128 effect-free unary operations
  MDefinition* unarySimd128(MDefinition* src, SimdOp op) {
    if (inDeadCode()) {
      return nullptr;
    }

    MOZ_ASSERT(src->type() == MIRType::Simd128);
    auto* ins = MWasmUnarySimd128::New(alloc(), src, op);
    curBlock_->add(ins);
    return ins;
  }

  // (v128, imm) -> scalar effect-free unary operations
  MDefinition* reduceSimd128(MDefinition* src, SimdOp op, ValType outType,
                             uint32_t imm = 0) {
    if (inDeadCode()) {
      return nullptr;
    }

    MOZ_ASSERT(src->type() == MIRType::Simd128);
    auto* ins =
        MWasmReduceSimd128::New(alloc(), src, op, ToMIRType(outType), imm);
    curBlock_->add(ins);
    return ins;
  }

  // (v128, v128, v128) -> v128 effect-free operations
  MDefinition* bitselectSimd128(MDefinition* v1, MDefinition* v2,
                                MDefinition* control) {
    if (inDeadCode()) {
      return nullptr;
    }

    MOZ_ASSERT(v1->type() == MIRType::Simd128);
    MOZ_ASSERT(v2->type() == MIRType::Simd128);
    MOZ_ASSERT(control->type() == MIRType::Simd128);
    auto* ins = MWasmBitselectSimd128::New(alloc(), v1, v2, control);
    curBlock_->add(ins);
    return ins;
  }

  // (v128, v128, imm_v128) -> v128 effect-free operations
  MDefinition* shuffleSimd128(MDefinition* v1, MDefinition* v2, V128 control) {
    if (inDeadCode()) {
      return nullptr;
    }

    MOZ_ASSERT(v1->type() == MIRType::Simd128);
    MOZ_ASSERT(v2->type() == MIRType::Simd128);
    auto* ins = MWasmShuffleSimd128::New(
        alloc(), v1, v2,
        SimdConstant::CreateX16(reinterpret_cast<int8_t*>(control.bytes)));
    curBlock_->add(ins);
    return ins;
  }

  MDefinition* loadSplatSimd128(Scalar::Type viewType,
                                const LinearMemoryAddress<MDefinition*>& addr,
                                wasm::SimdOp splatOp) {
    if (inDeadCode()) {
      return nullptr;
    }

    MemoryAccessDesc access(viewType, addr.align, addr.offset,
                            bytecodeIfNotAsmJS());

    // Generate better code (on x86)
    if (viewType == Scalar::Float64) {
      access.setSplatSimd128Load();
      return load(addr.base, &access, ValType::V128);
    }

    ValType resultType = ValType::I32;
    if (viewType == Scalar::Float32) {
      resultType = ValType::F32;
      splatOp = wasm::SimdOp::F32x4Splat;
    }
    auto* scalar = load(addr.base, &access, resultType);
    if (!inDeadCode() && !scalar) {
      return nullptr;
    }
    return scalarToSimd128(scalar, splatOp);
  }

  MDefinition* loadExtendSimd128(const LinearMemoryAddress<MDefinition*>& addr,
                                 wasm::SimdOp op) {
    if (inDeadCode()) {
      return nullptr;
    }

    // Generate better code (on x86) by loading as a double with an
    // operation that sign extends directly.
    MemoryAccessDesc access(Scalar::Float64, addr.align, addr.offset,
                            bytecodeIfNotAsmJS());
    access.setWidenSimd128Load(op);
    return load(addr.base, &access, ValType::V128);
  }

  MDefinition* loadZeroSimd128(Scalar::Type viewType, size_t numBytes,
                               const LinearMemoryAddress<MDefinition*>& addr) {
    if (inDeadCode()) {
      return nullptr;
    }

    MemoryAccessDesc access(viewType, addr.align, addr.offset,
                            bytecodeIfNotAsmJS());
    access.setZeroExtendSimd128Load();
    return load(addr.base, &access, ValType::V128);
  }

  MDefinition* loadLaneSimd128(uint32_t laneSize,
                               const LinearMemoryAddress<MDefinition*>& addr,
                               uint32_t laneIndex, MDefinition* src) {
    if (inDeadCode()) {
      return nullptr;
    }

    MemoryAccessDesc access(Scalar::Simd128, addr.align, addr.offset,
                            bytecodeIfNotAsmJS());
    MWasmLoadTls* memoryBase = maybeLoadMemoryBase();
    MDefinition* base = addr.base;
    MOZ_ASSERT(!moduleEnv_.isAsmJS());
    checkOffsetAndAlignmentAndBounds(&access, &base);
    MInstruction* load = MWasmLoadLaneSimd128::New(
        alloc(), memoryBase, base, access, laneSize, laneIndex, src);
    if (!load) {
      return nullptr;
    }
    curBlock_->add(load);
    return load;
  }

  void storeLaneSimd128(uint32_t laneSize,
                        const LinearMemoryAddress<MDefinition*>& addr,
                        uint32_t laneIndex, MDefinition* src) {
    if (inDeadCode()) {
      return;
    }
    MemoryAccessDesc access(Scalar::Simd128, addr.align, addr.offset,
                            bytecodeIfNotAsmJS());
    MWasmLoadTls* memoryBase = maybeLoadMemoryBase();
    MDefinition* base = addr.base;
    MOZ_ASSERT(!moduleEnv_.isAsmJS());
    checkOffsetAndAlignmentAndBounds(&access, &base);
    MInstruction* store = MWasmStoreLaneSimd128::New(
        alloc(), memoryBase, base, access, laneSize, laneIndex, src);
    if (!store) {
      return;
    }
    curBlock_->add(store);
  }
#endif  // ENABLE_WASM_SIMD

 private:
  MWasmLoadTls* maybeLoadMemoryBase() {
    MWasmLoadTls* load = nullptr;
#ifdef JS_CODEGEN_X86
    AliasSet aliases = !moduleEnv_.memory->canMovingGrow()
                           ? AliasSet::None()
                           : AliasSet::Load(AliasSet::WasmHeapMeta);
    load = MWasmLoadTls::New(alloc(), tlsPointer_,
                             offsetof(wasm::TlsData, memoryBase),
                             MIRType::Pointer, aliases);
    curBlock_->add(load);
#endif
    return load;
  }

  MWasmLoadTls* maybeLoadBoundsCheckLimit(MIRType type) {
#ifdef JS_64BIT
    MOZ_ASSERT(type == MIRType::Int32 || type == MIRType::Int64);
#else
    MOZ_ASSERT(type == MIRType::Int32);
#endif
    if (moduleEnv_.hugeMemoryEnabled()) {
      return nullptr;
    }
    AliasSet aliases = !moduleEnv_.memory->canMovingGrow()
                           ? AliasSet::None()
                           : AliasSet::Load(AliasSet::WasmHeapMeta);
    auto* load = MWasmLoadTls::New(alloc(), tlsPointer_,
                                   offsetof(wasm::TlsData, boundsCheckLimit),
                                   type, aliases);
    curBlock_->add(load);
    return load;
  }

 public:
  MWasmHeapBase* memoryBase() {
    MWasmHeapBase* base = nullptr;
    AliasSet aliases = !moduleEnv_.memory->canMovingGrow()
                           ? AliasSet::None()
                           : AliasSet::Load(AliasSet::WasmHeapMeta);
    base = MWasmHeapBase::New(alloc(), tlsPointer_, aliases);
    curBlock_->add(base);
    return base;
  }

 private:
  // Only sets *mustAdd if it also returns true.
  bool needAlignmentCheck(MemoryAccessDesc* access, MDefinition* base,
                          bool* mustAdd) {
    MOZ_ASSERT(!*mustAdd);

    // asm.js accesses are always aligned and need no checks.
    if (moduleEnv_.isAsmJS() || !access->isAtomic()) {
      return false;
    }

    if (base->isConstant()) {
      int32_t ptr = base->toConstant()->toInt32();
      // OK to wrap around the address computation here.
      if (((ptr + access->offset()) & (access->byteSize() - 1)) == 0) {
        return false;
      }
    }

    *mustAdd = (access->offset() & (access->byteSize() - 1)) != 0;
    return true;
  }

  void checkOffsetAndAlignmentAndBounds(MemoryAccessDesc* access,
                                        MDefinition** base) {
    MOZ_ASSERT(!inDeadCode());
    MOZ_ASSERT(!moduleEnv_.isAsmJS());

    uint32_t offsetGuardLimit =
        GetMaxOffsetGuardLimit(moduleEnv_.hugeMemoryEnabled());

    // Fold a constant base into the offset and make the base 0, provided the
    // offset stays below the guard limit.  The reason for folding the base into
    // the offset rather than vice versa is that a small offset can be ignored
    // by both explicit bounds checking and bounds check elimination.
    if ((*base)->isConstant()) {
      uint32_t basePtr = (*base)->toConstant()->toInt32();
      uint32_t offset = access->offset();

      if (offset < offsetGuardLimit && basePtr < offsetGuardLimit - offset) {
        auto* ins = MConstant::New(alloc(), Int32Value(0), MIRType::Int32);
        curBlock_->add(ins);
        *base = ins;
        access->setOffset(access->offset() + basePtr);
      }
    }

    bool mustAdd = false;
    bool alignmentCheck = needAlignmentCheck(access, *base, &mustAdd);

    // If the offset is bigger than the guard region, a separate instruction is
    // necessary to add the offset to the base and check for overflow.
    //
    // Also add the offset if we have a Wasm atomic access that needs alignment
    // checking and the offset affects alignment.
    if (access->offset() >= offsetGuardLimit || mustAdd ||
        !JitOptions.wasmFoldOffsets) {
      *base = computeEffectiveAddress(*base, access);
    }

    if (alignmentCheck) {
      curBlock_->add(MWasmAlignmentCheck::New(
          alloc(), *base, access->byteSize(), bytecodeOffset()));
    }

#ifdef JS_64BIT
    // If the bounds check uses the full 64 bits of the bounds check limit, then
    // *base must be zero-extended to 64 bits before checking and wrapped back
    // to 32-bits after Spectre masking.  (And it's important that the value we
    // end up with has flowed through the Spectre mask.)
    //
    // If the memory's max size is known to be smaller than 64K pages exactly,
    // we can use a 32-bit check and avoid extension and wrapping.
    bool check64 = !moduleEnv_.memory->boundsCheckLimitIs32Bits() &&
                   ArrayBufferObject::maxBufferByteLength() >= 0x100000000;
#else
    bool check64 = false;
#endif
    MWasmLoadTls* boundsCheckLimit =
        maybeLoadBoundsCheckLimit(check64 ? MIRType::Int64 : MIRType::Int32);
    if (boundsCheckLimit) {
      // At the outset, actualBase could be the result of pretty much any i32
      // operation, or it could be the load of an i32 constant.  We may assume
      // the value has a canonical representation for the platform, see doc
      // block in MacroAssembler.h.
      MDefinition* actualBase = *base;

      // Extend the index value to perform a 64-bit bounds check if the memory
      // can be 4GB.

      if (check64) {
        auto* extended = MWasmExtendU32Index::New(alloc(), actualBase);
        curBlock_->add(extended);
        actualBase = extended;
      }
      auto* ins = MWasmBoundsCheck::New(alloc(), actualBase, boundsCheckLimit,
                                        bytecodeOffset());
      curBlock_->add(ins);
      actualBase = ins;

      // If we're masking, then we update *base to create a dependency chain
      // through the masked index.  But we will first need to wrap the index
      // value if it was extended above.

      if (JitOptions.spectreIndexMasking) {
        if (check64) {
          auto* wrapped = MWasmWrapU32Index::New(alloc(), actualBase);
          curBlock_->add(wrapped);
          actualBase = wrapped;
        }
        *base = actualBase;
      }
    }
  }

  bool isSmallerAccessForI64(ValType result, const MemoryAccessDesc* access) {
    if (result == ValType::I64 && access->byteSize() <= 4) {
      // These smaller accesses should all be zero-extending.
      MOZ_ASSERT(!isSignedIntType(access->type()));
      return true;
    }
    return false;
  }

 public:
  MDefinition* computeEffectiveAddress(MDefinition* base,
                                       MemoryAccessDesc* access) {
    if (inDeadCode()) {
      return nullptr;
    }
    if (!access->offset()) {
      return base;
    }
    auto* ins =
        MWasmAddOffset::New(alloc(), base, access->offset(), bytecodeOffset());
    curBlock_->add(ins);
    access->clearOffset();
    return ins;
  }

  MDefinition* load(MDefinition* base, MemoryAccessDesc* access,
                    ValType result) {
    if (inDeadCode()) {
      return nullptr;
    }

    MWasmLoadTls* memoryBase = maybeLoadMemoryBase();
    MInstruction* load = nullptr;
    if (moduleEnv_.isAsmJS()) {
      MOZ_ASSERT(access->offset() == 0);
      MWasmLoadTls* boundsCheckLimit =
          maybeLoadBoundsCheckLimit(MIRType::Int32);
      load = MAsmJSLoadHeap::New(alloc(), memoryBase, base, boundsCheckLimit,
                                 access->type());
    } else {
      checkOffsetAndAlignmentAndBounds(access, &base);
      load =
          MWasmLoad::New(alloc(), memoryBase, base, *access, ToMIRType(result));
    }
    if (!load) {
      return nullptr;
    }
    curBlock_->add(load);
    return load;
  }

  void store(MDefinition* base, MemoryAccessDesc* access, MDefinition* v) {
    if (inDeadCode()) {
      return;
    }

    MWasmLoadTls* memoryBase = maybeLoadMemoryBase();
    MInstruction* store = nullptr;
    if (moduleEnv_.isAsmJS()) {
      MOZ_ASSERT(access->offset() == 0);
      MWasmLoadTls* boundsCheckLimit =
          maybeLoadBoundsCheckLimit(MIRType::Int32);
      store = MAsmJSStoreHeap::New(alloc(), memoryBase, base, boundsCheckLimit,
                                   access->type(), v);
    } else {
      checkOffsetAndAlignmentAndBounds(access, &base);
      store = MWasmStore::New(alloc(), memoryBase, base, *access, v);
    }
    if (!store) {
      return;
    }
    curBlock_->add(store);
  }

  MDefinition* atomicCompareExchangeHeap(MDefinition* base,
                                         MemoryAccessDesc* access,
                                         ValType result, MDefinition* oldv,
                                         MDefinition* newv) {
    if (inDeadCode()) {
      return nullptr;
    }

    checkOffsetAndAlignmentAndBounds(access, &base);

    if (isSmallerAccessForI64(result, access)) {
      auto* cvtOldv =
          MWrapInt64ToInt32::New(alloc(), oldv, /*bottomHalf=*/true);
      curBlock_->add(cvtOldv);
      oldv = cvtOldv;

      auto* cvtNewv =
          MWrapInt64ToInt32::New(alloc(), newv, /*bottomHalf=*/true);
      curBlock_->add(cvtNewv);
      newv = cvtNewv;
    }

    MWasmLoadTls* memoryBase = maybeLoadMemoryBase();
    MInstruction* cas =
        MWasmCompareExchangeHeap::New(alloc(), bytecodeOffset(), memoryBase,
                                      base, *access, oldv, newv, tlsPointer_);
    if (!cas) {
      return nullptr;
    }
    curBlock_->add(cas);

    if (isSmallerAccessForI64(result, access)) {
      cas = MExtendInt32ToInt64::New(alloc(), cas, true);
      curBlock_->add(cas);
    }

    return cas;
  }

  MDefinition* atomicExchangeHeap(MDefinition* base, MemoryAccessDesc* access,
                                  ValType result, MDefinition* value) {
    if (inDeadCode()) {
      return nullptr;
    }

    checkOffsetAndAlignmentAndBounds(access, &base);

    if (isSmallerAccessForI64(result, access)) {
      auto* cvtValue =
          MWrapInt64ToInt32::New(alloc(), value, /*bottomHalf=*/true);
      curBlock_->add(cvtValue);
      value = cvtValue;
    }

    MWasmLoadTls* memoryBase = maybeLoadMemoryBase();
    MInstruction* xchg =
        MWasmAtomicExchangeHeap::New(alloc(), bytecodeOffset(), memoryBase,
                                     base, *access, value, tlsPointer_);
    if (!xchg) {
      return nullptr;
    }
    curBlock_->add(xchg);

    if (isSmallerAccessForI64(result, access)) {
      xchg = MExtendInt32ToInt64::New(alloc(), xchg, true);
      curBlock_->add(xchg);
    }

    return xchg;
  }

  MDefinition* atomicBinopHeap(AtomicOp op, MDefinition* base,
                               MemoryAccessDesc* access, ValType result,
                               MDefinition* value) {
    if (inDeadCode()) {
      return nullptr;
    }

    checkOffsetAndAlignmentAndBounds(access, &base);

    if (isSmallerAccessForI64(result, access)) {
      auto* cvtValue =
          MWrapInt64ToInt32::New(alloc(), value, /*bottomHalf=*/true);
      curBlock_->add(cvtValue);
      value = cvtValue;
    }

    MWasmLoadTls* memoryBase = maybeLoadMemoryBase();
    MInstruction* binop =
        MWasmAtomicBinopHeap::New(alloc(), bytecodeOffset(), op, memoryBase,
                                  base, *access, value, tlsPointer_);
    if (!binop) {
      return nullptr;
    }
    curBlock_->add(binop);

    if (isSmallerAccessForI64(result, access)) {
      binop = MExtendInt32ToInt64::New(alloc(), binop, true);
      curBlock_->add(binop);
    }

    return binop;
  }

  MDefinition* loadGlobalVar(unsigned globalDataOffset, bool isConst,
                             bool isIndirect, MIRType type) {
    if (inDeadCode()) {
      return nullptr;
    }

    MInstruction* load;
    if (isIndirect) {
      // Pull a pointer to the value out of TlsData::globalArea, then
      // load from that pointer.  Note that the pointer is immutable
      // even though the value it points at may change, hence the use of
      // |true| for the first node's |isConst| value, irrespective of
      // the |isConst| formal parameter to this method.  The latter
      // applies to the denoted value as a whole.
      auto* cellPtr =
          MWasmLoadGlobalVar::New(alloc(), MIRType::Pointer, globalDataOffset,
                                  /*isConst=*/true, tlsPointer_);
      curBlock_->add(cellPtr);
      load = MWasmLoadGlobalCell::New(alloc(), type, cellPtr);
    } else {
      // Pull the value directly out of TlsData::globalArea.
      load = MWasmLoadGlobalVar::New(alloc(), type, globalDataOffset, isConst,
                                     tlsPointer_);
    }
    curBlock_->add(load);
    return load;
  }

  MInstruction* storeGlobalVar(uint32_t globalDataOffset, bool isIndirect,
                               MDefinition* v) {
    if (inDeadCode()) {
      return nullptr;
    }

    MInstruction* store;
    MInstruction* valueAddr = nullptr;
    if (isIndirect) {
      // Pull a pointer to the value out of TlsData::globalArea, then
      // store through that pointer.
      auto* cellPtr =
          MWasmLoadGlobalVar::New(alloc(), MIRType::Pointer, globalDataOffset,
                                  /*isConst=*/true, tlsPointer_);
      curBlock_->add(cellPtr);
      if (v->type() == MIRType::RefOrNull) {
        valueAddr = cellPtr;
        store = MWasmStoreRef::New(alloc(), tlsPointer_, valueAddr, v,
                                   AliasSet::WasmGlobalCell);
      } else {
        store = MWasmStoreGlobalCell::New(alloc(), v, cellPtr);
      }
    } else {
      // Store the value directly in TlsData::globalArea.
      if (v->type() == MIRType::RefOrNull) {
        valueAddr = MWasmDerivedPointer::New(
            alloc(), tlsPointer_,
            offsetof(wasm::TlsData, globalArea) + globalDataOffset);
        curBlock_->add(valueAddr);
        store = MWasmStoreRef::New(alloc(), tlsPointer_, valueAddr, v,
                                   AliasSet::WasmGlobalVar);
      } else {
        store =
            MWasmStoreGlobalVar::New(alloc(), globalDataOffset, v, tlsPointer_);
      }
    }
    curBlock_->add(store);

    return valueAddr;
  }

  void addInterruptCheck() {
    if (inDeadCode()) {
      return;
    }
    curBlock_->add(
        MWasmInterruptCheck::New(alloc(), tlsPointer_, bytecodeOffset()));
  }

  /***************************************************************** Calls */

  // The IonMonkey backend maintains a single stack offset (from the stack
  // pointer to the base of the frame) by adding the total amount of spill
  // space required plus the maximum stack required for argument passing.
  // Since we do not use IonMonkey's MPrepareCall/MPassArg/MCall, we must
  // manually accumulate, for the entire function, the maximum required stack
  // space for argument passing. (This is passed to the CodeGenerator via
  // MIRGenerator::maxWasmStackArgBytes.) This is just be the maximum of the
  // stack space required for each individual call (as determined by the call
  // ABI).

  // Operations that modify a CallCompileState.

  bool passInstance(MIRType instanceType, CallCompileState* args) {
    if (inDeadCode()) {
      return true;
    }

    // Should only pass an instance once.  And it must be a non-GC pointer.
    MOZ_ASSERT(args->instanceArg_ == ABIArg());
    MOZ_ASSERT(instanceType == MIRType::Pointer);
    args->instanceArg_ = args->abi_.next(MIRType::Pointer);
    return true;
  }

  // Do not call this directly.  Call one of the passArg() variants instead.
  bool passArgWorker(MDefinition* argDef, MIRType type,
                     CallCompileState* call) {
    ABIArg arg = call->abi_.next(type);
    switch (arg.kind()) {
#ifdef JS_CODEGEN_REGISTER_PAIR
      case ABIArg::GPR_PAIR: {
        auto mirLow =
            MWrapInt64ToInt32::New(alloc(), argDef, /* bottomHalf = */ true);
        curBlock_->add(mirLow);
        auto mirHigh =
            MWrapInt64ToInt32::New(alloc(), argDef, /* bottomHalf = */ false);
        curBlock_->add(mirHigh);
        return call->regArgs_.append(
                   MWasmCall::Arg(AnyRegister(arg.gpr64().low), mirLow)) &&
               call->regArgs_.append(
                   MWasmCall::Arg(AnyRegister(arg.gpr64().high), mirHigh));
      }
#endif
      case ABIArg::GPR:
      case ABIArg::FPU:
        return call->regArgs_.append(MWasmCall::Arg(arg.reg(), argDef));
      case ABIArg::Stack: {
        auto* mir =
            MWasmStackArg::New(alloc(), arg.offsetFromArgBase(), argDef);
        curBlock_->add(mir);
        return true;
      }
      case ABIArg::Uninitialized:
        MOZ_ASSERT_UNREACHABLE("Uninitialized ABIArg kind");
    }
    MOZ_CRASH("Unknown ABIArg kind.");
  }

  bool passArg(MDefinition* argDef, MIRType type, CallCompileState* call) {
    if (inDeadCode()) {
      return true;
    }
    return passArgWorker(argDef, type, call);
  }

  bool passArg(MDefinition* argDef, ValType type, CallCompileState* call) {
    if (inDeadCode()) {
      return true;
    }
    return passArgWorker(argDef, ToMIRType(type), call);
  }

  // If the call returns results on the stack, prepare a stack area to receive
  // them, and pass the address of the stack area to the callee as an additional
  // argument.
  bool passStackResultAreaCallArg(const ResultType& resultType,
                                  CallCompileState* call) {
    if (inDeadCode()) {
      return true;
    }
    ABIResultIter iter(resultType);
    while (!iter.done() && iter.cur().inRegister()) {
      iter.next();
    }
    if (iter.done()) {
      // No stack results.
      return true;
    }

    auto* stackResultArea = MWasmStackResultArea::New(alloc());
    if (!stackResultArea) {
      return false;
    }
    if (!stackResultArea->init(alloc(), iter.remaining())) {
      return false;
    }
    for (uint32_t base = iter.index(); !iter.done(); iter.next()) {
      MWasmStackResultArea::StackResult loc(iter.cur().stackOffset(),
                                            ToMIRType(iter.cur().type()));
      stackResultArea->initResult(iter.index() - base, loc);
    }
    curBlock_->add(stackResultArea);
    if (!passArg(stackResultArea, MIRType::Pointer, call)) {
      return false;
    }
    call->stackResultArea_ = stackResultArea;
    return true;
  }

  bool finishCall(CallCompileState* call) {
    if (inDeadCode()) {
      return true;
    }

    if (!call->regArgs_.append(
            MWasmCall::Arg(AnyRegister(WasmTlsReg), tlsPointer_))) {
      return false;
    }

    uint32_t stackBytes = call->abi_.stackBytesConsumedSoFar();

    maxStackArgBytes_ = std::max(maxStackArgBytes_, stackBytes);
    return true;
  }

  // Wrappers for creating various kinds of calls.

  bool collectUnaryCallResult(MIRType type, MDefinition** result) {
    MInstruction* def;
    switch (type) {
      case MIRType::Int32:
        def = MWasmRegisterResult::New(alloc(), MIRType::Int32, ReturnReg);
        break;
      case MIRType::Int64:
        def = MWasmRegister64Result::New(alloc(), ReturnReg64);
        break;
      case MIRType::Float32:
        def = MWasmFloatRegisterResult::New(alloc(), type, ReturnFloat32Reg);
        break;
      case MIRType::Double:
        def = MWasmFloatRegisterResult::New(alloc(), type, ReturnDoubleReg);
        break;
#ifdef ENABLE_WASM_SIMD
      case MIRType::Simd128:
        def = MWasmFloatRegisterResult::New(alloc(), type, ReturnSimd128Reg);
        break;
#endif
      case MIRType::RefOrNull:
        def = MWasmRegisterResult::New(alloc(), MIRType::RefOrNull, ReturnReg);
        break;
      default:
        MOZ_CRASH("unexpected MIRType result for builtin call");
    }

    if (!def) {
      return false;
    }

    curBlock_->add(def);
    *result = def;

    return true;
  }

  bool collectCallResults(const ResultType& type,
                          MWasmStackResultArea* stackResultArea,
                          DefVector* results) {
    if (!results->reserve(type.length())) {
      return false;
    }

    // The result iterator goes in the order in which results would be popped
    // off; we want the order in which they would be pushed.
    ABIResultIter iter(type);
    uint32_t stackResultCount = 0;
    while (!iter.done()) {
      if (iter.cur().onStack()) {
        stackResultCount++;
      }
      iter.next();
    }

    for (iter.switchToPrev(); !iter.done(); iter.prev()) {
      if (!mirGen().ensureBallast()) {
        return false;
      }
      const ABIResult& result = iter.cur();
      MInstruction* def;
      if (result.inRegister()) {
        switch (result.type().kind()) {
          case wasm::ValType::I32:
            def =
                MWasmRegisterResult::New(alloc(), MIRType::Int32, result.gpr());
            break;
          case wasm::ValType::I64:
            def = MWasmRegister64Result::New(alloc(), result.gpr64());
            break;
          case wasm::ValType::F32:
            def = MWasmFloatRegisterResult::New(alloc(), MIRType::Float32,
                                                result.fpr());
            break;
          case wasm::ValType::F64:
            def = MWasmFloatRegisterResult::New(alloc(), MIRType::Double,
                                                result.fpr());
            break;
          case wasm::ValType::Rtt:
          case wasm::ValType::Ref:
            def = MWasmRegisterResult::New(alloc(), MIRType::RefOrNull,
                                           result.gpr());
            break;
          case wasm::ValType::V128:
#ifdef ENABLE_WASM_SIMD
            def = MWasmFloatRegisterResult::New(alloc(), MIRType::Simd128,
                                                result.fpr());
#else
            return this->iter().fail("Ion has no SIMD support yet");
#endif
        }
      } else {
        MOZ_ASSERT(stackResultArea);
        MOZ_ASSERT(stackResultCount);
        uint32_t idx = --stackResultCount;
        def = MWasmStackResult::New(alloc(), stackResultArea, idx);
      }

      if (!def) {
        return false;
      }
      curBlock_->add(def);
      results->infallibleAppend(def);
    }

    MOZ_ASSERT(results->length() == type.length());

    return true;
  }

  bool callDirect(const FuncType& funcType, uint32_t funcIndex,
                  uint32_t lineOrBytecode, const CallCompileState& call,
                  DefVector* results) {
    if (inDeadCode()) {
      return true;
    }

    CallSiteDesc desc(lineOrBytecode, CallSiteDesc::Func);
    ResultType resultType = ResultType::Vector(funcType.results());
    auto callee = CalleeDesc::function(funcIndex);
    ArgTypeVector args(funcType);
    auto* ins = MWasmCall::New(alloc(), desc, callee, call.regArgs_,
                               StackArgAreaSizeUnaligned(args));
    if (!ins) {
      return false;
    }

    curBlock_->add(ins);

    return collectCallResults(resultType, call.stackResultArea_, results);
  }

  bool callIndirect(uint32_t funcTypeIndex, uint32_t tableIndex,
                    MDefinition* index, uint32_t lineOrBytecode,
                    const CallCompileState& call, DefVector* results) {
    if (inDeadCode()) {
      return true;
    }

    const FuncType& funcType = moduleEnv_.types[funcTypeIndex].funcType();
    const TypeIdDesc& funcTypeId = moduleEnv_.typeIds[funcTypeIndex];

    CalleeDesc callee;
    if (moduleEnv_.isAsmJS()) {
      MOZ_ASSERT(tableIndex == 0);
      MOZ_ASSERT(funcTypeId.kind() == TypeIdDescKind::None);
      const TableDesc& table =
          moduleEnv_.tables[moduleEnv_.asmJSSigToTableIndex[funcTypeIndex]];
      MOZ_ASSERT(IsPowerOfTwo(table.initialLength));

      MConstant* mask =
          MConstant::New(alloc(), Int32Value(table.initialLength - 1));
      curBlock_->add(mask);
      MBitAnd* maskedIndex = MBitAnd::New(alloc(), index, mask, MIRType::Int32);
      curBlock_->add(maskedIndex);

      index = maskedIndex;
      callee = CalleeDesc::asmJSTable(table);
    } else {
      MOZ_ASSERT(funcTypeId.kind() != TypeIdDescKind::None);
      const TableDesc& table = moduleEnv_.tables[tableIndex];
      callee = CalleeDesc::wasmTable(table, funcTypeId);
    }

    CallSiteDesc desc(lineOrBytecode, CallSiteDesc::Dynamic);
    ArgTypeVector args(funcType);
    ResultType resultType = ResultType::Vector(funcType.results());
    auto* ins = MWasmCall::New(alloc(), desc, callee, call.regArgs_,
                               StackArgAreaSizeUnaligned(args), index);
    if (!ins) {
      return false;
    }

    curBlock_->add(ins);

    return collectCallResults(resultType, call.stackResultArea_, results);
  }

  bool callImport(unsigned globalDataOffset, uint32_t lineOrBytecode,
                  const CallCompileState& call, const FuncType& funcType,
                  DefVector* results) {
    if (inDeadCode()) {
      return true;
    }

    CallSiteDesc desc(lineOrBytecode, CallSiteDesc::Dynamic);
    auto callee = CalleeDesc::import(globalDataOffset);
    ArgTypeVector args(funcType);
    ResultType resultType = ResultType::Vector(funcType.results());
    auto* ins = MWasmCall::New(alloc(), desc, callee, call.regArgs_,
                               StackArgAreaSizeUnaligned(args));
    if (!ins) {
      return false;
    }

    curBlock_->add(ins);

    return collectCallResults(resultType, call.stackResultArea_, results);
  }

  bool builtinCall(const SymbolicAddressSignature& builtin,
                   uint32_t lineOrBytecode, const CallCompileState& call,
                   MDefinition** def) {
    if (inDeadCode()) {
      *def = nullptr;
      return true;
    }

    MOZ_ASSERT(builtin.failureMode == FailureMode::Infallible);

    CallSiteDesc desc(lineOrBytecode, CallSiteDesc::Symbolic);
    auto callee = CalleeDesc::builtin(builtin.identity);
    auto* ins = MWasmCall::New(alloc(), desc, callee, call.regArgs_,
                               StackArgAreaSizeUnaligned(builtin));
    if (!ins) {
      return false;
    }

    curBlock_->add(ins);

    return collectUnaryCallResult(builtin.retType, def);
  }

  bool builtinInstanceMethodCall(const SymbolicAddressSignature& builtin,
                                 uint32_t lineOrBytecode,
                                 const CallCompileState& call,
                                 MDefinition** def = nullptr) {
    MOZ_ASSERT_IF(!def, builtin.retType == MIRType::None);
    if (inDeadCode()) {
      if (def) {
        *def = nullptr;
      }
      return true;
    }

    CallSiteDesc desc(lineOrBytecode, CallSiteDesc::Symbolic);
    auto* ins = MWasmCall::NewBuiltinInstanceMethodCall(
        alloc(), desc, builtin.identity, builtin.failureMode, call.instanceArg_,
        call.regArgs_, StackArgAreaSizeUnaligned(builtin));
    if (!ins) {
      return false;
    }

    curBlock_->add(ins);

    return def ? collectUnaryCallResult(builtin.retType, def) : true;
  }

  /*********************************************** Control flow generation */

  inline bool inDeadCode() const { return curBlock_ == nullptr; }

  bool returnValues(const DefVector& values) {
    if (inDeadCode()) {
      return true;
    }

    if (values.empty()) {
      curBlock_->end(MWasmReturnVoid::New(alloc(), tlsPointer_));
    } else {
      ResultType resultType = ResultType::Vector(funcType().results());
      ABIResultIter iter(resultType);
      // Switch to iterate in FIFO order instead of the default LIFO.
      while (!iter.done()) {
        iter.next();
      }
      iter.switchToPrev();
      for (uint32_t i = 0; !iter.done(); iter.prev(), i++) {
        if (!mirGen().ensureBallast()) {
          return false;
        }
        const ABIResult& result = iter.cur();
        if (result.onStack()) {
          MOZ_ASSERT(iter.remaining() > 1);
          if (result.type().isReference()) {
            auto* loc = MWasmDerivedPointer::New(alloc(), stackResultPointer_,
                                                 result.stackOffset());
            curBlock_->add(loc);
            auto* store =
                MWasmStoreRef::New(alloc(), tlsPointer_, loc, values[i],
                                   AliasSet::WasmStackResult);
            curBlock_->add(store);
          } else {
            auto* store = MWasmStoreStackResult::New(
                alloc(), stackResultPointer_, result.stackOffset(), values[i]);
            curBlock_->add(store);
          }
        } else {
          MOZ_ASSERT(iter.remaining() == 1);
          MOZ_ASSERT(i + 1 == values.length());
          curBlock_->end(MWasmReturn::New(alloc(), values[i], tlsPointer_));
        }
      }
    }
    curBlock_ = nullptr;
    return true;
  }

  void unreachableTrap() {
    if (inDeadCode()) {
      return;
    }

    auto* ins =
        MWasmTrap::New(alloc(), wasm::Trap::Unreachable, bytecodeOffset());
    curBlock_->end(ins);
    curBlock_ = nullptr;
  }

 private:
  static uint32_t numPushed(MBasicBlock* block) {
    return block->stackDepth() - block->info().firstStackSlot();
  }

 public:
  [[nodiscard]] bool pushDefs(const DefVector& defs) {
    if (inDeadCode()) {
      return true;
    }
    MOZ_ASSERT(numPushed(curBlock_) == 0);
    if (!curBlock_->ensureHasSlots(defs.length())) {
      return false;
    }
    for (MDefinition* def : defs) {
      MOZ_ASSERT(def->type() != MIRType::None);
      curBlock_->push(def);
    }
    return true;
  }

  bool popPushedDefs(DefVector* defs) {
    size_t n = numPushed(curBlock_);
    if (!defs->resizeUninitialized(n)) {
      return false;
    }
    for (; n > 0; n--) {
      MDefinition* def = curBlock_->pop();
      MOZ_ASSERT(def->type() != MIRType::Value);
      (*defs)[n - 1] = def;
    }
    return true;
  }

 private:
  bool addJoinPredecessor(const DefVector& defs, MBasicBlock** joinPred) {
    *joinPred = curBlock_;
    if (inDeadCode()) {
      return true;
    }
    return pushDefs(defs);
  }

 public:
  bool branchAndStartThen(MDefinition* cond, MBasicBlock** elseBlock) {
    if (inDeadCode()) {
      *elseBlock = nullptr;
    } else {
      MBasicBlock* thenBlock;
      if (!newBlock(curBlock_, &thenBlock)) {
        return false;
      }
      if (!newBlock(curBlock_, elseBlock)) {
        return false;
      }

      curBlock_->end(MTest::New(alloc(), cond, thenBlock, *elseBlock));

      curBlock_ = thenBlock;
      mirGraph().moveBlockToEnd(curBlock_);
    }

    return startBlock();
  }

  bool switchToElse(MBasicBlock* elseBlock, MBasicBlock** thenJoinPred) {
    DefVector values;
    if (!finishBlock(&values)) {
      return false;
    }

    if (!elseBlock) {
      *thenJoinPred = nullptr;
    } else {
      if (!addJoinPredecessor(values, thenJoinPred)) {
        return false;
      }

      curBlock_ = elseBlock;
      mirGraph().moveBlockToEnd(curBlock_);
    }

    return startBlock();
  }

  bool joinIfElse(MBasicBlock* thenJoinPred, DefVector* defs) {
    DefVector values;
    if (!finishBlock(&values)) {
      return false;
    }

    if (!thenJoinPred && inDeadCode()) {
      return true;
    }

    MBasicBlock* elseJoinPred;
    if (!addJoinPredecessor(values, &elseJoinPred)) {
      return false;
    }

    mozilla::Array<MBasicBlock*, 2> blocks;
    size_t numJoinPreds = 0;
    if (thenJoinPred) {
      blocks[numJoinPreds++] = thenJoinPred;
    }
    if (elseJoinPred) {
      blocks[numJoinPreds++] = elseJoinPred;
    }

    if (numJoinPreds == 0) {
      return true;
    }

    MBasicBlock* join;
    if (!goToNewBlock(blocks[0], &join)) {
      return false;
    }
    for (size_t i = 1; i < numJoinPreds; ++i) {
      if (!goToExistingBlock(blocks[i], join)) {
        return false;
      }
    }

    curBlock_ = join;
    return popPushedDefs(defs);
  }

  bool startBlock() {
    MOZ_ASSERT_IF(blockDepth_ < blockPatches_.length(),
                  blockPatches_[blockDepth_].empty());
    blockDepth_++;
    return true;
  }

  bool finishBlock(DefVector* defs) {
    MOZ_ASSERT(blockDepth_);
    uint32_t topLabel = --blockDepth_;
    return bindBranches(topLabel, defs);
  }

  bool startLoop(MBasicBlock** loopHeader, size_t paramCount) {
    *loopHeader = nullptr;

    blockDepth_++;
    loopDepth_++;

    if (inDeadCode()) {
      return true;
    }

    // Create the loop header.
    MOZ_ASSERT(curBlock_->loopDepth() == loopDepth_ - 1);
    *loopHeader = MBasicBlock::New(mirGraph(), info(), curBlock_,
                                   MBasicBlock::PENDING_LOOP_HEADER);
    if (!*loopHeader) {
      return false;
    }

    (*loopHeader)->setLoopDepth(loopDepth_);
    mirGraph().addBlock(*loopHeader);
    curBlock_->end(MGoto::New(alloc(), *loopHeader));

    DefVector loopParams;
    if (!iter().getResults(paramCount, &loopParams)) {
      return false;
    }
    for (size_t i = 0; i < paramCount; i++) {
      MPhi* phi = MPhi::New(alloc(), loopParams[i]->type());
      if (!phi) {
        return false;
      }
      if (!phi->reserveLength(2)) {
        return false;
      }
      (*loopHeader)->addPhi(phi);
      phi->addInput(loopParams[i]);
      loopParams[i] = phi;
    }
    iter().setResults(paramCount, loopParams);

    MBasicBlock* body;
    if (!goToNewBlock(*loopHeader, &body)) {
      return false;
    }
    curBlock_ = body;
    return true;
  }

 private:
  void fixupRedundantPhis(MBasicBlock* b) {
    for (size_t i = 0, depth = b->stackDepth(); i < depth; i++) {
      MDefinition* def = b->getSlot(i);
      if (def->isUnused()) {
        b->setSlot(i, def->toPhi()->getOperand(0));
      }
    }
  }

  bool setLoopBackedge(MBasicBlock* loopEntry, MBasicBlock* loopBody,
                       MBasicBlock* backedge, size_t paramCount) {
    if (!loopEntry->setBackedgeWasm(backedge, paramCount)) {
      return false;
    }

    // Flag all redundant phis as unused.
    for (MPhiIterator phi = loopEntry->phisBegin(); phi != loopEntry->phisEnd();
         phi++) {
      MOZ_ASSERT(phi->numOperands() == 2);
      if (phi->getOperand(0) == phi->getOperand(1)) {
        phi->setUnused();
      }
    }

    // Fix up phis stored in the slots Vector of pending blocks.
    for (ControlFlowPatchVector& patches : blockPatches_) {
      for (ControlFlowPatch& p : patches) {
        MBasicBlock* block = p.ins->block();
        if (block->loopDepth() >= loopEntry->loopDepth()) {
          fixupRedundantPhis(block);
        }
      }
    }

    // The loop body, if any, might be referencing recycled phis too.
    if (loopBody) {
      fixupRedundantPhis(loopBody);
    }

    // Discard redundant phis and add to the free list.
    for (MPhiIterator phi = loopEntry->phisBegin();
         phi != loopEntry->phisEnd();) {
      MPhi* entryDef = *phi++;
      if (!entryDef->isUnused()) {
        continue;
      }

      entryDef->justReplaceAllUsesWith(entryDef->getOperand(0));
      loopEntry->discardPhi(entryDef);
      mirGraph().addPhiToFreeList(entryDef);
    }

    return true;
  }

 public:
  bool closeLoop(MBasicBlock* loopHeader, DefVector* loopResults) {
    MOZ_ASSERT(blockDepth_ >= 1);
    MOZ_ASSERT(loopDepth_);

    uint32_t headerLabel = blockDepth_ - 1;

    if (!loopHeader) {
      MOZ_ASSERT(inDeadCode());
      MOZ_ASSERT(headerLabel >= blockPatches_.length() ||
                 blockPatches_[headerLabel].empty());
      blockDepth_--;
      loopDepth_--;
      return true;
    }

    // Op::Loop doesn't have an implicit backedge so temporarily set
    // aside the end of the loop body to bind backedges.
    MBasicBlock* loopBody = curBlock_;
    curBlock_ = nullptr;

    // As explained in bug 1253544, Ion apparently has an invariant that
    // there is only one backedge to loop headers. To handle wasm's ability
    // to have multiple backedges to the same loop header, we bind all those
    // branches as forward jumps to a single backward jump. This is
    // unfortunate but the optimizer is able to fold these into single jumps
    // to backedges.
    DefVector backedgeValues;
    if (!bindBranches(headerLabel, &backedgeValues)) {
      return false;
    }

    MOZ_ASSERT(loopHeader->loopDepth() == loopDepth_);

    if (curBlock_) {
      // We're on the loop backedge block, created by bindBranches.
      for (size_t i = 0, n = numPushed(curBlock_); i != n; i++) {
        curBlock_->pop();
      }

      if (!pushDefs(backedgeValues)) {
        return false;
      }

      MOZ_ASSERT(curBlock_->loopDepth() == loopDepth_);
      curBlock_->end(MGoto::New(alloc(), loopHeader));
      if (!setLoopBackedge(loopHeader, loopBody, curBlock_,
                           backedgeValues.length())) {
        return false;
      }
    }

    curBlock_ = loopBody;

    loopDepth_--;

    // If the loop depth still at the inner loop body, correct it.
    if (curBlock_ && curBlock_->loopDepth() != loopDepth_) {
      MBasicBlock* out;
      if (!goToNewBlock(curBlock_, &out)) {
        return false;
      }
      curBlock_ = out;
    }

    blockDepth_ -= 1;
    return inDeadCode() || popPushedDefs(loopResults);
  }

  bool addControlFlowPatch(MControlInstruction* ins, uint32_t relative,
                           uint32_t index) {
    MOZ_ASSERT(relative < blockDepth_);
    uint32_t absolute = blockDepth_ - 1 - relative;

    if (absolute >= blockPatches_.length() &&
        !blockPatches_.resize(absolute + 1)) {
      return false;
    }

    return blockPatches_[absolute].append(ControlFlowPatch(ins, index));
  }

  bool br(uint32_t relativeDepth, const DefVector& values) {
    if (inDeadCode()) {
      return true;
    }

    MGoto* jump = MGoto::New(alloc());
    if (!addControlFlowPatch(jump, relativeDepth, MGoto::TargetIndex)) {
      return false;
    }

    if (!pushDefs(values)) {
      return false;
    }

    curBlock_->end(jump);
    curBlock_ = nullptr;
    return true;
  }

  bool brIf(uint32_t relativeDepth, const DefVector& values,
            MDefinition* condition) {
    if (inDeadCode()) {
      return true;
    }

    MBasicBlock* joinBlock = nullptr;
    if (!newBlock(curBlock_, &joinBlock)) {
      return false;
    }

    MTest* test = MTest::New(alloc(), condition, joinBlock);
    if (!addControlFlowPatch(test, relativeDepth, MTest::TrueBranchIndex)) {
      return false;
    }

    if (!pushDefs(values)) {
      return false;
    }

    curBlock_->end(test);
    curBlock_ = joinBlock;
    return true;
  }

  bool brTable(MDefinition* operand, uint32_t defaultDepth,
               const Uint32Vector& depths, const DefVector& values) {
    if (inDeadCode()) {
      return true;
    }

    size_t numCases = depths.length();
    MOZ_ASSERT(numCases <= INT32_MAX);
    MOZ_ASSERT(numCases);

    MTableSwitch* table =
        MTableSwitch::New(alloc(), operand, 0, int32_t(numCases - 1));

    size_t defaultIndex;
    if (!table->addDefault(nullptr, &defaultIndex)) {
      return false;
    }
    if (!addControlFlowPatch(table, defaultDepth, defaultIndex)) {
      return false;
    }

    using IndexToCaseMap =
        HashMap<uint32_t, uint32_t, DefaultHasher<uint32_t>, SystemAllocPolicy>;

    IndexToCaseMap indexToCase;
    if (!indexToCase.put(defaultDepth, defaultIndex)) {
      return false;
    }

    for (size_t i = 0; i < numCases; i++) {
      uint32_t depth = depths[i];

      size_t caseIndex;
      IndexToCaseMap::AddPtr p = indexToCase.lookupForAdd(depth);
      if (!p) {
        if (!table->addSuccessor(nullptr, &caseIndex)) {
          return false;
        }
        if (!addControlFlowPatch(table, depth, caseIndex)) {
          return false;
        }
        if (!indexToCase.add(p, depth, caseIndex)) {
          return false;
        }
      } else {
        caseIndex = p->value();
      }

      if (!table->addCase(caseIndex)) {
        return false;
      }
    }

    if (!pushDefs(values)) {
      return false;
    }

    curBlock_->end(table);
    curBlock_ = nullptr;

    return true;
  }

  /************************************************************ DECODING ***/

  uint32_t readCallSiteLineOrBytecode() {
    if (!func_.callSiteLineNums.empty()) {
      return func_.callSiteLineNums[lastReadCallSite_++];
    }
    return iter_.lastOpcodeOffset();
  }

#if DEBUG
  bool done() const { return iter_.done(); }
#endif

  /*************************************************************************/
 private:
  bool newBlock(MBasicBlock* pred, MBasicBlock** block) {
    *block = MBasicBlock::New(mirGraph(), info(), pred, MBasicBlock::NORMAL);
    if (!*block) {
      return false;
    }
    mirGraph().addBlock(*block);
    (*block)->setLoopDepth(loopDepth_);
    return true;
  }

  bool goToNewBlock(MBasicBlock* pred, MBasicBlock** block) {
    if (!newBlock(pred, block)) {
      return false;
    }
    pred->end(MGoto::New(alloc(), *block));
    return true;
  }

  bool goToExistingBlock(MBasicBlock* prev, MBasicBlock* next) {
    MOZ_ASSERT(prev);
    MOZ_ASSERT(next);
    prev->end(MGoto::New(alloc(), next));
    return next->addPredecessor(alloc(), prev);
  }

  bool bindBranches(uint32_t absolute, DefVector* defs) {
    if (absolute >= blockPatches_.length() || blockPatches_[absolute].empty()) {
      return inDeadCode() || popPushedDefs(defs);
    }

    ControlFlowPatchVector& patches = blockPatches_[absolute];
    MControlInstruction* ins = patches[0].ins;
    MBasicBlock* pred = ins->block();

    MBasicBlock* join = nullptr;
    if (!newBlock(pred, &join)) {
      return false;
    }

    pred->mark();
    ins->replaceSuccessor(patches[0].index, join);

    for (size_t i = 1; i < patches.length(); i++) {
      ins = patches[i].ins;

      pred = ins->block();
      if (!pred->isMarked()) {
        if (!join->addPredecessor(alloc(), pred)) {
          return false;
        }
        pred->mark();
      }

      ins->replaceSuccessor(patches[i].index, join);
    }

    MOZ_ASSERT_IF(curBlock_, !curBlock_->isMarked());
    for (uint32_t i = 0; i < join->numPredecessors(); i++) {
      join->getPredecessor(i)->unmark();
    }

    if (curBlock_ && !goToExistingBlock(curBlock_, join)) {
      return false;
    }

    curBlock_ = join;

    if (!popPushedDefs(defs)) {
      return false;
    }

    patches.clear();
    return true;
  }
};

template <>
MDefinition* FunctionCompiler::unary<MToFloat32>(MDefinition* op) {
  if (inDeadCode()) {
    return nullptr;
  }
  auto* ins = MToFloat32::New(alloc(), op, mustPreserveNaN(op->type()));
  curBlock_->add(ins);
  return ins;
}

template <>
MDefinition* FunctionCompiler::unary<MWasmBuiltinTruncateToInt32>(
    MDefinition* op) {
  if (inDeadCode()) {
    return nullptr;
  }
  auto* ins = MWasmBuiltinTruncateToInt32::New(alloc(), op, tlsPointer_,
                                               bytecodeOffset());
  curBlock_->add(ins);
  return ins;
}

template <>
MDefinition* FunctionCompiler::unary<MNot>(MDefinition* op) {
  if (inDeadCode()) {
    return nullptr;
  }
  auto* ins = MNot::NewInt32(alloc(), op);
  curBlock_->add(ins);
  return ins;
}

template <>
MDefinition* FunctionCompiler::unary<MAbs>(MDefinition* op, MIRType type) {
  if (inDeadCode()) {
    return nullptr;
  }
  auto* ins = MAbs::NewWasm(alloc(), op, type);
  curBlock_->add(ins);
  return ins;
}

}  // end anonymous namespace

static bool EmitI32Const(FunctionCompiler& f) {
  int32_t i32;
  if (!f.iter().readI32Const(&i32)) {
    return false;
  }

  f.iter().setResult(f.constant(Int32Value(i32), MIRType::Int32));
  return true;
}

static bool EmitI64Const(FunctionCompiler& f) {
  int64_t i64;
  if (!f.iter().readI64Const(&i64)) {
    return false;
  }

  f.iter().setResult(f.constant(i64));
  return true;
}

static bool EmitF32Const(FunctionCompiler& f) {
  float f32;
  if (!f.iter().readF32Const(&f32)) {
    return false;
  }

  f.iter().setResult(f.constant(f32));
  return true;
}

static bool EmitF64Const(FunctionCompiler& f) {
  double f64;
  if (!f.iter().readF64Const(&f64)) {
    return false;
  }

  f.iter().setResult(f.constant(f64));
  return true;
}

static bool EmitBlock(FunctionCompiler& f) {
  ResultType params;
  return f.iter().readBlock(&params) && f.startBlock();
}

static bool EmitLoop(FunctionCompiler& f) {
  ResultType params;
  if (!f.iter().readLoop(&params)) {
    return false;
  }

  MBasicBlock* loopHeader;
  if (!f.startLoop(&loopHeader, params.length())) {
    return false;
  }

  f.addInterruptCheck();

  f.iter().controlItem() = loopHeader;
  return true;
}

static bool EmitIf(FunctionCompiler& f) {
  ResultType params;
  MDefinition* condition = nullptr;
  if (!f.iter().readIf(&params, &condition)) {
    return false;
  }

  MBasicBlock* elseBlock;
  if (!f.branchAndStartThen(condition, &elseBlock)) {
    return false;
  }

  f.iter().controlItem() = elseBlock;
  return true;
}

static bool EmitElse(FunctionCompiler& f) {
  ResultType paramType;
  ResultType resultType;
  DefVector thenValues;
  if (!f.iter().readElse(&paramType, &resultType, &thenValues)) {
    return false;
  }

  if (!f.pushDefs(thenValues)) {
    return false;
  }

  if (!f.switchToElse(f.iter().controlItem(), &f.iter().controlItem())) {
    return false;
  }

  return true;
}

static bool EmitEnd(FunctionCompiler& f) {
  LabelKind kind;
  ResultType type;
  DefVector preJoinDefs;
  DefVector resultsForEmptyElse;
  if (!f.iter().readEnd(&kind, &type, &preJoinDefs, &resultsForEmptyElse)) {
    return false;
  }

  MBasicBlock* block = f.iter().controlItem();
  f.iter().popEnd();

  if (!f.pushDefs(preJoinDefs)) {
    return false;
  }

  DefVector postJoinDefs;
  switch (kind) {
    case LabelKind::Body:
      MOZ_ASSERT(f.iter().controlStackEmpty());
      if (!f.finishBlock(&postJoinDefs)) {
        return false;
      }
      if (!f.returnValues(postJoinDefs)) {
        return false;
      }
      return f.iter().endFunction(f.iter().end());
    case LabelKind::Block:
      if (!f.finishBlock(&postJoinDefs)) {
        return false;
      }
      break;
    case LabelKind::Loop:
      if (!f.closeLoop(block, &postJoinDefs)) {
        return false;
      }
      break;
    case LabelKind::Then: {
      // If we didn't see an Else, create a trivial else block so that we create
      // a diamond anyway, to preserve Ion invariants.
      if (!f.switchToElse(block, &block)) {
        return false;
      }

      if (!f.pushDefs(resultsForEmptyElse)) {
        return false;
      }

      if (!f.joinIfElse(block, &postJoinDefs)) {
        return false;
      }
      break;
    }
    case LabelKind::Else:
      if (!f.joinIfElse(block, &postJoinDefs)) {
        return false;
      }
      break;
#ifdef ENABLE_WASM_EXCEPTIONS
    case LabelKind::Try:
      MOZ_CRASH("NYI");
      break;
    case LabelKind::Catch:
      MOZ_CRASH("NYI");
      break;
    case LabelKind::CatchAll:
      MOZ_CRASH("NYI");
      break;
#endif
  }

  MOZ_ASSERT_IF(!f.inDeadCode(), postJoinDefs.length() == type.length());
  f.iter().setResults(postJoinDefs.length(), postJoinDefs);

  return true;
}

static bool EmitBr(FunctionCompiler& f) {
  uint32_t relativeDepth;
  ResultType type;
  DefVector values;
  if (!f.iter().readBr(&relativeDepth, &type, &values)) {
    return false;
  }

  return f.br(relativeDepth, values);
}

static bool EmitBrIf(FunctionCompiler& f) {
  uint32_t relativeDepth;
  ResultType type;
  DefVector values;
  MDefinition* condition;
  if (!f.iter().readBrIf(&relativeDepth, &type, &values, &condition)) {
    return false;
  }

  return f.brIf(relativeDepth, values, condition);
}

static bool EmitBrTable(FunctionCompiler& f) {
  Uint32Vector depths;
  uint32_t defaultDepth;
  ResultType branchValueType;
  DefVector branchValues;
  MDefinition* index;
  if (!f.iter().readBrTable(&depths, &defaultDepth, &branchValueType,
                            &branchValues, &index)) {
    return false;
  }

  // If all the targets are the same, or there are no targets, we can just
  // use a goto. This is not just an optimization: MaybeFoldConditionBlock
  // assumes that tables have more than one successor.
  bool allSameDepth = true;
  for (uint32_t depth : depths) {
    if (depth != defaultDepth) {
      allSameDepth = false;
      break;
    }
  }

  if (allSameDepth) {
    return f.br(defaultDepth, branchValues);
  }

  return f.brTable(index, defaultDepth, depths, branchValues);
}

static bool EmitReturn(FunctionCompiler& f) {
  DefVector values;
  if (!f.iter().readReturn(&values)) {
    return false;
  }

  return f.returnValues(values);
}

static bool EmitUnreachable(FunctionCompiler& f) {
  if (!f.iter().readUnreachable()) {
    return false;
  }

  f.unreachableTrap();
  return true;
}

#ifdef ENABLE_WASM_EXCEPTIONS
static bool EmitTry(FunctionCompiler& f) {
  ResultType params;
  if (!f.iter().readTry(&params)) {
    return false;
  }

  MOZ_CRASH("NYI");
}

static bool EmitCatch(FunctionCompiler& f) {
  LabelKind kind;
  uint32_t eventIndex;
  ResultType paramType, resultType;
  DefVector tryValues;
  if (!f.iter().readCatch(&kind, &eventIndex, &paramType, &resultType,
                          &tryValues)) {
    return false;
  }

  MOZ_CRASH("NYI");
}

static bool EmitCatchAll(FunctionCompiler& f) {
  LabelKind kind;
  ResultType paramType, resultType;
  DefVector tryValues;
  if (!f.iter().readCatchAll(&kind, &paramType, &resultType, &tryValues)) {
    return false;
  }

  MOZ_CRASH("NYI");
}

static bool EmitDelegate(FunctionCompiler& f) {
  uint32_t relativeDepth;
  ResultType resultType;
  DefVector tryValues;
  if (!f.iter().readDelegate(&relativeDepth, &resultType, &tryValues)) {
    return false;
  }
  f.iter().popDelegate();

  MOZ_CRASH("NYI");
}

static bool EmitThrow(FunctionCompiler& f) {
  uint32_t exnIndex;
  DefVector argValues;
  if (!f.iter().readThrow(&exnIndex, &argValues)) {
    return false;
  }

  MOZ_CRASH("NYI");
}

static bool EmitRethrow(FunctionCompiler& f) {
  uint32_t relativeDepth;
  if (!f.iter().readRethrow(&relativeDepth)) {
    return false;
  }

  MOZ_CRASH("NYI");
}
#endif

static bool EmitCallArgs(FunctionCompiler& f, const FuncType& funcType,
                         const DefVector& args, CallCompileState* call) {
  for (size_t i = 0, n = funcType.args().length(); i < n; ++i) {
    if (!f.mirGen().ensureBallast()) {
      return false;
    }
    if (!f.passArg(args[i], funcType.args()[i], call)) {
      return false;
    }
  }

  ResultType resultType = ResultType::Vector(funcType.results());
  if (!f.passStackResultAreaCallArg(resultType, call)) {
    return false;
  }

  return f.finishCall(call);
}

static bool EmitCall(FunctionCompiler& f, bool asmJSFuncDef) {
  uint32_t lineOrBytecode = f.readCallSiteLineOrBytecode();

  uint32_t funcIndex;
  DefVector args;
  if (asmJSFuncDef) {
    if (!f.iter().readOldCallDirect(f.moduleEnv().numFuncImports(), &funcIndex,
                                    &args)) {
      return false;
    }
  } else {
    if (!f.iter().readCall(&funcIndex, &args)) {
      return false;
    }
  }

  if (f.inDeadCode()) {
    return true;
  }

  const FuncType& funcType = *f.moduleEnv().funcs[funcIndex].type;

  CallCompileState call;
  if (!EmitCallArgs(f, funcType, args, &call)) {
    return false;
  }

  DefVector results;
  if (f.moduleEnv().funcIsImport(funcIndex)) {
    uint32_t globalDataOffset =
        f.moduleEnv().funcImportGlobalDataOffsets[funcIndex];
    if (!f.callImport(globalDataOffset, lineOrBytecode, call, funcType,
                      &results)) {
      return false;
    }
  } else {
    if (!f.callDirect(funcType, funcIndex, lineOrBytecode, call, &results)) {
      return false;
    }
  }

  f.iter().setResults(results.length(), results);
  return true;
}

static bool EmitCallIndirect(FunctionCompiler& f, bool oldStyle) {
  uint32_t lineOrBytecode = f.readCallSiteLineOrBytecode();

  uint32_t funcTypeIndex;
  uint32_t tableIndex;
  MDefinition* callee;
  DefVector args;
  if (oldStyle) {
    tableIndex = 0;
    if (!f.iter().readOldCallIndirect(&funcTypeIndex, &callee, &args)) {
      return false;
    }
  } else {
    if (!f.iter().readCallIndirect(&funcTypeIndex, &tableIndex, &callee,
                                   &args)) {
      return false;
    }
  }

  if (f.inDeadCode()) {
    return true;
  }

  const FuncType& funcType = f.moduleEnv().types[funcTypeIndex].funcType();

  CallCompileState call;
  if (!EmitCallArgs(f, funcType, args, &call)) {
    return false;
  }

  DefVector results;
  if (!f.callIndirect(funcTypeIndex, tableIndex, callee, lineOrBytecode, call,
                      &results)) {
    return false;
  }

  f.iter().setResults(results.length(), results);
  return true;
}

static bool EmitGetLocal(FunctionCompiler& f) {
  uint32_t id;
  if (!f.iter().readGetLocal(f.locals(), &id)) {
    return false;
  }

  f.iter().setResult(f.getLocalDef(id));
  return true;
}

static bool EmitSetLocal(FunctionCompiler& f) {
  uint32_t id;
  MDefinition* value;
  if (!f.iter().readSetLocal(f.locals(), &id, &value)) {
    return false;
  }

  f.assign(id, value);
  return true;
}

static bool EmitTeeLocal(FunctionCompiler& f) {
  uint32_t id;
  MDefinition* value;
  if (!f.iter().readTeeLocal(f.locals(), &id, &value)) {
    return false;
  }

  f.assign(id, value);
  return true;
}

static bool EmitGetGlobal(FunctionCompiler& f) {
  uint32_t id;
  if (!f.iter().readGetGlobal(&id)) {
    return false;
  }

  const GlobalDesc& global = f.moduleEnv().globals[id];
  if (!global.isConstant()) {
    f.iter().setResult(f.loadGlobalVar(global.offset(), !global.isMutable(),
                                       global.isIndirect(),
                                       ToMIRType(global.type())));
    return true;
  }

  LitVal value = global.constantValue();
  MIRType mirType = ToMIRType(value.type());

  MDefinition* result;
  switch (value.type().kind()) {
    case ValType::I32:
      result = f.constant(Int32Value(value.i32()), mirType);
      break;
    case ValType::I64:
      result = f.constant(int64_t(value.i64()));
      break;
    case ValType::F32:
      result = f.constant(value.f32());
      break;
    case ValType::F64:
      result = f.constant(value.f64());
      break;
    case ValType::V128:
#ifdef ENABLE_WASM_SIMD
      result = f.constant(value.v128());
      break;
#else
      return f.iter().fail("Ion has no SIMD support yet");
#endif
    case ValType::Ref:
      switch (value.type().refTypeKind()) {
        case RefType::Func:
        case RefType::Extern:
        case RefType::Eq:
          MOZ_ASSERT(value.ref().isNull());
          result = f.nullRefConstant();
          break;
        case RefType::TypeIndex:
          MOZ_CRASH("unexpected reference type in EmitGetGlobal");
      }
      break;
    default:
      MOZ_CRASH("unexpected type in EmitGetGlobal");
  }

  f.iter().setResult(result);
  return true;
}

static bool EmitSetGlobal(FunctionCompiler& f) {
  uint32_t lineOrBytecode = f.readCallSiteLineOrBytecode();

  uint32_t id;
  MDefinition* value;
  if (!f.iter().readSetGlobal(&id, &value)) {
    return false;
  }

  const GlobalDesc& global = f.moduleEnv().globals[id];
  MOZ_ASSERT(global.isMutable());
  MInstruction* barrierAddr =
      f.storeGlobalVar(global.offset(), global.isIndirect(), value);

  // We always call the C++ postbarrier because the location will never be in
  // the nursery, and the value stored will very frequently be in the nursery.
  // The C++ postbarrier performs any necessary filtering.

  if (barrierAddr) {
    const SymbolicAddressSignature& callee = SASigPostBarrierFiltering;
    CallCompileState args;
    if (!f.passInstance(callee.argTypes[0], &args)) {
      return false;
    }
    if (!f.passArg(barrierAddr, callee.argTypes[1], &args)) {
      return false;
    }
    f.finishCall(&args);
    if (!f.builtinInstanceMethodCall(callee, lineOrBytecode, args)) {
      return false;
    }
  }

  return true;
}

static bool EmitTeeGlobal(FunctionCompiler& f) {
  uint32_t id;
  MDefinition* value;
  if (!f.iter().readTeeGlobal(&id, &value)) {
    return false;
  }

  const GlobalDesc& global = f.moduleEnv().globals[id];
  MOZ_ASSERT(global.isMutable());

  f.storeGlobalVar(global.offset(), global.isIndirect(), value);
  return true;
}

template <typename MIRClass>
static bool EmitUnary(FunctionCompiler& f, ValType operandType) {
  MDefinition* input;
  if (!f.iter().readUnary(operandType, &input)) {
    return false;
  }

  f.iter().setResult(f.unary<MIRClass>(input));
  return true;
}

template <typename MIRClass>
static bool EmitConversion(FunctionCompiler& f, ValType operandType,
                           ValType resultType) {
  MDefinition* input;
  if (!f.iter().readConversion(operandType, resultType, &input)) {
    return false;
  }

  f.iter().setResult(f.unary<MIRClass>(input));
  return true;
}

template <typename MIRClass>
static bool EmitUnaryWithType(FunctionCompiler& f, ValType operandType,
                              MIRType mirType) {
  MDefinition* input;
  if (!f.iter().readUnary(operandType, &input)) {
    return false;
  }

  f.iter().setResult(f.unary<MIRClass>(input, mirType));
  return true;
}

template <typename MIRClass>
static bool EmitConversionWithType(FunctionCompiler& f, ValType operandType,
                                   ValType resultType, MIRType mirType) {
  MDefinition* input;
  if (!f.iter().readConversion(operandType, resultType, &input)) {
    return false;
  }

  f.iter().setResult(f.unary<MIRClass>(input, mirType));
  return true;
}

static bool EmitTruncate(FunctionCompiler& f, ValType operandType,
                         ValType resultType, bool isUnsigned,
                         bool isSaturating) {
  MDefinition* input = nullptr;
  if (!f.iter().readConversion(operandType, resultType, &input)) {
    return false;
  }

  TruncFlags flags = 0;
  if (isUnsigned) {
    flags |= TRUNC_UNSIGNED;
  }
  if (isSaturating) {
    flags |= TRUNC_SATURATING;
  }
  if (resultType == ValType::I32) {
    if (f.moduleEnv().isAsmJS()) {
      if (input && (input->type() == MIRType::Double ||
                    input->type() == MIRType::Float32)) {
        f.iter().setResult(f.unary<MWasmBuiltinTruncateToInt32>(input));
      } else {
        f.iter().setResult(f.unary<MTruncateToInt32>(input));
      }
    } else {
      f.iter().setResult(f.truncate<MWasmTruncateToInt32>(input, flags));
    }
  } else {
    MOZ_ASSERT(resultType == ValType::I64);
    MOZ_ASSERT(!f.moduleEnv().isAsmJS());
#if defined(JS_CODEGEN_ARM)
    f.iter().setResult(f.truncateWithTls(input, flags));
#else
    f.iter().setResult(f.truncate<MWasmTruncateToInt64>(input, flags));
#endif
  }
  return true;
}

static bool EmitSignExtend(FunctionCompiler& f, uint32_t srcSize,
                           uint32_t targetSize) {
  MDefinition* input;
  ValType type = targetSize == 4 ? ValType::I32 : ValType::I64;
  if (!f.iter().readConversion(type, type, &input)) {
    return false;
  }

  f.iter().setResult(f.signExtend(input, srcSize, targetSize));
  return true;
}

static bool EmitExtendI32(FunctionCompiler& f, bool isUnsigned) {
  MDefinition* input;
  if (!f.iter().readConversion(ValType::I32, ValType::I64, &input)) {
    return false;
  }

  f.iter().setResult(f.extendI32(input, isUnsigned));
  return true;
}

static bool EmitConvertI64ToFloatingPoint(FunctionCompiler& f,
                                          ValType resultType, MIRType mirType,
                                          bool isUnsigned) {
  MDefinition* input;
  if (!f.iter().readConversion(ValType::I64, resultType, &input)) {
    return false;
  }

  f.iter().setResult(f.convertI64ToFloatingPoint(input, mirType, isUnsigned));
  return true;
}

static bool EmitReinterpret(FunctionCompiler& f, ValType resultType,
                            ValType operandType, MIRType mirType) {
  MDefinition* input;
  if (!f.iter().readConversion(operandType, resultType, &input)) {
    return false;
  }

  f.iter().setResult(f.unary<MWasmReinterpret>(input, mirType));
  return true;
}

static bool EmitAdd(FunctionCompiler& f, ValType type, MIRType mirType) {
  MDefinition* lhs;
  MDefinition* rhs;
  if (!f.iter().readBinary(type, &lhs, &rhs)) {
    return false;
  }

  f.iter().setResult(f.add(lhs, rhs, mirType));
  return true;
}

static bool EmitSub(FunctionCompiler& f, ValType type, MIRType mirType) {
  MDefinition* lhs;
  MDefinition* rhs;
  if (!f.iter().readBinary(type, &lhs, &rhs)) {
    return false;
  }

  f.iter().setResult(f.sub(lhs, rhs, mirType));
  return true;
}

static bool EmitRotate(FunctionCompiler& f, ValType type, bool isLeftRotation) {
  MDefinition* lhs;
  MDefinition* rhs;
  if (!f.iter().readBinary(type, &lhs, &rhs)) {
    return false;
  }

  MDefinition* result = f.rotate(lhs, rhs, ToMIRType(type), isLeftRotation);
  f.iter().setResult(result);
  return true;
}

static bool EmitBitNot(FunctionCompiler& f, ValType operandType) {
  MDefinition* input;
  if (!f.iter().readUnary(operandType, &input)) {
    return false;
  }

  f.iter().setResult(f.bitnot(input));
  return true;
}

template <typename MIRClass>
static bool EmitBitwise(FunctionCompiler& f, ValType operandType,
                        MIRType mirType) {
  MDefinition* lhs;
  MDefinition* rhs;
  if (!f.iter().readBinary(operandType, &lhs, &rhs)) {
    return false;
  }

  f.iter().setResult(f.binary<MIRClass>(lhs, rhs, mirType));
  return true;
}

static bool EmitUrsh(FunctionCompiler& f, ValType operandType,
                     MIRType mirType) {
  MDefinition* lhs;
  MDefinition* rhs;
  if (!f.iter().readBinary(operandType, &lhs, &rhs)) {
    return false;
  }

  f.iter().setResult(f.ursh(lhs, rhs, mirType));
  return true;
}

static bool EmitMul(FunctionCompiler& f, ValType operandType, MIRType mirType) {
  MDefinition* lhs;
  MDefinition* rhs;
  if (!f.iter().readBinary(operandType, &lhs, &rhs)) {
    return false;
  }

  f.iter().setResult(
      f.mul(lhs, rhs, mirType,
            mirType == MIRType::Int32 ? MMul::Integer : MMul::Normal));
  return true;
}

static bool EmitDiv(FunctionCompiler& f, ValType operandType, MIRType mirType,
                    bool isUnsigned) {
  MDefinition* lhs;
  MDefinition* rhs;
  if (!f.iter().readBinary(operandType, &lhs, &rhs)) {
    return false;
  }

  f.iter().setResult(f.div(lhs, rhs, mirType, isUnsigned));
  return true;
}

static bool EmitRem(FunctionCompiler& f, ValType operandType, MIRType mirType,
                    bool isUnsigned) {
  MDefinition* lhs;
  MDefinition* rhs;
  if (!f.iter().readBinary(operandType, &lhs, &rhs)) {
    return false;
  }

  f.iter().setResult(f.mod(lhs, rhs, mirType, isUnsigned));
  return true;
}

static bool EmitMinMax(FunctionCompiler& f, ValType operandType,
                       MIRType mirType, bool isMax) {
  MDefinition* lhs;
  MDefinition* rhs;
  if (!f.iter().readBinary(operandType, &lhs, &rhs)) {
    return false;
  }

  f.iter().setResult(f.minMax(lhs, rhs, mirType, isMax));
  return true;
}

static bool EmitCopySign(FunctionCompiler& f, ValType operandType) {
  MDefinition* lhs;
  MDefinition* rhs;
  if (!f.iter().readBinary(operandType, &lhs, &rhs)) {
    return false;
  }

  f.iter().setResult(f.binary<MCopySign>(lhs, rhs, ToMIRType(operandType)));
  return true;
}

static bool EmitComparison(FunctionCompiler& f, ValType operandType,
                           JSOp compareOp, MCompare::CompareType compareType) {
  MDefinition* lhs;
  MDefinition* rhs;
  if (!f.iter().readComparison(operandType, &lhs, &rhs)) {
    return false;
  }

  f.iter().setResult(f.compare(lhs, rhs, compareOp, compareType));
  return true;
}

static bool EmitSelect(FunctionCompiler& f, bool typed) {
  StackType type;
  MDefinition* trueValue;
  MDefinition* falseValue;
  MDefinition* condition;
  if (!f.iter().readSelect(typed, &type, &trueValue, &falseValue, &condition)) {
    return false;
  }

  f.iter().setResult(f.select(trueValue, falseValue, condition));
  return true;
}

static bool EmitLoad(FunctionCompiler& f, ValType type, Scalar::Type viewType) {
  LinearMemoryAddress<MDefinition*> addr;
  if (!f.iter().readLoad(type, Scalar::byteSize(viewType), &addr)) {
    return false;
  }

  MemoryAccessDesc access(viewType, addr.align, addr.offset,
                          f.bytecodeIfNotAsmJS());
  auto* ins = f.load(addr.base, &access, type);
  if (!f.inDeadCode() && !ins) {
    return false;
  }

  f.iter().setResult(ins);
  return true;
}

static bool EmitStore(FunctionCompiler& f, ValType resultType,
                      Scalar::Type viewType) {
  LinearMemoryAddress<MDefinition*> addr;
  MDefinition* value;
  if (!f.iter().readStore(resultType, Scalar::byteSize(viewType), &addr,
                          &value)) {
    return false;
  }

  MemoryAccessDesc access(viewType, addr.align, addr.offset,
                          f.bytecodeIfNotAsmJS());

  f.store(addr.base, &access, value);
  return true;
}

static bool EmitTeeStore(FunctionCompiler& f, ValType resultType,
                         Scalar::Type viewType) {
  LinearMemoryAddress<MDefinition*> addr;
  MDefinition* value;
  if (!f.iter().readTeeStore(resultType, Scalar::byteSize(viewType), &addr,
                             &value)) {
    return false;
  }

  MemoryAccessDesc access(viewType, addr.align, addr.offset,
                          f.bytecodeIfNotAsmJS());

  f.store(addr.base, &access, value);
  return true;
}

static bool EmitTeeStoreWithCoercion(FunctionCompiler& f, ValType resultType,
                                     Scalar::Type viewType) {
  LinearMemoryAddress<MDefinition*> addr;
  MDefinition* value;
  if (!f.iter().readTeeStore(resultType, Scalar::byteSize(viewType), &addr,
                             &value)) {
    return false;
  }

  if (resultType == ValType::F32 && viewType == Scalar::Float64) {
    value = f.unary<MToDouble>(value);
  } else if (resultType == ValType::F64 && viewType == Scalar::Float32) {
    value = f.unary<MToFloat32>(value);
  } else {
    MOZ_CRASH("unexpected coerced store");
  }

  MemoryAccessDesc access(viewType, addr.align, addr.offset,
                          f.bytecodeIfNotAsmJS());

  f.store(addr.base, &access, value);
  return true;
}

static bool TryInlineUnaryBuiltin(FunctionCompiler& f, SymbolicAddress callee,
                                  MDefinition* input) {
  if (!input) {
    return false;
  }

  MOZ_ASSERT(IsFloatingPointType(input->type()));

  RoundingMode mode;
  if (!IsRoundingFunction(callee, &mode)) {
    return false;
  }

  if (!MNearbyInt::HasAssemblerSupport(mode)) {
    return false;
  }

  f.iter().setResult(f.nearbyInt(input, mode));
  return true;
}

static bool EmitUnaryMathBuiltinCall(FunctionCompiler& f,
                                     const SymbolicAddressSignature& callee) {
  MOZ_ASSERT(callee.numArgs == 1);

  uint32_t lineOrBytecode = f.readCallSiteLineOrBytecode();

  MDefinition* input;
  if (!f.iter().readUnary(ValType(callee.argTypes[0]), &input)) {
    return false;
  }

  if (TryInlineUnaryBuiltin(f, callee.identity, input)) {
    return true;
  }

  CallCompileState call;
  if (!f.passArg(input, callee.argTypes[0], &call)) {
    return false;
  }

  if (!f.finishCall(&call)) {
    return false;
  }

  MDefinition* def;
  if (!f.builtinCall(callee, lineOrBytecode, call, &def)) {
    return false;
  }

  f.iter().setResult(def);
  return true;
}

static bool EmitBinaryMathBuiltinCall(FunctionCompiler& f,
                                      const SymbolicAddressSignature& callee) {
  MOZ_ASSERT(callee.numArgs == 2);
  MOZ_ASSERT(callee.argTypes[0] == callee.argTypes[1]);

  uint32_t lineOrBytecode = f.readCallSiteLineOrBytecode();

  CallCompileState call;
  MDefinition* lhs;
  MDefinition* rhs;
  // This call to readBinary assumes both operands have the same type.
  if (!f.iter().readBinary(ValType(callee.argTypes[0]), &lhs, &rhs)) {
    return false;
  }

  if (!f.passArg(lhs, callee.argTypes[0], &call)) {
    return false;
  }

  if (!f.passArg(rhs, callee.argTypes[1], &call)) {
    return false;
  }

  if (!f.finishCall(&call)) {
    return false;
  }

  MDefinition* def;
  if (!f.builtinCall(callee, lineOrBytecode, call, &def)) {
    return false;
  }

  f.iter().setResult(def);
  return true;
}

static bool EmitMemoryGrow(FunctionCompiler& f) {
  uint32_t lineOrBytecode = f.readCallSiteLineOrBytecode();

  const SymbolicAddressSignature& callee = SASigMemoryGrow;
  CallCompileState args;
  if (!f.passInstance(callee.argTypes[0], &args)) {
    return false;
  }

  MDefinition* delta;
  if (!f.iter().readMemoryGrow(&delta)) {
    return false;
  }

  if (!f.passArg(delta, callee.argTypes[1], &args)) {
    return false;
  }

  f.finishCall(&args);

  MDefinition* ret;
  if (!f.builtinInstanceMethodCall(callee, lineOrBytecode, args, &ret)) {
    return false;
  }

  f.iter().setResult(ret);
  return true;
}

static bool EmitMemorySize(FunctionCompiler& f) {
  uint32_t lineOrBytecode = f.readCallSiteLineOrBytecode();

  const SymbolicAddressSignature& callee = SASigMemorySize;
  CallCompileState args;

  if (!f.iter().readMemorySize()) {
    return false;
  }

  if (!f.passInstance(callee.argTypes[0], &args)) {
    return false;
  }

  f.finishCall(&args);

  MDefinition* ret;
  if (!f.builtinInstanceMethodCall(callee, lineOrBytecode, args, &ret)) {
    return false;
  }

  f.iter().setResult(ret);
  return true;
}

static bool EmitAtomicCmpXchg(FunctionCompiler& f, ValType type,
                              Scalar::Type viewType) {
  LinearMemoryAddress<MDefinition*> addr;
  MDefinition* oldValue;
  MDefinition* newValue;
  if (!f.iter().readAtomicCmpXchg(&addr, type, byteSize(viewType), &oldValue,
                                  &newValue)) {
    return false;
  }

  MemoryAccessDesc access(viewType, addr.align, addr.offset, f.bytecodeOffset(),
                          Synchronization::Full());
  auto* ins =
      f.atomicCompareExchangeHeap(addr.base, &access, type, oldValue, newValue);
  if (!f.inDeadCode() && !ins) {
    return false;
  }

  f.iter().setResult(ins);
  return true;
}

static bool EmitAtomicLoad(FunctionCompiler& f, ValType type,
                           Scalar::Type viewType) {
  LinearMemoryAddress<MDefinition*> addr;
  if (!f.iter().readAtomicLoad(&addr, type, byteSize(viewType))) {
    return false;
  }

  MemoryAccessDesc access(viewType, addr.align, addr.offset, f.bytecodeOffset(),
                          Synchronization::Load());
  auto* ins = f.load(addr.base, &access, type);
  if (!f.inDeadCode() && !ins) {
    return false;
  }

  f.iter().setResult(ins);
  return true;
}

static bool EmitAtomicRMW(FunctionCompiler& f, ValType type,
                          Scalar::Type viewType, jit::AtomicOp op) {
  LinearMemoryAddress<MDefinition*> addr;
  MDefinition* value;
  if (!f.iter().readAtomicRMW(&addr, type, byteSize(viewType), &value)) {
    return false;
  }

  MemoryAccessDesc access(viewType, addr.align, addr.offset, f.bytecodeOffset(),
                          Synchronization::Full());
  auto* ins = f.atomicBinopHeap(op, addr.base, &access, type, value);
  if (!f.inDeadCode() && !ins) {
    return false;
  }

  f.iter().setResult(ins);
  return true;
}

static bool EmitAtomicStore(FunctionCompiler& f, ValType type,
                            Scalar::Type viewType) {
  LinearMemoryAddress<MDefinition*> addr;
  MDefinition* value;
  if (!f.iter().readAtomicStore(&addr, type, byteSize(viewType), &value)) {
    return false;
  }

  MemoryAccessDesc access(viewType, addr.align, addr.offset, f.bytecodeOffset(),
                          Synchronization::Store());
  f.store(addr.base, &access, value);
  return true;
}

static bool EmitWait(FunctionCompiler& f, ValType type, uint32_t byteSize) {
  MOZ_ASSERT(type == ValType::I32 || type == ValType::I64);
  MOZ_ASSERT(SizeOf(type) == byteSize);

  uint32_t lineOrBytecode = f.readCallSiteLineOrBytecode();

  const SymbolicAddressSignature& callee =
      type == ValType::I32 ? SASigWaitI32 : SASigWaitI64;
  CallCompileState args;
  if (!f.passInstance(callee.argTypes[0], &args)) {
    return false;
  }

  LinearMemoryAddress<MDefinition*> addr;
  MDefinition* expected;
  MDefinition* timeout;
  if (!f.iter().readWait(&addr, type, byteSize, &expected, &timeout)) {
    return false;
  }

  MemoryAccessDesc access(type == ValType::I32 ? Scalar::Int32 : Scalar::Int64,
                          addr.align, addr.offset, f.bytecodeOffset());
  MDefinition* ptr = f.computeEffectiveAddress(addr.base, &access);
  if (!f.inDeadCode() && !ptr) {
    return false;
  }

  if (!f.passArg(ptr, callee.argTypes[1], &args)) {
    return false;
  }

  MOZ_ASSERT(ToMIRType(type) == callee.argTypes[2]);
  if (!f.passArg(expected, callee.argTypes[2], &args)) {
    return false;
  }

  if (!f.passArg(timeout, callee.argTypes[3], &args)) {
    return false;
  }

  if (!f.finishCall(&args)) {
    return false;
  }

  MDefinition* ret;
  if (!f.builtinInstanceMethodCall(callee, lineOrBytecode, args, &ret)) {
    return false;
  }

  f.iter().setResult(ret);
  return true;
}

static bool EmitFence(FunctionCompiler& f) {
  if (!f.iter().readFence()) {
    return false;
  }

  f.fence();
  return true;
}

static bool EmitWake(FunctionCompiler& f) {
  uint32_t lineOrBytecode = f.readCallSiteLineOrBytecode();

  const SymbolicAddressSignature& callee = SASigWake;
  CallCompileState args;
  if (!f.passInstance(callee.argTypes[0], &args)) {
    return false;
  }

  LinearMemoryAddress<MDefinition*> addr;
  MDefinition* count;
  if (!f.iter().readWake(&addr, &count)) {
    return false;
  }

  MemoryAccessDesc access(Scalar::Int32, addr.align, addr.offset,
                          f.bytecodeOffset());
  MDefinition* ptr = f.computeEffectiveAddress(addr.base, &access);
  if (!f.inDeadCode() && !ptr) {
    return false;
  }

  if (!f.passArg(ptr, callee.argTypes[1], &args)) {
    return false;
  }

  if (!f.passArg(count, callee.argTypes[2], &args)) {
    return false;
  }

  if (!f.finishCall(&args)) {
    return false;
  }

  MDefinition* ret;
  if (!f.builtinInstanceMethodCall(callee, lineOrBytecode, args, &ret)) {
    return false;
  }

  f.iter().setResult(ret);
  return true;
}

static bool EmitAtomicXchg(FunctionCompiler& f, ValType type,
                           Scalar::Type viewType) {
  LinearMemoryAddress<MDefinition*> addr;
  MDefinition* value;
  if (!f.iter().readAtomicRMW(&addr, type, byteSize(viewType), &value)) {
    return false;
  }

  MemoryAccessDesc access(viewType, addr.align, addr.offset, f.bytecodeOffset(),
                          Synchronization::Full());
  MDefinition* ins = f.atomicExchangeHeap(addr.base, &access, type, value);
  if (!f.inDeadCode() && !ins) {
    return false;
  }

  f.iter().setResult(ins);
  return true;
}

static bool EmitMemCopyCall(FunctionCompiler& f, MDefinition* dst,
                            MDefinition* src, MDefinition* len) {
  uint32_t lineOrBytecode = f.readCallSiteLineOrBytecode();

  const SymbolicAddressSignature& callee =
      (f.moduleEnv().usesSharedMemory() ? SASigMemCopyShared32
                                        : SASigMemCopy32);
  CallCompileState args;
  if (!f.passInstance(callee.argTypes[0], &args)) {
    return false;
  }

  if (!f.passArg(dst, callee.argTypes[1], &args)) {
    return false;
  }
  if (!f.passArg(src, callee.argTypes[2], &args)) {
    return false;
  }
  if (!f.passArg(len, callee.argTypes[3], &args)) {
    return false;
  }
  MDefinition* memoryBase = f.memoryBase();
  if (!f.passArg(memoryBase, callee.argTypes[4], &args)) {
    return false;
  }
  if (!f.finishCall(&args)) {
    return false;
  }

  return f.builtinInstanceMethodCall(callee, lineOrBytecode, args);
}

static bool EmitMemCopyInline(FunctionCompiler& f, MDefinition* dst,
                              MDefinition* src, MDefinition* len) {
  MOZ_ASSERT(MaxInlineMemoryCopyLength != 0);

  MOZ_ASSERT(len->isConstant() && len->type() == MIRType::Int32);
  uint32_t length = len->toConstant()->toInt32();
  MOZ_ASSERT(length != 0 && length <= MaxInlineMemoryCopyLength);

  // Compute the number of copies of each width we will need to do
  size_t remainder = length;
#ifdef JS_64BIT
  size_t numCopies8 = remainder / sizeof(uint64_t);
  remainder %= sizeof(uint64_t);
#endif
  size_t numCopies4 = remainder / sizeof(uint32_t);
  remainder %= sizeof(uint32_t);
  size_t numCopies2 = remainder / sizeof(uint16_t);
  remainder %= sizeof(uint16_t);
  size_t numCopies1 = remainder;

  // Load all source bytes from low to high using the widest transfer width we
  // can for the system. We will trap without writing anything if any source
  // byte is out-of-bounds.
  size_t offset = 0;
  DefVector loadedValues;

#ifdef JS_64BIT
  for (uint32_t i = 0; i < numCopies8; i++) {
    MemoryAccessDesc access(Scalar::Int64, 1, offset, f.bytecodeOffset());
    auto* load = f.load(src, &access, ValType::I64);
    if (!load || !loadedValues.append(load)) {
      return false;
    }

    offset += sizeof(uint64_t);
  }
#endif

  for (uint32_t i = 0; i < numCopies4; i++) {
    MemoryAccessDesc access(Scalar::Uint32, 1, offset, f.bytecodeOffset());
    auto* load = f.load(src, &access, ValType::I32);
    if (!load || !loadedValues.append(load)) {
      return false;
    }

    offset += sizeof(uint32_t);
  }

  if (numCopies2) {
    MemoryAccessDesc access(Scalar::Uint16, 1, offset, f.bytecodeOffset());
    auto* load = f.load(src, &access, ValType::I32);
    if (!load || !loadedValues.append(load)) {
      return false;
    }

    offset += sizeof(uint16_t);
  }

  if (numCopies1) {
    MemoryAccessDesc access(Scalar::Uint8, 1, offset, f.bytecodeOffset());
    auto* load = f.load(src, &access, ValType::I32);
    if (!load || !loadedValues.append(load)) {
      return false;
    }
  }

  // Store all source bytes to the destination from high to low. We will trap
  // without writing anything on the first store if any dest byte is
  // out-of-bounds.
  offset = length;

  if (numCopies1) {
    offset -= sizeof(uint8_t);

    MemoryAccessDesc access(Scalar::Uint8, 1, offset, f.bytecodeOffset());
    auto* value = loadedValues.popCopy();
    f.store(dst, &access, value);
  }

  if (numCopies2) {
    offset -= sizeof(uint16_t);

    MemoryAccessDesc access(Scalar::Uint16, 1, offset, f.bytecodeOffset());
    auto* value = loadedValues.popCopy();
    f.store(dst, &access, value);
  }

  for (uint32_t i = 0; i < numCopies4; i++) {
    offset -= sizeof(uint32_t);

    MemoryAccessDesc access(Scalar::Uint32, 1, offset, f.bytecodeOffset());
    auto* value = loadedValues.popCopy();
    f.store(dst, &access, value);
  }

#ifdef JS_64BIT
  for (uint32_t i = 0; i < numCopies8; i++) {
    offset -= sizeof(uint64_t);

    MemoryAccessDesc access(Scalar::Int64, 1, offset, f.bytecodeOffset());
    auto* value = loadedValues.popCopy();
    f.store(dst, &access, value);
  }
#endif

  return true;
}

static bool EmitMemCopy(FunctionCompiler& f) {
  MDefinition *dst, *src, *len;
  uint32_t dstMemIndex;
  uint32_t srcMemIndex;
  if (!f.iter().readMemOrTableCopy(true, &dstMemIndex, &dst, &srcMemIndex, &src,
                                   &len)) {
    return false;
  }

  if (f.inDeadCode()) {
    return true;
  }

  if (MacroAssembler::SupportsFastUnalignedAccesses() && len->isConstant() &&
      len->type() == MIRType::Int32 && len->toConstant()->toInt32() != 0 &&
      uint32_t(len->toConstant()->toInt32()) <= MaxInlineMemoryCopyLength) {
    return EmitMemCopyInline(f, dst, src, len);
  }
  return EmitMemCopyCall(f, dst, src, len);
}

static bool EmitTableCopy(FunctionCompiler& f) {
  MDefinition *dst, *src, *len;
  uint32_t dstTableIndex;
  uint32_t srcTableIndex;
  if (!f.iter().readMemOrTableCopy(false, &dstTableIndex, &dst, &srcTableIndex,
                                   &src, &len)) {
    return false;
  }

  if (f.inDeadCode()) {
    return true;
  }

  uint32_t lineOrBytecode = f.readCallSiteLineOrBytecode();

  const SymbolicAddressSignature& callee = SASigTableCopy;
  CallCompileState args;
  if (!f.passInstance(callee.argTypes[0], &args)) {
    return false;
  }

  if (!f.passArg(dst, callee.argTypes[1], &args)) {
    return false;
  }
  if (!f.passArg(src, callee.argTypes[2], &args)) {
    return false;
  }
  if (!f.passArg(len, callee.argTypes[3], &args)) {
    return false;
  }
  MDefinition* dti = f.constant(Int32Value(dstTableIndex), MIRType::Int32);
  if (!dti) {
    return false;
  }
  if (!f.passArg(dti, callee.argTypes[4], &args)) {
    return false;
  }
  MDefinition* sti = f.constant(Int32Value(srcTableIndex), MIRType::Int32);
  if (!sti) {
    return false;
  }
  if (!f.passArg(sti, callee.argTypes[5], &args)) {
    return false;
  }
  if (!f.finishCall(&args)) {
    return false;
  }

  return f.builtinInstanceMethodCall(callee, lineOrBytecode, args);
}

static bool EmitDataOrElemDrop(FunctionCompiler& f, bool isData) {
  uint32_t segIndexVal = 0;
  if (!f.iter().readDataOrElemDrop(isData, &segIndexVal)) {
    return false;
  }

  if (f.inDeadCode()) {
    return true;
  }

  uint32_t lineOrBytecode = f.readCallSiteLineOrBytecode();

  const SymbolicAddressSignature& callee =
      isData ? SASigDataDrop : SASigElemDrop;
  CallCompileState args;
  if (!f.passInstance(callee.argTypes[0], &args)) {
    return false;
  }

  MDefinition* segIndex =
      f.constant(Int32Value(int32_t(segIndexVal)), MIRType::Int32);
  if (!f.passArg(segIndex, callee.argTypes[1], &args)) {
    return false;
  }

  if (!f.finishCall(&args)) {
    return false;
  }

  return f.builtinInstanceMethodCall(callee, lineOrBytecode, args);
}

static bool EmitMemFillCall(FunctionCompiler& f, MDefinition* start,
                            MDefinition* val, MDefinition* len) {
  uint32_t lineOrBytecode = f.readCallSiteLineOrBytecode();

  const SymbolicAddressSignature& callee =
      f.moduleEnv().usesSharedMemory() ? SASigMemFillShared32 : SASigMemFill32;
  CallCompileState args;
  if (!f.passInstance(callee.argTypes[0], &args)) {
    return false;
  }

  if (!f.passArg(start, callee.argTypes[1], &args)) {
    return false;
  }
  if (!f.passArg(val, callee.argTypes[2], &args)) {
    return false;
  }
  if (!f.passArg(len, callee.argTypes[3], &args)) {
    return false;
  }
  MDefinition* memoryBase = f.memoryBase();
  if (!f.passArg(memoryBase, callee.argTypes[4], &args)) {
    return false;
  }

  if (!f.finishCall(&args)) {
    return false;
  }

  return f.builtinInstanceMethodCall(callee, lineOrBytecode, args);
}

static bool EmitMemFillInline(FunctionCompiler& f, MDefinition* start,
                              MDefinition* val, MDefinition* len) {
  MOZ_ASSERT(MaxInlineMemoryFillLength != 0);

  MOZ_ASSERT(len->isConstant() && len->type() == MIRType::Int32 &&
             val->isConstant() && val->type() == MIRType::Int32);

  uint32_t length = len->toConstant()->toInt32();
  uint32_t value = val->toConstant()->toInt32();
  MOZ_ASSERT(length != 0 && length <= MaxInlineMemoryFillLength);

  // Compute the number of copies of each width we will need to do
  size_t remainder = length;
#ifdef JS_64BIT
  size_t numCopies8 = remainder / sizeof(uint64_t);
  remainder %= sizeof(uint64_t);
#endif
  size_t numCopies4 = remainder / sizeof(uint32_t);
  remainder %= sizeof(uint32_t);
  size_t numCopies2 = remainder / sizeof(uint16_t);
  remainder %= sizeof(uint16_t);
  size_t numCopies1 = remainder;

  // Generate splatted definitions for wider fills as needed
#ifdef JS_64BIT
  MDefinition* val8 =
      numCopies8 ? f.constant(int64_t(SplatByteToUInt<uint64_t>(value, 8)))
                 : nullptr;
#endif
  MDefinition* val4 =
      numCopies4 ? f.constant(Int32Value(SplatByteToUInt<uint32_t>(value, 4)),
                              MIRType::Int32)
                 : nullptr;
  MDefinition* val2 =
      numCopies2 ? f.constant(Int32Value(SplatByteToUInt<uint32_t>(value, 2)),
                              MIRType::Int32)
                 : nullptr;

  // Store the fill value to the destination from high to low. We will trap
  // without writing anything on the first store if any dest byte is
  // out-of-bounds.
  size_t offset = length;

  if (numCopies1) {
    offset -= sizeof(uint8_t);

    MemoryAccessDesc access(Scalar::Uint8, 1, offset, f.bytecodeOffset());
    f.store(start, &access, val);
  }

  if (numCopies2) {
    offset -= sizeof(uint16_t);

    MemoryAccessDesc access(Scalar::Uint16, 1, offset, f.bytecodeOffset());
    f.store(start, &access, val2);
  }

  for (uint32_t i = 0; i < numCopies4; i++) {
    offset -= sizeof(uint32_t);

    MemoryAccessDesc access(Scalar::Uint32, 1, offset, f.bytecodeOffset());
    f.store(start, &access, val4);
  }

#ifdef JS_64BIT
  for (uint32_t i = 0; i < numCopies8; i++) {
    offset -= sizeof(uint64_t);

    MemoryAccessDesc access(Scalar::Int64, 1, offset, f.bytecodeOffset());
    f.store(start, &access, val8);
  }
#endif

  return true;
}

static bool EmitMemFill(FunctionCompiler& f) {
  MDefinition *start, *val, *len;
  if (!f.iter().readMemFill(&start, &val, &len)) {
    return false;
  }

  if (f.inDeadCode()) {
    return true;
  }

  if (MacroAssembler::SupportsFastUnalignedAccesses() && len->isConstant() &&
      len->type() == MIRType::Int32 && len->toConstant()->toInt32() != 0 &&
      uint32_t(len->toConstant()->toInt32()) <= MaxInlineMemoryFillLength &&
      val->isConstant() && val->type() == MIRType::Int32) {
    return EmitMemFillInline(f, start, val, len);
  }
  return EmitMemFillCall(f, start, val, len);
}

static bool EmitMemOrTableInit(FunctionCompiler& f, bool isMem) {
  uint32_t segIndexVal = 0, dstTableIndex = 0;
  MDefinition *dstOff, *srcOff, *len;
  if (!f.iter().readMemOrTableInit(isMem, &segIndexVal, &dstTableIndex, &dstOff,
                                   &srcOff, &len)) {
    return false;
  }

  if (f.inDeadCode()) {
    return true;
  }

  uint32_t lineOrBytecode = f.readCallSiteLineOrBytecode();

  const SymbolicAddressSignature& callee =
      isMem ? SASigMemInit32 : SASigTableInit;
  CallCompileState args;
  if (!f.passInstance(callee.argTypes[0], &args)) {
    return false;
  }

  if (!f.passArg(dstOff, callee.argTypes[1], &args)) {
    return false;
  }
  if (!f.passArg(srcOff, callee.argTypes[2], &args)) {
    return false;
  }
  if (!f.passArg(len, callee.argTypes[3], &args)) {
    return false;
  }

  MDefinition* segIndex =
      f.constant(Int32Value(int32_t(segIndexVal)), MIRType::Int32);
  if (!f.passArg(segIndex, callee.argTypes[4], &args)) {
    return false;
  }
  if (!isMem) {
    MDefinition* dti = f.constant(Int32Value(dstTableIndex), MIRType::Int32);
    if (!dti) {
      return false;
    }
    if (!f.passArg(dti, callee.argTypes[5], &args)) {
      return false;
    }
  }
  if (!f.finishCall(&args)) {
    return false;
  }

  return f.builtinInstanceMethodCall(callee, lineOrBytecode, args);
}

// Note, table.{get,grow,set} on table(funcref) are currently rejected by the
// verifier.

static bool EmitTableFill(FunctionCompiler& f) {
  uint32_t tableIndex;
  MDefinition *start, *val, *len;
  if (!f.iter().readTableFill(&tableIndex, &start, &val, &len)) {
    return false;
  }

  if (f.inDeadCode()) {
    return true;
  }

  uint32_t lineOrBytecode = f.readCallSiteLineOrBytecode();

  const SymbolicAddressSignature& callee = SASigTableFill;
  CallCompileState args;
  if (!f.passInstance(callee.argTypes[0], &args)) {
    return false;
  }

  if (!f.passArg(start, callee.argTypes[1], &args)) {
    return false;
  }
  if (!f.passArg(val, callee.argTypes[2], &args)) {
    return false;
  }
  if (!f.passArg(len, callee.argTypes[3], &args)) {
    return false;
  }

  MDefinition* tableIndexArg =
      f.constant(Int32Value(tableIndex), MIRType::Int32);
  if (!tableIndexArg) {
    return false;
  }
  if (!f.passArg(tableIndexArg, callee.argTypes[4], &args)) {
    return false;
  }

  if (!f.finishCall(&args)) {
    return false;
  }

  return f.builtinInstanceMethodCall(callee, lineOrBytecode, args);
}

static bool EmitTableGet(FunctionCompiler& f) {
  uint32_t tableIndex;
  MDefinition* index;
  if (!f.iter().readTableGet(&tableIndex, &index)) {
    return false;
  }

  if (f.inDeadCode()) {
    return true;
  }

  uint32_t lineOrBytecode = f.readCallSiteLineOrBytecode();

  const SymbolicAddressSignature& callee = SASigTableGet;
  CallCompileState args;
  if (!f.passInstance(callee.argTypes[0], &args)) {
    return false;
  }

  if (!f.passArg(index, callee.argTypes[1], &args)) {
    return false;
  }

  MDefinition* tableIndexArg =
      f.constant(Int32Value(tableIndex), MIRType::Int32);
  if (!tableIndexArg) {
    return false;
  }
  if (!f.passArg(tableIndexArg, callee.argTypes[2], &args)) {
    return false;
  }

  if (!f.finishCall(&args)) {
    return false;
  }

  // The return value here is either null, denoting an error, or a short-lived
  // pointer to a location containing a possibly-null ref.
  MDefinition* ret;
  if (!f.builtinInstanceMethodCall(callee, lineOrBytecode, args, &ret)) {
    return false;
  }

  f.iter().setResult(ret);
  return true;
}

static bool EmitTableGrow(FunctionCompiler& f) {
  uint32_t tableIndex;
  MDefinition* initValue;
  MDefinition* delta;
  if (!f.iter().readTableGrow(&tableIndex, &initValue, &delta)) {
    return false;
  }

  if (f.inDeadCode()) {
    return true;
  }

  uint32_t lineOrBytecode = f.readCallSiteLineOrBytecode();

  const SymbolicAddressSignature& callee = SASigTableGrow;
  CallCompileState args;
  if (!f.passInstance(callee.argTypes[0], &args)) {
    return false;
  }

  if (!f.passArg(initValue, callee.argTypes[1], &args)) {
    return false;
  }

  if (!f.passArg(delta, callee.argTypes[2], &args)) {
    return false;
  }

  MDefinition* tableIndexArg =
      f.constant(Int32Value(tableIndex), MIRType::Int32);
  if (!tableIndexArg) {
    return false;
  }
  if (!f.passArg(tableIndexArg, callee.argTypes[3], &args)) {
    return false;
  }

  if (!f.finishCall(&args)) {
    return false;
  }

  MDefinition* ret;
  if (!f.builtinInstanceMethodCall(callee, lineOrBytecode, args, &ret)) {
    return false;
  }

  f.iter().setResult(ret);
  return true;
}

static bool EmitTableSet(FunctionCompiler& f) {
  uint32_t tableIndex;
  MDefinition* index;
  MDefinition* value;
  if (!f.iter().readTableSet(&tableIndex, &index, &value)) {
    return false;
  }

  if (f.inDeadCode()) {
    return true;
  }

  uint32_t lineOrBytecode = f.readCallSiteLineOrBytecode();

  const SymbolicAddressSignature& callee = SASigTableSet;
  CallCompileState args;
  if (!f.passInstance(callee.argTypes[0], &args)) {
    return false;
  }

  if (!f.passArg(index, callee.argTypes[1], &args)) {
    return false;
  }

  if (!f.passArg(value, callee.argTypes[2], &args)) {
    return false;
  }

  MDefinition* tableIndexArg =
      f.constant(Int32Value(tableIndex), MIRType::Int32);
  if (!tableIndexArg) {
    return false;
  }
  if (!f.passArg(tableIndexArg, callee.argTypes[3], &args)) {
    return false;
  }

  if (!f.finishCall(&args)) {
    return false;
  }

  return f.builtinInstanceMethodCall(callee, lineOrBytecode, args);
}

static bool EmitTableSize(FunctionCompiler& f) {
  uint32_t tableIndex;
  if (!f.iter().readTableSize(&tableIndex)) {
    return false;
  }

  if (f.inDeadCode()) {
    return true;
  }

  uint32_t lineOrBytecode = f.readCallSiteLineOrBytecode();

  const SymbolicAddressSignature& callee = SASigTableSize;
  CallCompileState args;
  if (!f.passInstance(callee.argTypes[0], &args)) {
    return false;
  }

  MDefinition* tableIndexArg =
      f.constant(Int32Value(tableIndex), MIRType::Int32);
  if (!tableIndexArg) {
    return false;
  }
  if (!f.passArg(tableIndexArg, callee.argTypes[1], &args)) {
    return false;
  }

  if (!f.finishCall(&args)) {
    return false;
  }

  MDefinition* ret;
  if (!f.builtinInstanceMethodCall(callee, lineOrBytecode, args, &ret)) {
    return false;
  }

  f.iter().setResult(ret);
  return true;
}

static bool EmitRefFunc(FunctionCompiler& f) {
  uint32_t funcIndex;
  if (!f.iter().readRefFunc(&funcIndex)) {
    return false;
  }

  if (f.inDeadCode()) {
    return true;
  }

  uint32_t lineOrBytecode = f.readCallSiteLineOrBytecode();

  const SymbolicAddressSignature& callee = SASigRefFunc;
  CallCompileState args;
  if (!f.passInstance(callee.argTypes[0], &args)) {
    return false;
  }

  MDefinition* funcIndexArg = f.constant(Int32Value(funcIndex), MIRType::Int32);
  if (!funcIndexArg) {
    return false;
  }
  if (!f.passArg(funcIndexArg, callee.argTypes[1], &args)) {
    return false;
  }

  if (!f.finishCall(&args)) {
    return false;
  }

  // The return value here is either null, denoting an error, or a short-lived
  // pointer to a location containing a possibly-null ref.
  MDefinition* ret;
  if (!f.builtinInstanceMethodCall(callee, lineOrBytecode, args, &ret)) {
    return false;
  }

  f.iter().setResult(ret);
  return true;
}

static bool EmitRefNull(FunctionCompiler& f) {
  RefType type;
  if (!f.iter().readRefNull(&type)) {
    return false;
  }

  if (f.inDeadCode()) {
    return true;
  }

  MDefinition* nullVal = f.nullRefConstant();
  if (!nullVal) {
    return false;
  }
  f.iter().setResult(nullVal);
  return true;
}

static bool EmitRefIsNull(FunctionCompiler& f) {
  MDefinition* input;
  if (!f.iter().readRefIsNull(&input)) {
    return false;
  }

  if (f.inDeadCode()) {
    return true;
  }

  MDefinition* nullVal = f.nullRefConstant();
  if (!nullVal) {
    return false;
  }
  f.iter().setResult(
      f.compare(input, nullVal, JSOp::Eq, MCompare::Compare_RefOrNull));
  return true;
}

#ifdef ENABLE_WASM_SIMD
static bool EmitConstSimd128(FunctionCompiler& f) {
  V128 v128;
  if (!f.iter().readV128Const(&v128)) {
    return false;
  }

  f.iter().setResult(f.constant(v128));
  return true;
}

static bool EmitBinarySimd128(FunctionCompiler& f, bool commutative,
                              SimdOp op) {
  MDefinition* lhs;
  MDefinition* rhs;
  if (!f.iter().readBinary(ValType::V128, &lhs, &rhs)) {
    return false;
  }

  f.iter().setResult(f.binarySimd128(lhs, rhs, commutative, op));
  return true;
}

static bool EmitShiftSimd128(FunctionCompiler& f, SimdOp op) {
  MDefinition* lhs;
  MDefinition* rhs;
  if (!f.iter().readVectorShift(&lhs, &rhs)) {
    return false;
  }

  f.iter().setResult(f.shiftSimd128(lhs, rhs, op));
  return true;
}

static bool EmitSplatSimd128(FunctionCompiler& f, ValType inType, SimdOp op) {
  MDefinition* src;
  if (!f.iter().readConversion(inType, ValType::V128, &src)) {
    return false;
  }

  f.iter().setResult(f.scalarToSimd128(src, op));
  return true;
}

static bool EmitUnarySimd128(FunctionCompiler& f, SimdOp op) {
  MDefinition* src;
  if (!f.iter().readUnary(ValType::V128, &src)) {
    return false;
  }

  f.iter().setResult(f.unarySimd128(src, op));
  return true;
}

static bool EmitReduceSimd128(FunctionCompiler& f, SimdOp op) {
  MDefinition* src;
  if (!f.iter().readConversion(ValType::V128, ValType::I32, &src)) {
    return false;
  }

  f.iter().setResult(f.reduceSimd128(src, op, ValType::I32));
  return true;
}

static bool EmitExtractLaneSimd128(FunctionCompiler& f, ValType outType,
                                   uint32_t laneLimit, SimdOp op) {
  uint32_t laneIndex;
  MDefinition* src;
  if (!f.iter().readExtractLane(outType, laneLimit, &laneIndex, &src)) {
    return false;
  }

  f.iter().setResult(f.reduceSimd128(src, op, outType, laneIndex));
  return true;
}

static bool EmitReplaceLaneSimd128(FunctionCompiler& f, ValType laneType,
                                   uint32_t laneLimit, SimdOp op) {
  uint32_t laneIndex;
  MDefinition* lhs;
  MDefinition* rhs;
  if (!f.iter().readReplaceLane(laneType, laneLimit, &laneIndex, &lhs, &rhs)) {
    return false;
  }

  f.iter().setResult(f.replaceLaneSimd128(lhs, rhs, laneIndex, op));
  return true;
}

static bool EmitBitselectSimd128(FunctionCompiler& f) {
  MDefinition* v1;
  MDefinition* v2;
  MDefinition* control;
  if (!f.iter().readVectorSelect(&v1, &v2, &control)) {
    return false;
  }

  f.iter().setResult(f.bitselectSimd128(v1, v2, control));
  return true;
}

static bool EmitShuffleSimd128(FunctionCompiler& f) {
  MDefinition* v1;
  MDefinition* v2;
  V128 control;
  if (!f.iter().readVectorShuffle(&v1, &v2, &control)) {
    return false;
  }

#  ifdef ENABLE_WASM_SIMD_WORMHOLE
  if (f.moduleEnv().simdWormholeEnabled() && IsWormholeTrigger(control)) {
    switch (control.bytes[15]) {
      case 0:
        f.iter().setResult(
            f.binarySimd128(v1, v2, false, wasm::SimdOp::MozWHSELFTEST));
        return true;
#    if defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64)
      case 1:
        f.iter().setResult(
            f.binarySimd128(v1, v2, false, wasm::SimdOp::MozWHPMADDUBSW));
        return true;
      case 2:
        f.iter().setResult(
            f.binarySimd128(v1, v2, false, wasm::SimdOp::MozWHPMADDWD));
        return true;
#    endif
      default:
        return f.iter().fail("Unrecognized wormhole opcode");
    }
  }
#  endif

  f.iter().setResult(f.shuffleSimd128(v1, v2, control));
  return true;
}

static bool EmitLoadSplatSimd128(FunctionCompiler& f, Scalar::Type viewType,
                                 wasm::SimdOp splatOp) {
  LinearMemoryAddress<MDefinition*> addr;
  if (!f.iter().readLoadSplat(Scalar::byteSize(viewType), &addr)) {
    return false;
  }

  f.iter().setResult(f.loadSplatSimd128(viewType, addr, splatOp));
  return true;
}

static bool EmitLoadExtendSimd128(FunctionCompiler& f, wasm::SimdOp op) {
  LinearMemoryAddress<MDefinition*> addr;
  if (!f.iter().readLoadExtend(&addr)) {
    return false;
  }

  f.iter().setResult(f.loadExtendSimd128(addr, op));
  return true;
}

static bool EmitLoadZeroSimd128(FunctionCompiler& f, Scalar::Type viewType,
                                size_t numBytes) {
  LinearMemoryAddress<MDefinition*> addr;
  if (!f.iter().readLoadSplat(numBytes, &addr)) {
    return false;
  }

  f.iter().setResult(f.loadZeroSimd128(viewType, numBytes, addr));
  return true;
}

static bool EmitLoadLaneSimd128(FunctionCompiler& f, uint32_t laneSize) {
  uint32_t laneIndex;
  MDefinition* src;
  LinearMemoryAddress<MDefinition*> addr;
  if (!f.iter().readLoadLane(laneSize, &addr, &laneIndex, &src)) {
    return false;
  }

  f.iter().setResult(f.loadLaneSimd128(laneSize, addr, laneIndex, src));
  return true;
}

static bool EmitStoreLaneSimd128(FunctionCompiler& f, uint32_t laneSize) {
  uint32_t laneIndex;
  MDefinition* src;
  LinearMemoryAddress<MDefinition*> addr;
  if (!f.iter().readStoreLane(laneSize, &addr, &laneIndex, &src)) {
    return false;
  }

  f.storeLaneSimd128(laneSize, addr, laneIndex, src);
  return true;
}
#endif

static bool EmitBodyExprs(FunctionCompiler& f) {
  if (!f.iter().startFunction(f.funcIndex())) {
    return false;
  }

#define CHECK(c)          \
  if (!(c)) return false; \
  break

  while (true) {
    if (!f.mirGen().ensureBallast()) {
      return false;
    }

    OpBytes op;
    if (!f.iter().readOp(&op)) {
      return false;
    }

    switch (op.b0) {
      case uint16_t(Op::End):
        if (!EmitEnd(f)) {
          return false;
        }
        if (f.iter().controlStackEmpty()) {
          return true;
        }
        break;

      // Control opcodes
      case uint16_t(Op::Unreachable):
        CHECK(EmitUnreachable(f));
      case uint16_t(Op::Nop):
        CHECK(f.iter().readNop());
      case uint16_t(Op::Block):
        CHECK(EmitBlock(f));
      case uint16_t(Op::Loop):
        CHECK(EmitLoop(f));
      case uint16_t(Op::If):
        CHECK(EmitIf(f));
      case uint16_t(Op::Else):
        CHECK(EmitElse(f));
#ifdef ENABLE_WASM_EXCEPTIONS
      case uint16_t(Op::Try):
        if (!f.moduleEnv().exceptionsEnabled()) {
          return f.iter().unrecognizedOpcode(&op);
        }
        CHECK(EmitTry(f));
      case uint16_t(Op::Catch):
        if (!f.moduleEnv().exceptionsEnabled()) {
          return f.iter().unrecognizedOpcode(&op);
        }
        CHECK(EmitCatch(f));
      case uint16_t(Op::CatchAll):
        if (!f.moduleEnv().exceptionsEnabled()) {
          return f.iter().unrecognizedOpcode(&op);
        }
        CHECK(EmitCatchAll(f));
      case uint16_t(Op::Delegate):
        if (!f.moduleEnv().exceptionsEnabled()) {
          return f.iter().unrecognizedOpcode(&op);
        }
        if (!EmitDelegate(f)) {
          return false;
        }
        break;
      case uint16_t(Op::Throw):
        if (!f.moduleEnv().exceptionsEnabled()) {
          return f.iter().unrecognizedOpcode(&op);
        }
        CHECK(EmitThrow(f));
      case uint16_t(Op::Rethrow):
        if (!f.moduleEnv().exceptionsEnabled()) {
          return f.iter().unrecognizedOpcode(&op);
        }
        CHECK(EmitRethrow(f));
#endif
      case uint16_t(Op::Br):
        CHECK(EmitBr(f));
      case uint16_t(Op::BrIf):
        CHECK(EmitBrIf(f));
      case uint16_t(Op::BrTable):
        CHECK(EmitBrTable(f));
      case uint16_t(Op::Return):
        CHECK(EmitReturn(f));

      // Calls
      case uint16_t(Op::Call):
        CHECK(EmitCall(f, /* asmJSFuncDef = */ false));
      case uint16_t(Op::CallIndirect):
        CHECK(EmitCallIndirect(f, /* oldStyle = */ false));

      // Parametric operators
      case uint16_t(Op::Drop):
        CHECK(f.iter().readDrop());
      case uint16_t(Op::SelectNumeric):
        CHECK(EmitSelect(f, /*typed*/ false));
      case uint16_t(Op::SelectTyped):
        CHECK(EmitSelect(f, /*typed*/ true));

      // Locals and globals
      case uint16_t(Op::GetLocal):
        CHECK(EmitGetLocal(f));
      case uint16_t(Op::SetLocal):
        CHECK(EmitSetLocal(f));
      case uint16_t(Op::TeeLocal):
        CHECK(EmitTeeLocal(f));
      case uint16_t(Op::GetGlobal):
        CHECK(EmitGetGlobal(f));
      case uint16_t(Op::SetGlobal):
        CHECK(EmitSetGlobal(f));
      case uint16_t(Op::TableGet):
        CHECK(EmitTableGet(f));
      case uint16_t(Op::TableSet):
        CHECK(EmitTableSet(f));

      // Memory-related operators
      case uint16_t(Op::I32Load):
        CHECK(EmitLoad(f, ValType::I32, Scalar::Int32));
      case uint16_t(Op::I64Load):
        CHECK(EmitLoad(f, ValType::I64, Scalar::Int64));
      case uint16_t(Op::F32Load):
        CHECK(EmitLoad(f, ValType::F32, Scalar::Float32));
      case uint16_t(Op::F64Load):
        CHECK(EmitLoad(f, ValType::F64, Scalar::Float64));
      case uint16_t(Op::I32Load8S):
        CHECK(EmitLoad(f, ValType::I32, Scalar::Int8));
      case uint16_t(Op::I32Load8U):
        CHECK(EmitLoad(f, ValType::I32, Scalar::Uint8));
      case uint16_t(Op::I32Load16S):
        CHECK(EmitLoad(f, ValType::I32, Scalar::Int16));
      case uint16_t(Op::I32Load16U):
        CHECK(EmitLoad(f, ValType::I32, Scalar::Uint16));
      case uint16_t(Op::I64Load8S):
        CHECK(EmitLoad(f, ValType::I64, Scalar::Int8));
      case uint16_t(Op::I64Load8U):
        CHECK(EmitLoad(f, ValType::I64, Scalar::Uint8));
      case uint16_t(Op::I64Load16S):
        CHECK(EmitLoad(f, ValType::I64, Scalar::Int16));
      case uint16_t(Op::I64Load16U):
        CHECK(EmitLoad(f, ValType::I64, Scalar::Uint16));
      case uint16_t(Op::I64Load32S):
        CHECK(EmitLoad(f, ValType::I64, Scalar::Int32));
      case uint16_t(Op::I64Load32U):
        CHECK(EmitLoad(f, ValType::I64, Scalar::Uint32));
      case uint16_t(Op::I32Store):
        CHECK(EmitStore(f, ValType::I32, Scalar::Int32));
      case uint16_t(Op::I64Store):
        CHECK(EmitStore(f, ValType::I64, Scalar::Int64));
      case uint16_t(Op::F32Store):
        CHECK(EmitStore(f, ValType::F32, Scalar::Float32));
      case uint16_t(Op::F64Store):
        CHECK(EmitStore(f, ValType::F64, Scalar::Float64));
      case uint16_t(Op::I32Store8):
        CHECK(EmitStore(f, ValType::I32, Scalar::Int8));
      case uint16_t(Op::I32Store16):
        CHECK(EmitStore(f, ValType::I32, Scalar::Int16));
      case uint16_t(Op::I64Store8):
        CHECK(EmitStore(f, ValType::I64, Scalar::Int8));
      case uint16_t(Op::I64Store16):
        CHECK(EmitStore(f, ValType::I64, Scalar::Int16));
      case uint16_t(Op::I64Store32):
        CHECK(EmitStore(f, ValType::I64, Scalar::Int32));
      case uint16_t(Op::MemorySize):
        CHECK(EmitMemorySize(f));
      case uint16_t(Op::MemoryGrow):
        CHECK(EmitMemoryGrow(f));

      // Constants
      case uint16_t(Op::I32Const):
        CHECK(EmitI32Const(f));
      case uint16_t(Op::I64Const):
        CHECK(EmitI64Const(f));
      case uint16_t(Op::F32Const):
        CHECK(EmitF32Const(f));
      case uint16_t(Op::F64Const):
        CHECK(EmitF64Const(f));

      // Comparison operators
      case uint16_t(Op::I32Eqz):
        CHECK(EmitConversion<MNot>(f, ValType::I32, ValType::I32));
      case uint16_t(Op::I32Eq):
        CHECK(
            EmitComparison(f, ValType::I32, JSOp::Eq, MCompare::Compare_Int32));
      case uint16_t(Op::I32Ne):
        CHECK(
            EmitComparison(f, ValType::I32, JSOp::Ne, MCompare::Compare_Int32));
      case uint16_t(Op::I32LtS):
        CHECK(
            EmitComparison(f, ValType::I32, JSOp::Lt, MCompare::Compare_Int32));
      case uint16_t(Op::I32LtU):
        CHECK(EmitComparison(f, ValType::I32, JSOp::Lt,
                             MCompare::Compare_UInt32));
      case uint16_t(Op::I32GtS):
        CHECK(
            EmitComparison(f, ValType::I32, JSOp::Gt, MCompare::Compare_Int32));
      case uint16_t(Op::I32GtU):
        CHECK(EmitComparison(f, ValType::I32, JSOp::Gt,
                             MCompare::Compare_UInt32));
      case uint16_t(Op::I32LeS):
        CHECK(
            EmitComparison(f, ValType::I32, JSOp::Le, MCompare::Compare_Int32));
      case uint16_t(Op::I32LeU):
        CHECK(EmitComparison(f, ValType::I32, JSOp::Le,
                             MCompare::Compare_UInt32));
      case uint16_t(Op::I32GeS):
        CHECK(
            EmitComparison(f, ValType::I32, JSOp::Ge, MCompare::Compare_Int32));
      case uint16_t(Op::I32GeU):
        CHECK(EmitComparison(f, ValType::I32, JSOp::Ge,
                             MCompare::Compare_UInt32));
      case uint16_t(Op::I64Eqz):
        CHECK(EmitConversion<MNot>(f, ValType::I64, ValType::I32));
      case uint16_t(Op::I64Eq):
        CHECK(
            EmitComparison(f, ValType::I64, JSOp::Eq, MCompare::Compare_Int64));
      case uint16_t(Op::I64Ne):
        CHECK(
            EmitComparison(f, ValType::I64, JSOp::Ne, MCompare::Compare_Int64));
      case uint16_t(Op::I64LtS):
        CHECK(
            EmitComparison(f, ValType::I64, JSOp::Lt, MCompare::Compare_Int64));
      case uint16_t(Op::I64LtU):
        CHECK(EmitComparison(f, ValType::I64, JSOp::Lt,
                             MCompare::Compare_UInt64));
      case uint16_t(Op::I64GtS):
        CHECK(
            EmitComparison(f, ValType::I64, JSOp::Gt, MCompare::Compare_Int64));
      case uint16_t(Op::I64GtU):
        CHECK(EmitComparison(f, ValType::I64, JSOp::Gt,
                             MCompare::Compare_UInt64));
      case uint16_t(Op::I64LeS):
        CHECK(
            EmitComparison(f, ValType::I64, JSOp::Le, MCompare::Compare_Int64));
      case uint16_t(Op::I64LeU):
        CHECK(EmitComparison(f, ValType::I64, JSOp::Le,
                             MCompare::Compare_UInt64));
      case uint16_t(Op::I64GeS):
        CHECK(
            EmitComparison(f, ValType::I64, JSOp::Ge, MCompare::Compare_Int64));
      case uint16_t(Op::I64GeU):
        CHECK(EmitComparison(f, ValType::I64, JSOp::Ge,
                             MCompare::Compare_UInt64));
      case uint16_t(Op::F32Eq):
        CHECK(EmitComparison(f, ValType::F32, JSOp::Eq,
                             MCompare::Compare_Float32));
      case uint16_t(Op::F32Ne):
        CHECK(EmitComparison(f, ValType::F32, JSOp::Ne,
                             MCompare::Compare_Float32));
      case uint16_t(Op::F32Lt):
        CHECK(EmitComparison(f, ValType::F32, JSOp::Lt,
                             MCompare::Compare_Float32));
      case uint16_t(Op::F32Gt):
        CHECK(EmitComparison(f, ValType::F32, JSOp::Gt,
                             MCompare::Compare_Float32));
      case uint16_t(Op::F32Le):
        CHECK(EmitComparison(f, ValType::F32, JSOp::Le,
                             MCompare::Compare_Float32));
      case uint16_t(Op::F32Ge):
        CHECK(EmitComparison(f, ValType::F32, JSOp::Ge,
                             MCompare::Compare_Float32));
      case uint16_t(Op::F64Eq):
        CHECK(EmitComparison(f, ValType::F64, JSOp::Eq,
                             MCompare::Compare_Double));
      case uint16_t(Op::F64Ne):
        CHECK(EmitComparison(f, ValType::F64, JSOp::Ne,
                             MCompare::Compare_Double));
      case uint16_t(Op::F64Lt):
        CHECK(EmitComparison(f, ValType::F64, JSOp::Lt,
                             MCompare::Compare_Double));
      case uint16_t(Op::F64Gt):
        CHECK(EmitComparison(f, ValType::F64, JSOp::Gt,
                             MCompare::Compare_Double));
      case uint16_t(Op::F64Le):
        CHECK(EmitComparison(f, ValType::F64, JSOp::Le,
                             MCompare::Compare_Double));
      case uint16_t(Op::F64Ge):
        CHECK(EmitComparison(f, ValType::F64, JSOp::Ge,
                             MCompare::Compare_Double));

      // Numeric operators
      case uint16_t(Op::I32Clz):
        CHECK(EmitUnaryWithType<MClz>(f, ValType::I32, MIRType::Int32));
      case uint16_t(Op::I32Ctz):
        CHECK(EmitUnaryWithType<MCtz>(f, ValType::I32, MIRType::Int32));
      case uint16_t(Op::I32Popcnt):
        CHECK(EmitUnaryWithType<MPopcnt>(f, ValType::I32, MIRType::Int32));
      case uint16_t(Op::I32Add):
        CHECK(EmitAdd(f, ValType::I32, MIRType::Int32));
      case uint16_t(Op::I32Sub):
        CHECK(EmitSub(f, ValType::I32, MIRType::Int32));
      case uint16_t(Op::I32Mul):
        CHECK(EmitMul(f, ValType::I32, MIRType::Int32));
      case uint16_t(Op::I32DivS):
      case uint16_t(Op::I32DivU):
        CHECK(
            EmitDiv(f, ValType::I32, MIRType::Int32, Op(op.b0) == Op::I32DivU));
      case uint16_t(Op::I32RemS):
      case uint16_t(Op::I32RemU):
        CHECK(
            EmitRem(f, ValType::I32, MIRType::Int32, Op(op.b0) == Op::I32RemU));
      case uint16_t(Op::I32And):
        CHECK(EmitBitwise<MBitAnd>(f, ValType::I32, MIRType::Int32));
      case uint16_t(Op::I32Or):
        CHECK(EmitBitwise<MBitOr>(f, ValType::I32, MIRType::Int32));
      case uint16_t(Op::I32Xor):
        CHECK(EmitBitwise<MBitXor>(f, ValType::I32, MIRType::Int32));
      case uint16_t(Op::I32Shl):
        CHECK(EmitBitwise<MLsh>(f, ValType::I32, MIRType::Int32));
      case uint16_t(Op::I32ShrS):
        CHECK(EmitBitwise<MRsh>(f, ValType::I32, MIRType::Int32));
      case uint16_t(Op::I32ShrU):
        CHECK(EmitUrsh(f, ValType::I32, MIRType::Int32));
      case uint16_t(Op::I32Rotl):
      case uint16_t(Op::I32Rotr):
        CHECK(EmitRotate(f, ValType::I32, Op(op.b0) == Op::I32Rotl));
      case uint16_t(Op::I64Clz):
        CHECK(EmitUnaryWithType<MClz>(f, ValType::I64, MIRType::Int64));
      case uint16_t(Op::I64Ctz):
        CHECK(EmitUnaryWithType<MCtz>(f, ValType::I64, MIRType::Int64));
      case uint16_t(Op::I64Popcnt):
        CHECK(EmitUnaryWithType<MPopcnt>(f, ValType::I64, MIRType::Int64));
      case uint16_t(Op::I64Add):
        CHECK(EmitAdd(f, ValType::I64, MIRType::Int64));
      case uint16_t(Op::I64Sub):
        CHECK(EmitSub(f, ValType::I64, MIRType::Int64));
      case uint16_t(Op::I64Mul):
        CHECK(EmitMul(f, ValType::I64, MIRType::Int64));
      case uint16_t(Op::I64DivS):
      case uint16_t(Op::I64DivU):
        CHECK(
            EmitDiv(f, ValType::I64, MIRType::Int64, Op(op.b0) == Op::I64DivU));
      case uint16_t(Op::I64RemS):
      case uint16_t(Op::I64RemU):
        CHECK(
            EmitRem(f, ValType::I64, MIRType::Int64, Op(op.b0) == Op::I64RemU));
      case uint16_t(Op::I64And):
        CHECK(EmitBitwise<MBitAnd>(f, ValType::I64, MIRType::Int64));
      case uint16_t(Op::I64Or):
        CHECK(EmitBitwise<MBitOr>(f, ValType::I64, MIRType::Int64));
      case uint16_t(Op::I64Xor):
        CHECK(EmitBitwise<MBitXor>(f, ValType::I64, MIRType::Int64));
      case uint16_t(Op::I64Shl):
        CHECK(EmitBitwise<MLsh>(f, ValType::I64, MIRType::Int64));
      case uint16_t(Op::I64ShrS):
        CHECK(EmitBitwise<MRsh>(f, ValType::I64, MIRType::Int64));
      case uint16_t(Op::I64ShrU):
        CHECK(EmitUrsh(f, ValType::I64, MIRType::Int64));
      case uint16_t(Op::I64Rotl):
      case uint16_t(Op::I64Rotr):
        CHECK(EmitRotate(f, ValType::I64, Op(op.b0) == Op::I64Rotl));
      case uint16_t(Op::F32Abs):
        CHECK(EmitUnaryWithType<MAbs>(f, ValType::F32, MIRType::Float32));
      case uint16_t(Op::F32Neg):
        CHECK(EmitUnaryWithType<MWasmNeg>(f, ValType::F32, MIRType::Float32));
      case uint16_t(Op::F32Ceil):
        CHECK(EmitUnaryMathBuiltinCall(f, SASigCeilF));
      case uint16_t(Op::F32Floor):
        CHECK(EmitUnaryMathBuiltinCall(f, SASigFloorF));
      case uint16_t(Op::F32Trunc):
        CHECK(EmitUnaryMathBuiltinCall(f, SASigTruncF));
      case uint16_t(Op::F32Nearest):
        CHECK(EmitUnaryMathBuiltinCall(f, SASigNearbyIntF));
      case uint16_t(Op::F32Sqrt):
        CHECK(EmitUnaryWithType<MSqrt>(f, ValType::F32, MIRType::Float32));
      case uint16_t(Op::F32Add):
        CHECK(EmitAdd(f, ValType::F32, MIRType::Float32));
      case uint16_t(Op::F32Sub):
        CHECK(EmitSub(f, ValType::F32, MIRType::Float32));
      case uint16_t(Op::F32Mul):
        CHECK(EmitMul(f, ValType::F32, MIRType::Float32));
      case uint16_t(Op::F32Div):
        CHECK(EmitDiv(f, ValType::F32, MIRType::Float32,
                      /* isUnsigned = */ false));
      case uint16_t(Op::F32Min):
      case uint16_t(Op::F32Max):
        CHECK(EmitMinMax(f, ValType::F32, MIRType::Float32,
                         Op(op.b0) == Op::F32Max));
      case uint16_t(Op::F32CopySign):
        CHECK(EmitCopySign(f, ValType::F32));
      case uint16_t(Op::F64Abs):
        CHECK(EmitUnaryWithType<MAbs>(f, ValType::F64, MIRType::Double));
      case uint16_t(Op::F64Neg):
        CHECK(EmitUnaryWithType<MWasmNeg>(f, ValType::F64, MIRType::Double));
      case uint16_t(Op::F64Ceil):
        CHECK(EmitUnaryMathBuiltinCall(f, SASigCeilD));
      case uint16_t(Op::F64Floor):
        CHECK(EmitUnaryMathBuiltinCall(f, SASigFloorD));
      case uint16_t(Op::F64Trunc):
        CHECK(EmitUnaryMathBuiltinCall(f, SASigTruncD));
      case uint16_t(Op::F64Nearest):
        CHECK(EmitUnaryMathBuiltinCall(f, SASigNearbyIntD));
      case uint16_t(Op::F64Sqrt):
        CHECK(EmitUnaryWithType<MSqrt>(f, ValType::F64, MIRType::Double));
      case uint16_t(Op::F64Add):
        CHECK(EmitAdd(f, ValType::F64, MIRType::Double));
      case uint16_t(Op::F64Sub):
        CHECK(EmitSub(f, ValType::F64, MIRType::Double));
      case uint16_t(Op::F64Mul):
        CHECK(EmitMul(f, ValType::F64, MIRType::Double));
      case uint16_t(Op::F64Div):
        CHECK(EmitDiv(f, ValType::F64, MIRType::Double,
                      /* isUnsigned = */ false));
      case uint16_t(Op::F64Min):
      case uint16_t(Op::F64Max):
        CHECK(EmitMinMax(f, ValType::F64, MIRType::Double,
                         Op(op.b0) == Op::F64Max));
      case uint16_t(Op::F64CopySign):
        CHECK(EmitCopySign(f, ValType::F64));

      // Conversions
      case uint16_t(Op::I32WrapI64):
        CHECK(EmitConversion<MWrapInt64ToInt32>(f, ValType::I64, ValType::I32));
      case uint16_t(Op::I32TruncSF32):
      case uint16_t(Op::I32TruncUF32):
        CHECK(EmitTruncate(f, ValType::F32, ValType::I32,
                           Op(op.b0) == Op::I32TruncUF32, false));
      case uint16_t(Op::I32TruncSF64):
      case uint16_t(Op::I32TruncUF64):
        CHECK(EmitTruncate(f, ValType::F64, ValType::I32,
                           Op(op.b0) == Op::I32TruncUF64, false));
      case uint16_t(Op::I64ExtendSI32):
      case uint16_t(Op::I64ExtendUI32):
        CHECK(EmitExtendI32(f, Op(op.b0) == Op::I64ExtendUI32));
      case uint16_t(Op::I64TruncSF32):
      case uint16_t(Op::I64TruncUF32):
        CHECK(EmitTruncate(f, ValType::F32, ValType::I64,
                           Op(op.b0) == Op::I64TruncUF32, false));
      case uint16_t(Op::I64TruncSF64):
      case uint16_t(Op::I64TruncUF64):
        CHECK(EmitTruncate(f, ValType::F64, ValType::I64,
                           Op(op.b0) == Op::I64TruncUF64, false));
      case uint16_t(Op::F32ConvertSI32):
        CHECK(EmitConversion<MToFloat32>(f, ValType::I32, ValType::F32));
      case uint16_t(Op::F32ConvertUI32):
        CHECK(EmitConversion<MWasmUnsignedToFloat32>(f, ValType::I32,
                                                     ValType::F32));
      case uint16_t(Op::F32ConvertSI64):
      case uint16_t(Op::F32ConvertUI64):
        CHECK(EmitConvertI64ToFloatingPoint(f, ValType::F32, MIRType::Float32,
                                            Op(op.b0) == Op::F32ConvertUI64));
      case uint16_t(Op::F32DemoteF64):
        CHECK(EmitConversion<MToFloat32>(f, ValType::F64, ValType::F32));
      case uint16_t(Op::F64ConvertSI32):
        CHECK(EmitConversion<MToDouble>(f, ValType::I32, ValType::F64));
      case uint16_t(Op::F64ConvertUI32):
        CHECK(EmitConversion<MWasmUnsignedToDouble>(f, ValType::I32,
                                                    ValType::F64));
      case uint16_t(Op::F64ConvertSI64):
      case uint16_t(Op::F64ConvertUI64):
        CHECK(EmitConvertI64ToFloatingPoint(f, ValType::F64, MIRType::Double,
                                            Op(op.b0) == Op::F64ConvertUI64));
      case uint16_t(Op::F64PromoteF32):
        CHECK(EmitConversion<MToDouble>(f, ValType::F32, ValType::F64));

      // Reinterpretations
      case uint16_t(Op::I32ReinterpretF32):
        CHECK(EmitReinterpret(f, ValType::I32, ValType::F32, MIRType::Int32));
      case uint16_t(Op::I64ReinterpretF64):
        CHECK(EmitReinterpret(f, ValType::I64, ValType::F64, MIRType::Int64));
      case uint16_t(Op::F32ReinterpretI32):
        CHECK(EmitReinterpret(f, ValType::F32, ValType::I32, MIRType::Float32));
      case uint16_t(Op::F64ReinterpretI64):
        CHECK(EmitReinterpret(f, ValType::F64, ValType::I64, MIRType::Double));

#ifdef ENABLE_WASM_GC
      case uint16_t(Op::RefEq):
        if (!f.moduleEnv().gcEnabled()) {
          return f.iter().unrecognizedOpcode(&op);
        }
        CHECK(EmitComparison(f, RefType::extern_(), JSOp::Eq,
                             MCompare::Compare_RefOrNull));
#endif
      case uint16_t(Op::RefFunc):
        CHECK(EmitRefFunc(f));
      case uint16_t(Op::RefNull):
        CHECK(EmitRefNull(f));
      case uint16_t(Op::RefIsNull):
        CHECK(EmitRefIsNull(f));

      // Sign extensions
      case uint16_t(Op::I32Extend8S):
        CHECK(EmitSignExtend(f, 1, 4));
      case uint16_t(Op::I32Extend16S):
        CHECK(EmitSignExtend(f, 2, 4));
      case uint16_t(Op::I64Extend8S):
        CHECK(EmitSignExtend(f, 1, 8));
      case uint16_t(Op::I64Extend16S):
        CHECK(EmitSignExtend(f, 2, 8));
      case uint16_t(Op::I64Extend32S):
        CHECK(EmitSignExtend(f, 4, 8));

        // Gc operations
#ifdef ENABLE_WASM_GC
      case uint16_t(Op::GcPrefix): {
        return f.iter().unrecognizedOpcode(&op);
      }
#endif

      // SIMD operations
#ifdef ENABLE_WASM_SIMD
      case uint16_t(Op::SimdPrefix): {
        if (!f.moduleEnv().v128Enabled()) {
          return f.iter().unrecognizedOpcode(&op);
        }
        switch (op.b1) {
          case uint32_t(SimdOp::V128Const):
            CHECK(EmitConstSimd128(f));
          case uint32_t(SimdOp::V128Load):
            CHECK(EmitLoad(f, ValType::V128, Scalar::Simd128));
          case uint32_t(SimdOp::V128Store):
            CHECK(EmitStore(f, ValType::V128, Scalar::Simd128));
          case uint32_t(SimdOp::V128And):
          case uint32_t(SimdOp::V128Or):
          case uint32_t(SimdOp::V128Xor):
          case uint32_t(SimdOp::I8x16AvgrU):
          case uint32_t(SimdOp::I16x8AvgrU):
          case uint32_t(SimdOp::I8x16Add):
          case uint32_t(SimdOp::I8x16AddSaturateS):
          case uint32_t(SimdOp::I8x16AddSaturateU):
          case uint32_t(SimdOp::I8x16MinS):
          case uint32_t(SimdOp::I8x16MinU):
          case uint32_t(SimdOp::I8x16MaxS):
          case uint32_t(SimdOp::I8x16MaxU):
          case uint32_t(SimdOp::I16x8Add):
          case uint32_t(SimdOp::I16x8AddSaturateS):
          case uint32_t(SimdOp::I16x8AddSaturateU):
          case uint32_t(SimdOp::I16x8Mul):
          case uint32_t(SimdOp::I16x8MinS):
          case uint32_t(SimdOp::I16x8MinU):
          case uint32_t(SimdOp::I16x8MaxS):
          case uint32_t(SimdOp::I16x8MaxU):
          case uint32_t(SimdOp::I32x4Add):
          case uint32_t(SimdOp::I32x4Mul):
          case uint32_t(SimdOp::I32x4MinS):
          case uint32_t(SimdOp::I32x4MinU):
          case uint32_t(SimdOp::I32x4MaxS):
          case uint32_t(SimdOp::I32x4MaxU):
          case uint32_t(SimdOp::I64x2Add):
          case uint32_t(SimdOp::I64x2Mul):
          case uint32_t(SimdOp::F32x4Add):
          case uint32_t(SimdOp::F32x4Mul):
          case uint32_t(SimdOp::F32x4Min):
          case uint32_t(SimdOp::F32x4Max):
          case uint32_t(SimdOp::F64x2Add):
          case uint32_t(SimdOp::F64x2Mul):
          case uint32_t(SimdOp::F64x2Min):
          case uint32_t(SimdOp::F64x2Max):
          case uint32_t(SimdOp::I8x16Eq):
          case uint32_t(SimdOp::I8x16Ne):
          case uint32_t(SimdOp::I16x8Eq):
          case uint32_t(SimdOp::I16x8Ne):
          case uint32_t(SimdOp::I32x4Eq):
          case uint32_t(SimdOp::I32x4Ne):
          case uint32_t(SimdOp::I64x2Eq):
          case uint32_t(SimdOp::I64x2Ne):
          case uint32_t(SimdOp::F32x4Eq):
          case uint32_t(SimdOp::F32x4Ne):
          case uint32_t(SimdOp::F64x2Eq):
          case uint32_t(SimdOp::F64x2Ne):
          case uint32_t(SimdOp::I32x4DotSI16x8):
          case uint32_t(SimdOp::I16x8ExtMulLowSI8x16):
          case uint32_t(SimdOp::I16x8ExtMulHighSI8x16):
          case uint32_t(SimdOp::I16x8ExtMulLowUI8x16):
          case uint32_t(SimdOp::I16x8ExtMulHighUI8x16):
          case uint32_t(SimdOp::I32x4ExtMulLowSI16x8):
          case uint32_t(SimdOp::I32x4ExtMulHighSI16x8):
          case uint32_t(SimdOp::I32x4ExtMulLowUI16x8):
          case uint32_t(SimdOp::I32x4ExtMulHighUI16x8):
          case uint32_t(SimdOp::I64x2ExtMulLowSI32x4):
          case uint32_t(SimdOp::I64x2ExtMulHighSI32x4):
          case uint32_t(SimdOp::I64x2ExtMulLowUI32x4):
          case uint32_t(SimdOp::I64x2ExtMulHighUI32x4):
          case uint32_t(SimdOp::I16x8Q15MulrSatS):
            CHECK(EmitBinarySimd128(f, /* commutative= */ true, SimdOp(op.b1)));
          case uint32_t(SimdOp::V128AndNot):
          case uint32_t(SimdOp::I8x16Sub):
          case uint32_t(SimdOp::I8x16SubSaturateS):
          case uint32_t(SimdOp::I8x16SubSaturateU):
          case uint32_t(SimdOp::I16x8Sub):
          case uint32_t(SimdOp::I16x8SubSaturateS):
          case uint32_t(SimdOp::I16x8SubSaturateU):
          case uint32_t(SimdOp::I32x4Sub):
          case uint32_t(SimdOp::I64x2Sub):
          case uint32_t(SimdOp::F32x4Sub):
          case uint32_t(SimdOp::F32x4Div):
          case uint32_t(SimdOp::F64x2Sub):
          case uint32_t(SimdOp::F64x2Div):
          case uint32_t(SimdOp::I8x16NarrowSI16x8):
          case uint32_t(SimdOp::I8x16NarrowUI16x8):
          case uint32_t(SimdOp::I16x8NarrowSI32x4):
          case uint32_t(SimdOp::I16x8NarrowUI32x4):
          case uint32_t(SimdOp::I8x16LtS):
          case uint32_t(SimdOp::I8x16LtU):
          case uint32_t(SimdOp::I8x16GtS):
          case uint32_t(SimdOp::I8x16GtU):
          case uint32_t(SimdOp::I8x16LeS):
          case uint32_t(SimdOp::I8x16LeU):
          case uint32_t(SimdOp::I8x16GeS):
          case uint32_t(SimdOp::I8x16GeU):
          case uint32_t(SimdOp::I16x8LtS):
          case uint32_t(SimdOp::I16x8LtU):
          case uint32_t(SimdOp::I16x8GtS):
          case uint32_t(SimdOp::I16x8GtU):
          case uint32_t(SimdOp::I16x8LeS):
          case uint32_t(SimdOp::I16x8LeU):
          case uint32_t(SimdOp::I16x8GeS):
          case uint32_t(SimdOp::I16x8GeU):
          case uint32_t(SimdOp::I32x4LtS):
          case uint32_t(SimdOp::I32x4LtU):
          case uint32_t(SimdOp::I32x4GtS):
          case uint32_t(SimdOp::I32x4GtU):
          case uint32_t(SimdOp::I32x4LeS):
          case uint32_t(SimdOp::I32x4LeU):
          case uint32_t(SimdOp::I32x4GeS):
          case uint32_t(SimdOp::I32x4GeU):
          case uint32_t(SimdOp::I64x2LtS):
          case uint32_t(SimdOp::I64x2GtS):
          case uint32_t(SimdOp::I64x2LeS):
          case uint32_t(SimdOp::I64x2GeS):
          case uint32_t(SimdOp::F32x4Lt):
          case uint32_t(SimdOp::F32x4Gt):
          case uint32_t(SimdOp::F32x4Le):
          case uint32_t(SimdOp::F32x4Ge):
          case uint32_t(SimdOp::F64x2Lt):
          case uint32_t(SimdOp::F64x2Gt):
          case uint32_t(SimdOp::F64x2Le):
          case uint32_t(SimdOp::F64x2Ge):
          case uint32_t(SimdOp::V8x16Swizzle):
          case uint32_t(SimdOp::F32x4PMax):
          case uint32_t(SimdOp::F32x4PMin):
          case uint32_t(SimdOp::F64x2PMax):
          case uint32_t(SimdOp::F64x2PMin):
            CHECK(
                EmitBinarySimd128(f, /* commutative= */ false, SimdOp(op.b1)));
          case uint32_t(SimdOp::I8x16Splat):
          case uint32_t(SimdOp::I16x8Splat):
          case uint32_t(SimdOp::I32x4Splat):
            CHECK(EmitSplatSimd128(f, ValType::I32, SimdOp(op.b1)));
          case uint32_t(SimdOp::I64x2Splat):
            CHECK(EmitSplatSimd128(f, ValType::I64, SimdOp(op.b1)));
          case uint32_t(SimdOp::F32x4Splat):
            CHECK(EmitSplatSimd128(f, ValType::F32, SimdOp(op.b1)));
          case uint32_t(SimdOp::F64x2Splat):
            CHECK(EmitSplatSimd128(f, ValType::F64, SimdOp(op.b1)));
          case uint32_t(SimdOp::I8x16Neg):
          case uint32_t(SimdOp::I16x8Neg):
          case uint32_t(SimdOp::I16x8WidenLowSI8x16):
          case uint32_t(SimdOp::I16x8WidenHighSI8x16):
          case uint32_t(SimdOp::I16x8WidenLowUI8x16):
          case uint32_t(SimdOp::I16x8WidenHighUI8x16):
          case uint32_t(SimdOp::I32x4Neg):
          case uint32_t(SimdOp::I32x4WidenLowSI16x8):
          case uint32_t(SimdOp::I32x4WidenHighSI16x8):
          case uint32_t(SimdOp::I32x4WidenLowUI16x8):
          case uint32_t(SimdOp::I32x4WidenHighUI16x8):
          case uint32_t(SimdOp::I32x4TruncSSatF32x4):
          case uint32_t(SimdOp::I32x4TruncUSatF32x4):
          case uint32_t(SimdOp::I64x2Neg):
          case uint32_t(SimdOp::I64x2WidenLowSI32x4):
          case uint32_t(SimdOp::I64x2WidenHighSI32x4):
          case uint32_t(SimdOp::I64x2WidenLowUI32x4):
          case uint32_t(SimdOp::I64x2WidenHighUI32x4):
          case uint32_t(SimdOp::F32x4Abs):
          case uint32_t(SimdOp::F32x4Neg):
          case uint32_t(SimdOp::F32x4Sqrt):
          case uint32_t(SimdOp::F32x4ConvertSI32x4):
          case uint32_t(SimdOp::F32x4ConvertUI32x4):
          case uint32_t(SimdOp::F64x2Abs):
          case uint32_t(SimdOp::F64x2Neg):
          case uint32_t(SimdOp::F64x2Sqrt):
          case uint32_t(SimdOp::V128Not):
          case uint32_t(SimdOp::I8x16Popcnt):
          case uint32_t(SimdOp::I8x16Abs):
          case uint32_t(SimdOp::I16x8Abs):
          case uint32_t(SimdOp::I32x4Abs):
          case uint32_t(SimdOp::I64x2Abs):
          case uint32_t(SimdOp::F32x4Ceil):
          case uint32_t(SimdOp::F32x4Floor):
          case uint32_t(SimdOp::F32x4Trunc):
          case uint32_t(SimdOp::F32x4Nearest):
          case uint32_t(SimdOp::F64x2Ceil):
          case uint32_t(SimdOp::F64x2Floor):
          case uint32_t(SimdOp::F64x2Trunc):
          case uint32_t(SimdOp::F64x2Nearest):
          case uint32_t(SimdOp::F32x4DemoteF64x2Zero):
          case uint32_t(SimdOp::F64x2PromoteLowF32x4):
          case uint32_t(SimdOp::F64x2ConvertLowI32x4S):
          case uint32_t(SimdOp::F64x2ConvertLowI32x4U):
          case uint32_t(SimdOp::I32x4TruncSatF64x2SZero):
          case uint32_t(SimdOp::I32x4TruncSatF64x2UZero):
          case uint32_t(SimdOp::I16x8ExtAddPairwiseI8x16S):
          case uint32_t(SimdOp::I16x8ExtAddPairwiseI8x16U):
          case uint32_t(SimdOp::I32x4ExtAddPairwiseI16x8S):
          case uint32_t(SimdOp::I32x4ExtAddPairwiseI16x8U):
            CHECK(EmitUnarySimd128(f, SimdOp(op.b1)));
          case uint32_t(SimdOp::V128AnyTrue):
          case uint32_t(SimdOp::I8x16AllTrue):
          case uint32_t(SimdOp::I16x8AllTrue):
          case uint32_t(SimdOp::I32x4AllTrue):
          case uint32_t(SimdOp::I64x2AllTrue):
          case uint32_t(SimdOp::I8x16Bitmask):
          case uint32_t(SimdOp::I16x8Bitmask):
          case uint32_t(SimdOp::I32x4Bitmask):
          case uint32_t(SimdOp::I64x2Bitmask):
            CHECK(EmitReduceSimd128(f, SimdOp(op.b1)));
          case uint32_t(SimdOp::I8x16Shl):
          case uint32_t(SimdOp::I8x16ShrS):
          case uint32_t(SimdOp::I8x16ShrU):
          case uint32_t(SimdOp::I16x8Shl):
          case uint32_t(SimdOp::I16x8ShrS):
          case uint32_t(SimdOp::I16x8ShrU):
          case uint32_t(SimdOp::I32x4Shl):
          case uint32_t(SimdOp::I32x4ShrS):
          case uint32_t(SimdOp::I32x4ShrU):
          case uint32_t(SimdOp::I64x2Shl):
          case uint32_t(SimdOp::I64x2ShrS):
          case uint32_t(SimdOp::I64x2ShrU):
            CHECK(EmitShiftSimd128(f, SimdOp(op.b1)));
          case uint32_t(SimdOp::I8x16ExtractLaneS):
          case uint32_t(SimdOp::I8x16ExtractLaneU):
            CHECK(EmitExtractLaneSimd128(f, ValType::I32, 16, SimdOp(op.b1)));
          case uint32_t(SimdOp::I16x8ExtractLaneS):
          case uint32_t(SimdOp::I16x8ExtractLaneU):
            CHECK(EmitExtractLaneSimd128(f, ValType::I32, 8, SimdOp(op.b1)));
          case uint32_t(SimdOp::I32x4ExtractLane):
            CHECK(EmitExtractLaneSimd128(f, ValType::I32, 4, SimdOp(op.b1)));
          case uint32_t(SimdOp::I64x2ExtractLane):
            CHECK(EmitExtractLaneSimd128(f, ValType::I64, 2, SimdOp(op.b1)));
          case uint32_t(SimdOp::F32x4ExtractLane):
            CHECK(EmitExtractLaneSimd128(f, ValType::F32, 4, SimdOp(op.b1)));
          case uint32_t(SimdOp::F64x2ExtractLane):
            CHECK(EmitExtractLaneSimd128(f, ValType::F64, 2, SimdOp(op.b1)));
          case uint32_t(SimdOp::I8x16ReplaceLane):
            CHECK(EmitReplaceLaneSimd128(f, ValType::I32, 16, SimdOp(op.b1)));
          case uint32_t(SimdOp::I16x8ReplaceLane):
            CHECK(EmitReplaceLaneSimd128(f, ValType::I32, 8, SimdOp(op.b1)));
          case uint32_t(SimdOp::I32x4ReplaceLane):
            CHECK(EmitReplaceLaneSimd128(f, ValType::I32, 4, SimdOp(op.b1)));
          case uint32_t(SimdOp::I64x2ReplaceLane):
            CHECK(EmitReplaceLaneSimd128(f, ValType::I64, 2, SimdOp(op.b1)));
          case uint32_t(SimdOp::F32x4ReplaceLane):
            CHECK(EmitReplaceLaneSimd128(f, ValType::F32, 4, SimdOp(op.b1)));
          case uint32_t(SimdOp::F64x2ReplaceLane):
            CHECK(EmitReplaceLaneSimd128(f, ValType::F64, 2, SimdOp(op.b1)));
          case uint32_t(SimdOp::V128Bitselect):
            CHECK(EmitBitselectSimd128(f));
          case uint32_t(SimdOp::V8x16Shuffle):
            CHECK(EmitShuffleSimd128(f));
          case uint32_t(SimdOp::V8x16LoadSplat):
            CHECK(EmitLoadSplatSimd128(f, Scalar::Uint8, SimdOp::I8x16Splat));
          case uint32_t(SimdOp::V16x8LoadSplat):
            CHECK(EmitLoadSplatSimd128(f, Scalar::Uint16, SimdOp::I16x8Splat));
          case uint32_t(SimdOp::V32x4LoadSplat):
            CHECK(EmitLoadSplatSimd128(f, Scalar::Float32, SimdOp::I32x4Splat));
          case uint32_t(SimdOp::V64x2LoadSplat):
            CHECK(EmitLoadSplatSimd128(f, Scalar::Float64, SimdOp::I64x2Splat));
          case uint32_t(SimdOp::I16x8LoadS8x8):
          case uint32_t(SimdOp::I16x8LoadU8x8):
          case uint32_t(SimdOp::I32x4LoadS16x4):
          case uint32_t(SimdOp::I32x4LoadU16x4):
          case uint32_t(SimdOp::I64x2LoadS32x2):
          case uint32_t(SimdOp::I64x2LoadU32x2):
            CHECK(EmitLoadExtendSimd128(f, SimdOp(op.b1)));
          case uint32_t(SimdOp::V128Load32Zero):
            CHECK(EmitLoadZeroSimd128(f, Scalar::Float32, 4));
          case uint32_t(SimdOp::V128Load64Zero):
            CHECK(EmitLoadZeroSimd128(f, Scalar::Float64, 8));
          case uint32_t(SimdOp::V128Load8Lane):
            CHECK(EmitLoadLaneSimd128(f, 1));
          case uint32_t(SimdOp::V128Load16Lane):
            CHECK(EmitLoadLaneSimd128(f, 2));
          case uint32_t(SimdOp::V128Load32Lane):
            CHECK(EmitLoadLaneSimd128(f, 4));
          case uint32_t(SimdOp::V128Load64Lane):
            CHECK(EmitLoadLaneSimd128(f, 8));
          case uint32_t(SimdOp::V128Store8Lane):
            CHECK(EmitStoreLaneSimd128(f, 1));
          case uint32_t(SimdOp::V128Store16Lane):
            CHECK(EmitStoreLaneSimd128(f, 2));
          case uint32_t(SimdOp::V128Store32Lane):
            CHECK(EmitStoreLaneSimd128(f, 4));
          case uint32_t(SimdOp::V128Store64Lane):
            CHECK(EmitStoreLaneSimd128(f, 8));
          default:
            return f.iter().unrecognizedOpcode(&op);
        }  // switch (op.b1)
        break;
      }
#endif

      // Miscellaneous operations
      case uint16_t(Op::MiscPrefix): {
        switch (op.b1) {
          case uint32_t(MiscOp::I32TruncSSatF32):
          case uint32_t(MiscOp::I32TruncUSatF32):
            CHECK(EmitTruncate(f, ValType::F32, ValType::I32,
                               MiscOp(op.b1) == MiscOp::I32TruncUSatF32, true));
          case uint32_t(MiscOp::I32TruncSSatF64):
          case uint32_t(MiscOp::I32TruncUSatF64):
            CHECK(EmitTruncate(f, ValType::F64, ValType::I32,
                               MiscOp(op.b1) == MiscOp::I32TruncUSatF64, true));
          case uint32_t(MiscOp::I64TruncSSatF32):
          case uint32_t(MiscOp::I64TruncUSatF32):
            CHECK(EmitTruncate(f, ValType::F32, ValType::I64,
                               MiscOp(op.b1) == MiscOp::I64TruncUSatF32, true));
          case uint32_t(MiscOp::I64TruncSSatF64):
          case uint32_t(MiscOp::I64TruncUSatF64):
            CHECK(EmitTruncate(f, ValType::F64, ValType::I64,
                               MiscOp(op.b1) == MiscOp::I64TruncUSatF64, true));
          case uint32_t(MiscOp::MemCopy):
            CHECK(EmitMemCopy(f));
          case uint32_t(MiscOp::DataDrop):
            CHECK(EmitDataOrElemDrop(f, /*isData=*/true));
          case uint32_t(MiscOp::MemFill):
            CHECK(EmitMemFill(f));
          case uint32_t(MiscOp::MemInit):
            CHECK(EmitMemOrTableInit(f, /*isMem=*/true));
          case uint32_t(MiscOp::TableCopy):
            CHECK(EmitTableCopy(f));
          case uint32_t(MiscOp::ElemDrop):
            CHECK(EmitDataOrElemDrop(f, /*isData=*/false));
          case uint32_t(MiscOp::TableInit):
            CHECK(EmitMemOrTableInit(f, /*isMem=*/false));
          case uint32_t(MiscOp::TableFill):
            CHECK(EmitTableFill(f));
          case uint32_t(MiscOp::TableGrow):
            CHECK(EmitTableGrow(f));
          case uint32_t(MiscOp::TableSize):
            CHECK(EmitTableSize(f));
          default:
            return f.iter().unrecognizedOpcode(&op);
        }
        break;
      }

      // Thread operations
      case uint16_t(Op::ThreadPrefix): {
        // Though thread ops can be used on nonshared memories, we make them
        // unavailable if shared memory has been disabled in the prefs, for
        // maximum predictability and safety and consistency with JS.
        if (f.moduleEnv().sharedMemoryEnabled() == Shareable::False) {
          return f.iter().unrecognizedOpcode(&op);
        }
        switch (op.b1) {
          case uint32_t(ThreadOp::Wake):
            CHECK(EmitWake(f));

          case uint32_t(ThreadOp::I32Wait):
            CHECK(EmitWait(f, ValType::I32, 4));
          case uint32_t(ThreadOp::I64Wait):
            CHECK(EmitWait(f, ValType::I64, 8));
          case uint32_t(ThreadOp::Fence):
            CHECK(EmitFence(f));

          case uint32_t(ThreadOp::I32AtomicLoad):
            CHECK(EmitAtomicLoad(f, ValType::I32, Scalar::Int32));
          case uint32_t(ThreadOp::I64AtomicLoad):
            CHECK(EmitAtomicLoad(f, ValType::I64, Scalar::Int64));
          case uint32_t(ThreadOp::I32AtomicLoad8U):
            CHECK(EmitAtomicLoad(f, ValType::I32, Scalar::Uint8));
          case uint32_t(ThreadOp::I32AtomicLoad16U):
            CHECK(EmitAtomicLoad(f, ValType::I32, Scalar::Uint16));
          case uint32_t(ThreadOp::I64AtomicLoad8U):
            CHECK(EmitAtomicLoad(f, ValType::I64, Scalar::Uint8));
          case uint32_t(ThreadOp::I64AtomicLoad16U):
            CHECK(EmitAtomicLoad(f, ValType::I64, Scalar::Uint16));
          case uint32_t(ThreadOp::I64AtomicLoad32U):
            CHECK(EmitAtomicLoad(f, ValType::I64, Scalar::Uint32));

          case uint32_t(ThreadOp::I32AtomicStore):
            CHECK(EmitAtomicStore(f, ValType::I32, Scalar::Int32));
          case uint32_t(ThreadOp::I64AtomicStore):
            CHECK(EmitAtomicStore(f, ValType::I64, Scalar::Int64));
          case uint32_t(ThreadOp::I32AtomicStore8U):
            CHECK(EmitAtomicStore(f, ValType::I32, Scalar::Uint8));
          case uint32_t(ThreadOp::I32AtomicStore16U):
            CHECK(EmitAtomicStore(f, ValType::I32, Scalar::Uint16));
          case uint32_t(ThreadOp::I64AtomicStore8U):
            CHECK(EmitAtomicStore(f, ValType::I64, Scalar::Uint8));
          case uint32_t(ThreadOp::I64AtomicStore16U):
            CHECK(EmitAtomicStore(f, ValType::I64, Scalar::Uint16));
          case uint32_t(ThreadOp::I64AtomicStore32U):
            CHECK(EmitAtomicStore(f, ValType::I64, Scalar::Uint32));

          case uint32_t(ThreadOp::I32AtomicAdd):
            CHECK(EmitAtomicRMW(f, ValType::I32, Scalar::Int32,
                                AtomicFetchAddOp));
          case uint32_t(ThreadOp::I64AtomicAdd):
            CHECK(EmitAtomicRMW(f, ValType::I64, Scalar::Int64,
                                AtomicFetchAddOp));
          case uint32_t(ThreadOp::I32AtomicAdd8U):
            CHECK(EmitAtomicRMW(f, ValType::I32, Scalar::Uint8,
                                AtomicFetchAddOp));
          case uint32_t(ThreadOp::I32AtomicAdd16U):
            CHECK(EmitAtomicRMW(f, ValType::I32, Scalar::Uint16,
                                AtomicFetchAddOp));
          case uint32_t(ThreadOp::I64AtomicAdd8U):
            CHECK(EmitAtomicRMW(f, ValType::I64, Scalar::Uint8,
                                AtomicFetchAddOp));
          case uint32_t(ThreadOp::I64AtomicAdd16U):
            CHECK(EmitAtomicRMW(f, ValType::I64, Scalar::Uint16,
                                AtomicFetchAddOp));
          case uint32_t(ThreadOp::I64AtomicAdd32U):
            CHECK(EmitAtomicRMW(f, ValType::I64, Scalar::Uint32,
                                AtomicFetchAddOp));

          case uint32_t(ThreadOp::I32AtomicSub):
            CHECK(EmitAtomicRMW(f, ValType::I32, Scalar::Int32,
                                AtomicFetchSubOp));
          case uint32_t(ThreadOp::I64AtomicSub):
            CHECK(EmitAtomicRMW(f, ValType::I64, Scalar::Int64,
                                AtomicFetchSubOp));
          case uint32_t(ThreadOp::I32AtomicSub8U):
            CHECK(EmitAtomicRMW(f, ValType::I32, Scalar::Uint8,
                                AtomicFetchSubOp));
          case uint32_t(ThreadOp::I32AtomicSub16U):
            CHECK(EmitAtomicRMW(f, ValType::I32, Scalar::Uint16,
                                AtomicFetchSubOp));
          case uint32_t(ThreadOp::I64AtomicSub8U):
            CHECK(EmitAtomicRMW(f, ValType::I64, Scalar::Uint8,
                                AtomicFetchSubOp));
          case uint32_t(ThreadOp::I64AtomicSub16U):
            CHECK(EmitAtomicRMW(f, ValType::I64, Scalar::Uint16,
                                AtomicFetchSubOp));
          case uint32_t(ThreadOp::I64AtomicSub32U):
            CHECK(EmitAtomicRMW(f, ValType::I64, Scalar::Uint32,
                                AtomicFetchSubOp));

          case uint32_t(ThreadOp::I32AtomicAnd):
            CHECK(EmitAtomicRMW(f, ValType::I32, Scalar::Int32,
                                AtomicFetchAndOp));
          case uint32_t(ThreadOp::I64AtomicAnd):
            CHECK(EmitAtomicRMW(f, ValType::I64, Scalar::Int64,
                                AtomicFetchAndOp));
          case uint32_t(ThreadOp::I32AtomicAnd8U):
            CHECK(EmitAtomicRMW(f, ValType::I32, Scalar::Uint8,
                                AtomicFetchAndOp));
          case uint32_t(ThreadOp::I32AtomicAnd16U):
            CHECK(EmitAtomicRMW(f, ValType::I32, Scalar::Uint16,
                                AtomicFetchAndOp));
          case uint32_t(ThreadOp::I64AtomicAnd8U):
            CHECK(EmitAtomicRMW(f, ValType::I64, Scalar::Uint8,
                                AtomicFetchAndOp));
          case uint32_t(ThreadOp::I64AtomicAnd16U):
            CHECK(EmitAtomicRMW(f, ValType::I64, Scalar::Uint16,
                                AtomicFetchAndOp));
          case uint32_t(ThreadOp::I64AtomicAnd32U):
            CHECK(EmitAtomicRMW(f, ValType::I64, Scalar::Uint32,
                                AtomicFetchAndOp));

          case uint32_t(ThreadOp::I32AtomicOr):
            CHECK(
                EmitAtomicRMW(f, ValType::I32, Scalar::Int32, AtomicFetchOrOp));
          case uint32_t(ThreadOp::I64AtomicOr):
            CHECK(
                EmitAtomicRMW(f, ValType::I64, Scalar::Int64, AtomicFetchOrOp));
          case uint32_t(ThreadOp::I32AtomicOr8U):
            CHECK(
                EmitAtomicRMW(f, ValType::I32, Scalar::Uint8, AtomicFetchOrOp));
          case uint32_t(ThreadOp::I32AtomicOr16U):
            CHECK(EmitAtomicRMW(f, ValType::I32, Scalar::Uint16,
                                AtomicFetchOrOp));
          case uint32_t(ThreadOp::I64AtomicOr8U):
            CHECK(
                EmitAtomicRMW(f, ValType::I64, Scalar::Uint8, AtomicFetchOrOp));
          case uint32_t(ThreadOp::I64AtomicOr16U):
            CHECK(EmitAtomicRMW(f, ValType::I64, Scalar::Uint16,
                                AtomicFetchOrOp));
          case uint32_t(ThreadOp::I64AtomicOr32U):
            CHECK(EmitAtomicRMW(f, ValType::I64, Scalar::Uint32,
                                AtomicFetchOrOp));

          case uint32_t(ThreadOp::I32AtomicXor):
            CHECK(EmitAtomicRMW(f, ValType::I32, Scalar::Int32,
                                AtomicFetchXorOp));
          case uint32_t(ThreadOp::I64AtomicXor):
            CHECK(EmitAtomicRMW(f, ValType::I64, Scalar::Int64,
                                AtomicFetchXorOp));
          case uint32_t(ThreadOp::I32AtomicXor8U):
            CHECK(EmitAtomicRMW(f, ValType::I32, Scalar::Uint8,
                                AtomicFetchXorOp));
          case uint32_t(ThreadOp::I32AtomicXor16U):
            CHECK(EmitAtomicRMW(f, ValType::I32, Scalar::Uint16,
                                AtomicFetchXorOp));
          case uint32_t(ThreadOp::I64AtomicXor8U):
            CHECK(EmitAtomicRMW(f, ValType::I64, Scalar::Uint8,
                                AtomicFetchXorOp));
          case uint32_t(ThreadOp::I64AtomicXor16U):
            CHECK(EmitAtomicRMW(f, ValType::I64, Scalar::Uint16,
                                AtomicFetchXorOp));
          case uint32_t(ThreadOp::I64AtomicXor32U):
            CHECK(EmitAtomicRMW(f, ValType::I64, Scalar::Uint32,
                                AtomicFetchXorOp));

          case uint32_t(ThreadOp::I32AtomicXchg):
            CHECK(EmitAtomicXchg(f, ValType::I32, Scalar::Int32));
          case uint32_t(ThreadOp::I64AtomicXchg):
            CHECK(EmitAtomicXchg(f, ValType::I64, Scalar::Int64));
          case uint32_t(ThreadOp::I32AtomicXchg8U):
            CHECK(EmitAtomicXchg(f, ValType::I32, Scalar::Uint8));
          case uint32_t(ThreadOp::I32AtomicXchg16U):
            CHECK(EmitAtomicXchg(f, ValType::I32, Scalar::Uint16));
          case uint32_t(ThreadOp::I64AtomicXchg8U):
            CHECK(EmitAtomicXchg(f, ValType::I64, Scalar::Uint8));
          case uint32_t(ThreadOp::I64AtomicXchg16U):
            CHECK(EmitAtomicXchg(f, ValType::I64, Scalar::Uint16));
          case uint32_t(ThreadOp::I64AtomicXchg32U):
            CHECK(EmitAtomicXchg(f, ValType::I64, Scalar::Uint32));

          case uint32_t(ThreadOp::I32AtomicCmpXchg):
            CHECK(EmitAtomicCmpXchg(f, ValType::I32, Scalar::Int32));
          case uint32_t(ThreadOp::I64AtomicCmpXchg):
            CHECK(EmitAtomicCmpXchg(f, ValType::I64, Scalar::Int64));
          case uint32_t(ThreadOp::I32AtomicCmpXchg8U):
            CHECK(EmitAtomicCmpXchg(f, ValType::I32, Scalar::Uint8));
          case uint32_t(ThreadOp::I32AtomicCmpXchg16U):
            CHECK(EmitAtomicCmpXchg(f, ValType::I32, Scalar::Uint16));
          case uint32_t(ThreadOp::I64AtomicCmpXchg8U):
            CHECK(EmitAtomicCmpXchg(f, ValType::I64, Scalar::Uint8));
          case uint32_t(ThreadOp::I64AtomicCmpXchg16U):
            CHECK(EmitAtomicCmpXchg(f, ValType::I64, Scalar::Uint16));
          case uint32_t(ThreadOp::I64AtomicCmpXchg32U):
            CHECK(EmitAtomicCmpXchg(f, ValType::I64, Scalar::Uint32));

          default:
            return f.iter().unrecognizedOpcode(&op);
        }
        break;
      }

      // asm.js-specific operators
      case uint16_t(Op::MozPrefix): {
        if (!f.moduleEnv().isAsmJS()) {
          return f.iter().unrecognizedOpcode(&op);
        }
        switch (op.b1) {
          case uint32_t(MozOp::TeeGlobal):
            CHECK(EmitTeeGlobal(f));
          case uint32_t(MozOp::I32Min):
          case uint32_t(MozOp::I32Max):
            CHECK(EmitMinMax(f, ValType::I32, MIRType::Int32,
                             MozOp(op.b1) == MozOp::I32Max));
          case uint32_t(MozOp::I32Neg):
            CHECK(EmitUnaryWithType<MWasmNeg>(f, ValType::I32, MIRType::Int32));
          case uint32_t(MozOp::I32BitNot):
            CHECK(EmitBitNot(f, ValType::I32));
          case uint32_t(MozOp::I32Abs):
            CHECK(EmitUnaryWithType<MAbs>(f, ValType::I32, MIRType::Int32));
          case uint32_t(MozOp::F32TeeStoreF64):
            CHECK(EmitTeeStoreWithCoercion(f, ValType::F32, Scalar::Float64));
          case uint32_t(MozOp::F64TeeStoreF32):
            CHECK(EmitTeeStoreWithCoercion(f, ValType::F64, Scalar::Float32));
          case uint32_t(MozOp::I32TeeStore8):
            CHECK(EmitTeeStore(f, ValType::I32, Scalar::Int8));
          case uint32_t(MozOp::I32TeeStore16):
            CHECK(EmitTeeStore(f, ValType::I32, Scalar::Int16));
          case uint32_t(MozOp::I64TeeStore8):
            CHECK(EmitTeeStore(f, ValType::I64, Scalar::Int8));
          case uint32_t(MozOp::I64TeeStore16):
            CHECK(EmitTeeStore(f, ValType::I64, Scalar::Int16));
          case uint32_t(MozOp::I64TeeStore32):
            CHECK(EmitTeeStore(f, ValType::I64, Scalar::Int32));
          case uint32_t(MozOp::I32TeeStore):
            CHECK(EmitTeeStore(f, ValType::I32, Scalar::Int32));
          case uint32_t(MozOp::I64TeeStore):
            CHECK(EmitTeeStore(f, ValType::I64, Scalar::Int64));
          case uint32_t(MozOp::F32TeeStore):
            CHECK(EmitTeeStore(f, ValType::F32, Scalar::Float32));
          case uint32_t(MozOp::F64TeeStore):
            CHECK(EmitTeeStore(f, ValType::F64, Scalar::Float64));
          case uint32_t(MozOp::F64Mod):
            CHECK(EmitRem(f, ValType::F64, MIRType::Double,
                          /* isUnsigned = */ false));
          case uint32_t(MozOp::F64Sin):
            CHECK(EmitUnaryMathBuiltinCall(f, SASigSinD));
          case uint32_t(MozOp::F64Cos):
            CHECK(EmitUnaryMathBuiltinCall(f, SASigCosD));
          case uint32_t(MozOp::F64Tan):
            CHECK(EmitUnaryMathBuiltinCall(f, SASigTanD));
          case uint32_t(MozOp::F64Asin):
            CHECK(EmitUnaryMathBuiltinCall(f, SASigASinD));
          case uint32_t(MozOp::F64Acos):
            CHECK(EmitUnaryMathBuiltinCall(f, SASigACosD));
          case uint32_t(MozOp::F64Atan):
            CHECK(EmitUnaryMathBuiltinCall(f, SASigATanD));
          case uint32_t(MozOp::F64Exp):
            CHECK(EmitUnaryMathBuiltinCall(f, SASigExpD));
          case uint32_t(MozOp::F64Log):
            CHECK(EmitUnaryMathBuiltinCall(f, SASigLogD));
          case uint32_t(MozOp::F64Pow):
            CHECK(EmitBinaryMathBuiltinCall(f, SASigPowD));
          case uint32_t(MozOp::F64Atan2):
            CHECK(EmitBinaryMathBuiltinCall(f, SASigATan2D));
          case uint32_t(MozOp::OldCallDirect):
            CHECK(EmitCall(f, /* asmJSFuncDef = */ true));
          case uint32_t(MozOp::OldCallIndirect):
            CHECK(EmitCallIndirect(f, /* oldStyle = */ true));

          default:
            return f.iter().unrecognizedOpcode(&op);
        }
        break;
      }

      default:
        return f.iter().unrecognizedOpcode(&op);
    }
  }

  MOZ_CRASH("unreachable");

#undef CHECK
}

bool wasm::IonCompileFunctions(const ModuleEnvironment& moduleEnv,
                               const CompilerEnvironment& compilerEnv,
                               LifoAlloc& lifo,
                               const FuncCompileInputVector& inputs,
                               CompiledCode* code, UniqueChars* error) {
  MOZ_ASSERT(compilerEnv.tier() == Tier::Optimized);
  MOZ_ASSERT(compilerEnv.debug() == DebugEnabled::False);
  MOZ_ASSERT(compilerEnv.optimizedBackend() == OptimizedBackend::Ion);

  TempAllocator alloc(&lifo);
  JitContext jitContext(&alloc);
  MOZ_ASSERT(IsCompilingWasm());
  WasmMacroAssembler masm(alloc, moduleEnv);
#if defined(JS_CODEGEN_ARM64)
  masm.SetStackPointer64(PseudoStackPointer64);
#endif

  // Swap in already-allocated empty vectors to avoid malloc/free.
  MOZ_ASSERT(code->empty());
  if (!code->swap(masm)) {
    return false;
  }

  // Create a description of the stack layout created by GenerateTrapExit().
  MachineState trapExitLayout;
  size_t trapExitLayoutNumWords;
  GenerateTrapExitMachineState(&trapExitLayout, &trapExitLayoutNumWords);

  for (const FuncCompileInput& func : inputs) {
    JitSpewCont(JitSpew_Codegen, "\n");
    JitSpew(JitSpew_Codegen,
            "# ================================"
            "==================================");
    JitSpew(JitSpew_Codegen, "# ==");
    JitSpew(JitSpew_Codegen,
            "# wasm::IonCompileFunctions: starting on function index %d",
            (int)func.index);

    Decoder d(func.begin, func.end, func.lineOrBytecode, error);

    // Build the local types vector.

    const FuncType& funcType = *moduleEnv.funcs[func.index].type;
    const TypeIdDesc& funcTypeId = *moduleEnv.funcs[func.index].typeId;
    ValTypeVector locals;
    if (!locals.appendAll(funcType.args())) {
      return false;
    }
    if (!DecodeLocalEntries(d, moduleEnv.types, moduleEnv.features, &locals)) {
      return false;
    }

    // Set up for Ion compilation.

    const JitCompileOptions options;
    MIRGraph graph(&alloc);
    CompileInfo compileInfo(locals.length());
    MIRGenerator mir(nullptr, options, &alloc, &graph, &compileInfo,
                     IonOptimizations.get(OptimizationLevel::Wasm));
    if (moduleEnv.usesMemory()) {
      mir.initMinWasmHeapLength(moduleEnv.memory->initialLength32());
    }

    // Build MIR graph
    {
      FunctionCompiler f(moduleEnv, d, func, locals, mir);
      if (!f.init()) {
        return false;
      }

      if (!f.startBlock()) {
        return false;
      }

      if (!EmitBodyExprs(f)) {
        return false;
      }

      f.finish();
    }

    // Compile MIR graph
    {
      jit::SpewBeginWasmFunction(&mir, func.index);
      jit::AutoSpewEndFunction spewEndFunction(&mir);

      if (!OptimizeMIR(&mir)) {
        return false;
      }

      LIRGraph* lir = GenerateLIR(&mir);
      if (!lir) {
        return false;
      }

      CodeGenerator codegen(&mir, lir, &masm);

      BytecodeOffset prologueTrapOffset(func.lineOrBytecode);
      FuncOffsets offsets;
      ArgTypeVector args(funcType);
      if (!codegen.generateWasm(funcTypeId, prologueTrapOffset, args,
                                trapExitLayout, trapExitLayoutNumWords,
                                &offsets, &code->stackMaps)) {
        return false;
      }

      if (!code->codeRanges.emplaceBack(func.index, func.lineOrBytecode,
                                        offsets)) {
        return false;
      }
    }

    JitSpew(JitSpew_Codegen,
            "# wasm::IonCompileFunctions: completed function index %d",
            (int)func.index);
    JitSpew(JitSpew_Codegen, "# ==");
    JitSpew(JitSpew_Codegen,
            "# ================================"
            "==================================");
    JitSpewCont(JitSpew_Codegen, "\n");
  }

  masm.finish();
  if (masm.oom()) {
    return false;
  }

  return code->swap(masm);
}

bool js::wasm::IonPlatformSupport() {
#if defined(JS_CODEGEN_X64) || defined(JS_CODEGEN_X86) ||    \
    defined(JS_CODEGEN_ARM) || defined(JS_CODEGEN_MIPS32) || \
    defined(JS_CODEGEN_MIPS64) || defined(JS_CODEGEN_ARM64)
  return true;
#else
  return false;
#endif
}

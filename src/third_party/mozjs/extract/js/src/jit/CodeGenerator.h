/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_CodeGenerator_h
#define jit_CodeGenerator_h

#include "jit/PerfSpewer.h"
#include "js/ScalarType.h"  // js::Scalar::Type

#if defined(JS_CODEGEN_X86)
#  include "jit/x86/CodeGenerator-x86.h"
#elif defined(JS_CODEGEN_X64)
#  include "jit/x64/CodeGenerator-x64.h"
#elif defined(JS_CODEGEN_ARM)
#  include "jit/arm/CodeGenerator-arm.h"
#elif defined(JS_CODEGEN_ARM64)
#  include "jit/arm64/CodeGenerator-arm64.h"
#elif defined(JS_CODEGEN_MIPS32)
#  include "jit/mips32/CodeGenerator-mips32.h"
#elif defined(JS_CODEGEN_MIPS64)
#  include "jit/mips64/CodeGenerator-mips64.h"
#elif defined(JS_CODEGEN_LOONG64)
#  include "jit/loong64/CodeGenerator-loong64.h"
#elif defined(JS_CODEGEN_RISCV64)
#  include "jit/riscv64/CodeGenerator-riscv64.h"
#elif defined(JS_CODEGEN_WASM32)
#  include "jit/wasm32/CodeGenerator-wasm32.h"
#elif defined(JS_CODEGEN_NONE)
#  include "jit/none/CodeGenerator-none.h"
#else
#  error "Unknown architecture!"
#endif

namespace js {

namespace wasm {
class Decoder;
class StackMaps;
}  // namespace wasm

namespace jit {

class WarpSnapshot;

template <typename Fn, Fn fn, class ArgSeq, class StoreOutputTo>
class OutOfLineCallVM;

enum class SwitchTableType { Inline, OutOfLine };

template <SwitchTableType tableType>
class OutOfLineSwitch;
class OutOfLineTestObject;
class OutOfLineNewArray;
class OutOfLineNewObject;
class CheckOverRecursedFailure;
class OutOfLineUnboxFloatingPoint;
class OutOfLineStoreElementHole;
class OutOfLineTypeOfV;
class OutOfLineTypeOfIsNonPrimitiveV;
class OutOfLineTypeOfIsNonPrimitiveO;
class OutOfLineUpdateCache;
class OutOfLineICFallback;
class OutOfLineCallPostWriteBarrier;
class OutOfLineCallPostWriteElementBarrier;
class OutOfLineElementPostWriteBarrier;
class OutOfLineIsCallable;
class OutOfLineIsConstructor;
class OutOfLineRegExpMatcher;
class OutOfLineRegExpSearcher;
class OutOfLineRegExpExecMatch;
class OutOfLineRegExpExecTest;
class OutOfLineRegExpPrototypeOptimizable;
class OutOfLineRegExpInstanceOptimizable;
class OutOfLineNaNToZero;
class OutOfLineResumableWasmTrap;
class OutOfLineAbortingWasmTrap;
class OutOfLineGuardNumberToIntPtrIndex;
class OutOfLineBoxNonStrictThis;
class OutOfLineArrayPush;
class OutOfLineWasmCallPostWriteBarrier;

class CodeGenerator final : public CodeGeneratorSpecific {
  [[nodiscard]] bool generateBody();

  ConstantOrRegister toConstantOrRegister(LInstruction* lir, size_t n,
                                          MIRType type);

#ifdef CHECK_OSIPOINT_REGISTERS
  void resetOsiPointRegs(LSafepoint* safepoint);
  bool shouldVerifyOsiPointRegs(LSafepoint* safepoint);
  void verifyOsiPointRegs(LSafepoint* safepoint);
#endif

  void callVMInternal(VMFunctionId id, LInstruction* ins);

  template <typename Fn, Fn fn>
  void callVM(LInstruction* ins);

  template <typename Fn, Fn fn, class ArgSeq, class StoreOutputTo>
  inline OutOfLineCode* oolCallVM(LInstruction* ins, const ArgSeq& args,
                                  const StoreOutputTo& out);

  template <typename LCallIns>
  void emitCallNative(LCallIns* call, JSNative native);

 public:
  CodeGenerator(MIRGenerator* gen, LIRGraph* graph,
                MacroAssembler* masm = nullptr);
  ~CodeGenerator();

  [[nodiscard]] bool generate();
  [[nodiscard]] bool generateWasm(
      wasm::CallIndirectId callIndirectId, wasm::BytecodeOffset trapOffset,
      const wasm::ArgTypeVector& argTys, const RegisterOffsets& trapExitLayout,
      size_t trapExitLayoutNumWords, wasm::FuncOffsets* offsets,
      wasm::StackMaps* stackMaps, wasm::Decoder* decoder);

  [[nodiscard]] bool link(JSContext* cx, const WarpSnapshot* snapshot);

  void emitOOLTestObject(Register objreg, Label* ifTruthy, Label* ifFalsy,
                         Register scratch);
  void emitIntToString(Register input, Register output, Label* ool);

  void emitTypeOfCheck(JSValueType type, Register tag, Register output,
                       Label* done, Label* oolObject);
  void emitTypeOfJSType(JSValueType type, Register output);
  void emitTypeOfObject(Register obj, Register output, Label* done);
  void emitTypeOfIsObject(MTypeOfIs* mir, Register obj, Register output,
                          Label* success, Label* fail, Label* slowCheck);
  void emitTypeOfIsObjectOOL(MTypeOfIs* mir, Register obj, Register output);

  template <typename Fn, Fn fn, class ArgSeq, class StoreOutputTo>
  void visitOutOfLineCallVM(
      OutOfLineCallVM<Fn, fn, ArgSeq, StoreOutputTo>* ool);

  void visitOutOfLineRegExpMatcher(OutOfLineRegExpMatcher* ool);
  void visitOutOfLineRegExpSearcher(OutOfLineRegExpSearcher* ool);
  void visitOutOfLineRegExpExecMatch(OutOfLineRegExpExecMatch* ool);
  void visitOutOfLineRegExpExecTest(OutOfLineRegExpExecTest* ool);
  void visitOutOfLineRegExpPrototypeOptimizable(
      OutOfLineRegExpPrototypeOptimizable* ool);
  void visitOutOfLineRegExpInstanceOptimizable(
      OutOfLineRegExpInstanceOptimizable* ool);

  void visitOutOfLineTypeOfV(OutOfLineTypeOfV* ool);
  void visitOutOfLineTypeOfIsNonPrimitiveV(OutOfLineTypeOfIsNonPrimitiveV* ool);
  void visitOutOfLineTypeOfIsNonPrimitiveO(OutOfLineTypeOfIsNonPrimitiveO* ool);

  template <SwitchTableType tableType>
  void visitOutOfLineSwitch(OutOfLineSwitch<tableType>* ool);

  void visitOutOfLineIsCallable(OutOfLineIsCallable* ool);
  void visitOutOfLineIsConstructor(OutOfLineIsConstructor* ool);

  void visitOutOfLineNaNToZero(OutOfLineNaNToZero* ool);

  void visitOutOfLineResumableWasmTrap(OutOfLineResumableWasmTrap* ool);
  void visitOutOfLineAbortingWasmTrap(OutOfLineAbortingWasmTrap* ool);
  void visitCheckOverRecursedFailure(CheckOverRecursedFailure* ool);

  void visitOutOfLineUnboxFloatingPoint(OutOfLineUnboxFloatingPoint* ool);
  void visitOutOfLineStoreElementHole(OutOfLineStoreElementHole* ool);

  void visitOutOfLineBoxNonStrictThis(OutOfLineBoxNonStrictThis* ool);

  void visitOutOfLineICFallback(OutOfLineICFallback* ool);

  void visitOutOfLineCallPostWriteBarrier(OutOfLineCallPostWriteBarrier* ool);
  void visitOutOfLineCallPostWriteElementBarrier(
      OutOfLineCallPostWriteElementBarrier* ool);

  void visitOutOfLineElementPostWriteBarrier(
      OutOfLineElementPostWriteBarrier* ool);

  void visitOutOfLineNewArray(OutOfLineNewArray* ool);
  void visitOutOfLineNewObject(OutOfLineNewObject* ool);

  void visitOutOfLineGuardNumberToIntPtrIndex(
      OutOfLineGuardNumberToIntPtrIndex* ool);

  void visitOutOfLineArrayPush(OutOfLineArrayPush* ool);

  void visitOutOfLineWasmCallPostWriteBarrier(
      OutOfLineWasmCallPostWriteBarrier* ool);

 private:
  void emitPostWriteBarrier(const LAllocation* obj);
  void emitPostWriteBarrier(Register objreg);
  void emitPostWriteBarrierS(Address address, Register prev, Register next);

  void emitElementPostWriteBarrier(MInstruction* mir,
                                   const LiveRegisterSet& liveVolatileRegs,
                                   Register obj, const LAllocation* index,
                                   Register scratch,
                                   const ConstantOrRegister& val,
                                   int32_t indexDiff = 0);

  template <class LPostBarrierType, MIRType nurseryType>
  void visitPostWriteBarrierCommon(LPostBarrierType* lir, OutOfLineCode* ool);
  template <class LPostBarrierType>
  void visitPostWriteBarrierCommonV(LPostBarrierType* lir, OutOfLineCode* ool);

  void emitCallInvokeFunction(LInstruction* call, Register callereg,
                              bool isConstructing, bool ignoresReturnValue,
                              uint32_t argc, uint32_t unusedStack);
  template <typename T>
  void emitApplyGeneric(T* apply);
  template <typename T>
  void emitCallInvokeFunction(T* apply);
  void emitAllocateSpaceForApply(Register argcreg, Register scratch);
  void emitAllocateSpaceForConstructAndPushNewTarget(
      Register argcreg, Register newTargetAndScratch);
  void emitCopyValuesForApply(Register argvSrcBase, Register argvIndex,
                              Register copyreg, size_t argvSrcOffset,
                              size_t argvDstOffset);
  void emitRestoreStackPointerFromFP();
  void emitPushArguments(Register argcreg, Register scratch, Register copyreg,
                         uint32_t extraFormals);
  void emitPushArrayAsArguments(Register tmpArgc, Register srcBaseAndArgc,
                                Register scratch, size_t argvSrcOffset);
  void emitPushArguments(LApplyArgsGeneric* apply, Register scratch);
  void emitPushArguments(LApplyArgsObj* apply, Register scratch);
  void emitPushArguments(LApplyArrayGeneric* apply, Register scratch);
  void emitPushArguments(LConstructArgsGeneric* construct, Register scratch);
  void emitPushArguments(LConstructArrayGeneric* construct, Register scratch);

  template <class GetInlinedArgument>
  void emitGetInlinedArgument(GetInlinedArgument* lir, Register index,
                              ValueOperand output);

  using RegisterOrInt32 = mozilla::Variant<Register, int32_t>;

  static RegisterOrInt32 ToRegisterOrInt32(const LAllocation* allocation);

#ifdef DEBUG
  void emitAssertArgumentsSliceBounds(const RegisterOrInt32& begin,
                                      const RegisterOrInt32& count,
                                      Register numActualArgs);
#endif

  template <class ArgumentsSlice>
  void emitNewArray(ArgumentsSlice* lir, const RegisterOrInt32& count,
                    Register output, Register temp);

  void visitNewArrayCallVM(LNewArray* lir);
  void visitNewObjectVMCall(LNewObject* lir);

  void emitConcat(LInstruction* lir, Register lhs, Register rhs,
                  Register output);

  void emitInstanceOf(LInstruction* ins, Register protoReg);

  void loadJSScriptForBlock(MBasicBlock* block, Register reg);
  void loadOutermostJSScript(Register reg);

#ifdef DEBUG
  void emitAssertResultV(const ValueOperand output, const MDefinition* mir);
  void emitAssertGCThingResult(Register input, const MDefinition* mir);
#endif

#ifdef DEBUG
  void emitDebugForceBailing(LInstruction* lir);
#endif

  IonScriptCounts* extractScriptCounts() {
    IonScriptCounts* counts = scriptCounts_;
    scriptCounts_ = nullptr;  // prevent delete in dtor
    return counts;
  }

  void addGetPropertyCache(LInstruction* ins, LiveRegisterSet liveRegs,
                           TypedOrValueRegister value,
                           const ConstantOrRegister& id, ValueOperand output);
  void addSetPropertyCache(LInstruction* ins, LiveRegisterSet liveRegs,
                           Register objReg, Register temp,
                           const ConstantOrRegister& id,
                           const ConstantOrRegister& value, bool strict);

  template <class IteratorObject, class OrderedHashTable>
  void emitGetNextEntryForIterator(LGetNextEntryForIterator* lir);

  template <class OrderedHashTable>
  void emitLoadIteratorValues(Register result, Register temp, Register front);

  void emitStringToInt64(LInstruction* lir, Register input, Register64 output);

  OutOfLineCode* createBigIntOutOfLine(LInstruction* lir, Scalar::Type type,
                                       Register64 input, Register output);

  void emitCreateBigInt(LInstruction* lir, Scalar::Type type, Register64 input,
                        Register output, Register maybeTemp);

  template <size_t NumDefs>
  void emitIonToWasmCallBase(LIonToWasmCallBase<NumDefs>* lir);

  IonScriptCounts* maybeCreateScriptCounts();

  void emitWasmCompareAndSelect(LWasmCompareAndSelect* ins);

  void testValueTruthyForType(JSValueType type, ScratchTagScope& tag,
                              const ValueOperand& value, Register tempToUnbox,
                              Register temp, FloatRegister floatTemp,
                              Label* ifTruthy, Label* ifFalsy,
                              OutOfLineTestObject* ool, bool skipTypeTest);

  // Test whether value is truthy or not and jump to the corresponding label.
  // The control flow falls through when the object is truthy, as an
  // optimization.
  void testValueTruthy(const ValueOperand& value, Register tempToUnbox,
                       Register temp, FloatRegister floatTemp,
                       const TypeDataList& observedTypes, Label* ifTruthy,
                       Label* ifFalsy, OutOfLineTestObject* ool);

  // This function behaves like testObjectEmulatesUndefined with the exception
  // that it can choose to let control flow fall through when the object
  // doesn't emulate undefined, as an optimization. Use the regular
  // testObjectEmulatesUndefined when it's required to branch to one of the
  // two labels.
  void testObjectEmulatesUndefinedKernel(Register objreg,
                                         Label* ifEmulatesUndefined,
                                         Label* ifDoesntEmulateUndefined,
                                         Register scratch,
                                         OutOfLineTestObject* ool);

  // Test whether an object emulates |undefined|.  If it does, jump to
  // |ifEmulatesUndefined|; the caller is responsible for binding this label.
  // If it doesn't, fall through; the label |ifDoesntEmulateUndefined| (which
  // must be initially unbound) will be bound at this point.
  void branchTestObjectEmulatesUndefined(Register objreg,
                                         Label* ifEmulatesUndefined,
                                         Label* ifDoesntEmulateUndefined,
                                         Register scratch,
                                         OutOfLineTestObject* ool);

  // Test whether an object emulates |undefined|, and jump to the
  // corresponding label.
  //
  // This method should be used when subsequent code can't be laid out in a
  // straight line; if it can, branchTest* should be used instead.
  void testObjectEmulatesUndefined(Register objreg, Label* ifEmulatesUndefined,
                                   Label* ifDoesntEmulateUndefined,
                                   Register scratch, OutOfLineTestObject* ool);

  void emitStoreElementTyped(const LAllocation* value, MIRType valueType,
                             Register elements, const LAllocation* index);

  // Bailout if an element about to be written to is a hole.
  void emitStoreHoleCheck(Register elements, const LAllocation* index,
                          LSnapshot* snapshot);

  void emitAssertRangeI(MIRType type, const Range* r, Register input);
  void emitAssertRangeD(const Range* r, FloatRegister input,
                        FloatRegister temp);

  void maybeEmitGlobalBarrierCheck(const LAllocation* maybeGlobal,
                                   OutOfLineCode* ool);

  void incrementWarmUpCounter(AbsoluteAddress warmUpCount, JSScript* script,
                              Register tmp);

  Vector<CodeOffset, 0, JitAllocPolicy> ionScriptLabels_;

  // Used to bake in a pointer into the IonScript's list of nursery objects, for
  // MNurseryObject codegen.
  struct NurseryObjectLabel {
    CodeOffset offset;
    uint32_t nurseryIndex;
    NurseryObjectLabel(CodeOffset offset, uint32_t nurseryIndex)
        : offset(offset), nurseryIndex(nurseryIndex) {}
  };
  Vector<NurseryObjectLabel, 0, JitAllocPolicy> ionNurseryObjectLabels_;

  void branchIfInvalidated(Register temp, Label* invalidated);

#ifdef DEBUG
  void emitDebugResultChecks(LInstruction* ins);
  void emitGCThingResultChecks(LInstruction* lir, MDefinition* mir);
  void emitValueResultChecks(LInstruction* lir, MDefinition* mir);
#endif

  // Script counts created during code generation.
  IonScriptCounts* scriptCounts_;

  IonPerfSpewer perfSpewer_;

  // Bit mask of JitRealm stubs that are to be read-barriered.
  uint32_t realmStubsToReadBarrier_;

#ifdef FUZZING_JS_FUZZILLI
  void emitFuzzilliHashDouble(FloatRegister floatDouble, Register scratch,
                              Register output);
  void emitFuzzilliHashObject(LInstruction* lir, Register obj, Register output);
  void emitFuzzilliHashBigInt(Register bigInt, Register output);
#endif

#define LIR_OP(op) void visit##op(L##op* ins);
  LIR_OPCODE_LIST(LIR_OP)
#undef LIR_OP
};

class OutOfLineResumableWasmTrap : public OutOfLineCodeBase<CodeGenerator> {
  LInstruction* lir_;
  size_t framePushed_;
  wasm::BytecodeOffset bytecodeOffset_;
  wasm::Trap trap_;

 public:
  OutOfLineResumableWasmTrap(LInstruction* lir, size_t framePushed,
                             wasm::BytecodeOffset bytecodeOffset,
                             wasm::Trap trap)
      : lir_(lir),
        framePushed_(framePushed),
        bytecodeOffset_(bytecodeOffset),
        trap_(trap) {}

  void accept(CodeGenerator* codegen) override {
    codegen->visitOutOfLineResumableWasmTrap(this);
  }
  LInstruction* lir() const { return lir_; }
  size_t framePushed() const { return framePushed_; }
  wasm::BytecodeOffset bytecodeOffset() const { return bytecodeOffset_; }
  wasm::Trap trap() const { return trap_; }
};

class OutOfLineAbortingWasmTrap : public OutOfLineCodeBase<CodeGenerator> {
  wasm::BytecodeOffset bytecodeOffset_;
  wasm::Trap trap_;

 public:
  OutOfLineAbortingWasmTrap(wasm::BytecodeOffset bytecodeOffset,
                            wasm::Trap trap)
      : bytecodeOffset_(bytecodeOffset), trap_(trap) {}

  void accept(CodeGenerator* codegen) override {
    codegen->visitOutOfLineAbortingWasmTrap(this);
  }
  wasm::BytecodeOffset bytecodeOffset() const { return bytecodeOffset_; }
  wasm::Trap trap() const { return trap_; }
};

}  // namespace jit
}  // namespace js

#endif /* jit_CodeGenerator_h */

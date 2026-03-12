/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_CodeGenerator_h
#define jit_CodeGenerator_h

#include "jit/PerfSpewer.h"
#include "js/Prefs.h"
#include "js/ScalarType.h"  // js::Scalar::Type

#if defined(JS_CODEGEN_X86)
#  include "jit/x86/CodeGenerator-x86.h"
#elif defined(JS_CODEGEN_X64)
#  include "jit/x64/CodeGenerator-x64.h"
#elif defined(JS_CODEGEN_ARM)
#  include "jit/arm/CodeGenerator-arm.h"
#elif defined(JS_CODEGEN_ARM64)
#  include "jit/arm64/CodeGenerator-arm64.h"
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

class OutOfLineTestObject;
class OutOfLineICFallback;
class OutOfLineCallPostWriteBarrier;
class OutOfLineCallPostWriteElementBarrier;

class CodeGenerator final : public CodeGeneratorSpecific {
  // Warp snapshot. This is nullptr for Wasm compilations.
  const WarpSnapshot* snapshot_ = nullptr;

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
  void emitCallNative(LCallIns* call, JSNative native, Register argContextReg,
                      Register argUintNReg, Register argVpReg, Register tempReg,
                      uint32_t unusedStack);

  template <typename LCallIns>
  void emitCallNative(LCallIns* call, JSNative native);

 public:
  CodeGenerator(MIRGenerator* gen, LIRGraph* graph,
                MacroAssembler* masm = nullptr,
                const wasm::CodeMetadata* wasmCodeMeta = nullptr);
  ~CodeGenerator();

  [[nodiscard]] bool generate(const WarpSnapshot* snapshot);
  [[nodiscard]] bool generateWasm(wasm::CallIndirectId callIndirectId,
                                  const wasm::TrapSiteDesc& entryTrapSiteDesc,
                                  const wasm::ArgTypeVector& argTys,
                                  const RegisterOffsets& trapExitLayout,
                                  size_t trapExitLayoutNumWords,
                                  wasm::FuncOffsets* offsets,
                                  wasm::StackMaps* stackMaps,
                                  wasm::Decoder* decoder,
                                  jit::IonPerfSpewer* spewer);

  [[nodiscard]] bool link(JSContext* cx);

  void emitOOLTestObject(Register objreg, Label* ifTruthy, Label* ifFalsy,
                         Register scratch);

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

  void emitIsCallableOOL(Register object, Register output);

  void emitResumableWasmTrapOOL(LInstruction* lir, size_t framePushed,
                                const wasm::TrapSiteDesc& trapSiteDesc,
                                wasm::Trap trap);

  void visitOutOfLineICFallback(OutOfLineICFallback* ool);

  void visitOutOfLineCallPostWriteBarrier(OutOfLineCallPostWriteBarrier* ool);
  void visitOutOfLineCallPostWriteElementBarrier(
      OutOfLineCallPostWriteElementBarrier* ool);

  void callWasmStructAllocFun(LInstruction* lir, wasm::SymbolicAddress fun,
                              Register typeDefData, Register allocSite,
                              Register output,
                              const wasm::TrapSiteDesc& trapSiteDesc);

  void callWasmArrayAllocFun(LInstruction* lir, wasm::SymbolicAddress fun,
                             Register numElements, Register typeDefData,
                             Register allocSite, Register output,
                             const wasm::TrapSiteDesc& trapSiteDesc);

#ifdef ENABLE_WASM_JSPI
  void callWasmUpdateSuspenderState(wasm::UpdateSuspenderStateAction kind,
                                    Register suspender, Register temp);
  // Stack switching trampoline requires two arguments (suspender and data) to
  // be passed. The function prepares stack and registers according Wasm ABI.
  void prepareWasmStackSwitchTrampolineCall(Register suspender, Register data);
#endif

  void setCompilationTime(mozilla::TimeDuration duration) {
    compileTime_ = duration;
  }
  mozilla::TimeDuration getCompilationTime() const { return compileTime_; }

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
  void emitPushArguments(LApplyArgsGeneric* apply);
  void emitPushArguments(LApplyArgsObj* apply);
  void emitPushArguments(LApplyArrayGeneric* apply);
  void emitPushArguments(LConstructArgsGeneric* construct);
  void emitPushArguments(LConstructArrayGeneric* construct);

  template <typename T>
  void emitApplyNative(T* apply);
  template <typename T>
  void emitAlignStackForApplyNative(T* apply, Register argc);
  template <typename T>
  void emitPushNativeArguments(T* apply);
  template <typename T>
  void emitPushArrayAsNativeArguments(T* apply);
  void emitPushArguments(LApplyArgsNative* apply);
  void emitPushArguments(LApplyArgsObjNative* apply);
  void emitPushArguments(LApplyArrayNative* apply);
  void emitPushArguments(LConstructArgsNative* construct);
  void emitPushArguments(LConstructArrayNative* construct);

  template <typename T>
  void emitApplyArgsGuard(T* apply);

  template <typename T>
  void emitApplyArgsObjGuard(T* apply);

  template <typename T>
  void emitApplyArrayGuard(T* apply);

  template <class GetInlinedArgument>
  void emitGetInlinedArgument(GetInlinedArgument* lir, Register index,
                              ValueOperand output);

  void emitMaybeAtomizeSlot(LInstruction* ins, Register stringReg,
                            Address slotAddr, TypedOrValueRegister dest);

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

  template <class IteratorObject, class TableObject>
  void emitGetNextEntryForIterator(LGetNextEntryForIterator* lir);

  template <class TableObject>
  void emitLoadIteratorValues(Register result, Register temp, Register front);

  void emitStringToInt64(LInstruction* lir, Register input, Register64 output);

  OutOfLineCode* createBigIntOutOfLine(LInstruction* lir, Scalar::Type type,
                                       Register64 input, Register output);

  void emitCreateBigInt(LInstruction* lir, Scalar::Type type, Register64 input,
                        Register output, Register maybeTemp,
                        Register64 maybeTemp64 = Register64::Invalid());

  void emitCallMegamorphicGetter(LInstruction* lir,
                                 ValueOperand accessorAndOutput, Register obj,
                                 Register calleeScratch, Register argcScratch,
                                 Label* nullGetter);

  template <size_t NumDefs>
  void emitIonToWasmCallBase(LIonToWasmCallBase<NumDefs>* lir);

  IonScriptCounts* maybeCreateScriptCounts();

  template <typename InstructionWithMaybeTrapSite, class AddressOrBaseIndex>
  void emitWasmValueLoad(InstructionWithMaybeTrapSite* ins, MIRType type,
                         MWideningOp wideningOp, AddressOrBaseIndex addr,
                         AnyRegister dst);
  template <typename InstructionWithMaybeTrapSite, class AddressOrBaseIndex>
  void emitWasmValueStore(InstructionWithMaybeTrapSite* ins, MIRType type,
                          MNarrowingOp narrowingOp, AnyRegister src,
                          AddressOrBaseIndex addr);

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
  void emitWasmAnyrefResultChecks(LInstruction* lir, MDefinition* mir);
#endif

  // Script counts created during code generation.
  IonScriptCounts* scriptCounts_;

  IonPerfSpewer perfSpewer_;

  // Total Ion compilation time.
  mozilla::TimeDuration compileTime_;

#ifdef FUZZING_JS_FUZZILLI
  void emitFuzzilliHashObject(LInstruction* lir, Register obj, Register output);
  void emitFuzzilliHashBigInt(LInstruction* lir, Register bigInt,
                              Register output);
#endif

#define LIR_OP(op) void visit##op(L##op* ins);
  LIR_OPCODE_LIST(LIR_OP)
#undef LIR_OP

  // In debug mode, we need to validate that we've not made a mistake with the
  // fuse.
  void assertObjectDoesNotEmulateUndefined(Register input, Register temp,
                                           const MInstruction* mir);

  // Register a dependency on the HasSeenObjectEmulateUndefined fuse.
  bool addHasSeenObjectEmulateUndefinedFuseDependency();

  // Register a dependency on the HasSeenArrayExceedsInt32Length fuse.
  bool addHasSeenArrayExceedsInt32LengthFuseDependency();

  // Return true if the fuse is intact, and if the fuse is intact note the
  // dependency
  bool hasSeenObjectEmulateUndefinedFuseIntactAndDependencyNoted() {
    bool intact = gen->outerInfo().hasSeenObjectEmulateUndefinedFuseIntact();
    if (intact) {
      bool tryToAdd = addHasSeenObjectEmulateUndefinedFuseDependency();
      // If we oom, just pretend that the fuse is popped.
      return tryToAdd;
    }
    return false;
  }

  bool hasSeenArrayExceedsInt32LengthFuseIntactAndDependencyNoted() {
    bool intact = gen->outerInfo().hasSeenArrayExceedsInt32LengthFuseIntact();
    if (intact) {
      return addHasSeenArrayExceedsInt32LengthFuseDependency();
    }
    return false;
  }
};

}  // namespace jit
}  // namespace js

#endif /* jit_CodeGenerator_h */

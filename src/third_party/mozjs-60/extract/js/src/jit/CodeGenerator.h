/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_CodeGenerator_h
#define jit_CodeGenerator_h

#include "jit/CacheIR.h"
#if defined(JS_ION_PERF)
# include "jit/PerfSpewer.h"
#endif

#if defined(JS_CODEGEN_X86)
# include "jit/x86/CodeGenerator-x86.h"
#elif defined(JS_CODEGEN_X64)
# include "jit/x64/CodeGenerator-x64.h"
#elif defined(JS_CODEGEN_ARM)
# include "jit/arm/CodeGenerator-arm.h"
#elif defined(JS_CODEGEN_ARM64)
# include "jit/arm64/CodeGenerator-arm64.h"
#elif defined(JS_CODEGEN_MIPS32)
# include "jit/mips32/CodeGenerator-mips32.h"
#elif defined(JS_CODEGEN_MIPS64)
# include "jit/mips64/CodeGenerator-mips64.h"
#elif defined(JS_CODEGEN_NONE)
# include "jit/none/CodeGenerator-none.h"
#else
#error "Unknown architecture!"
#endif

namespace js {
namespace jit {

enum class SwitchTableType {
    Inline,
    OutOfLine
};

template <SwitchTableType tableType> class OutOfLineSwitch;
class OutOfLineTestObject;
class OutOfLineNewArray;
class OutOfLineNewObject;
class CheckOverRecursedFailure;
class OutOfLineInterruptCheckImplicit;
class OutOfLineUnboxFloatingPoint;
class OutOfLineStoreElementHole;
class OutOfLineTypeOfV;
class OutOfLineUpdateCache;
class OutOfLineICFallback;
class OutOfLineCallPostWriteBarrier;
class OutOfLineCallPostWriteElementBarrier;
class OutOfLineIsCallable;
class OutOfLineIsConstructor;
class OutOfLineRegExpMatcher;
class OutOfLineRegExpSearcher;
class OutOfLineRegExpTester;
class OutOfLineRegExpPrototypeOptimizable;
class OutOfLineRegExpInstanceOptimizable;
class OutOfLineLambdaArrow;
class OutOfLineNaNToZero;

class CodeGenerator final : public CodeGeneratorSpecific
{
    void generateArgumentsChecks(bool assert = false);
    MOZ_MUST_USE bool generateBody();

    ConstantOrRegister toConstantOrRegister(LInstruction* lir, size_t n, MIRType type);

  public:
    CodeGenerator(MIRGenerator* gen, LIRGraph* graph, MacroAssembler* masm = nullptr);
    ~CodeGenerator();

  public:
    MOZ_MUST_USE bool generate();
    MOZ_MUST_USE bool generateWasm(wasm::SigIdDesc sigId, wasm::BytecodeOffset trapOffset,
                                   wasm::FuncOffsets* offsets);
    MOZ_MUST_USE bool link(JSContext* cx, CompilerConstraintList* constraints);
    MOZ_MUST_USE bool linkSharedStubs(JSContext* cx);

    void visitOsiPoint(LOsiPoint* lir);
    void visitGoto(LGoto* lir);
    void visitTableSwitch(LTableSwitch* ins);
    void visitTableSwitchV(LTableSwitchV* ins);
    void visitCloneLiteral(LCloneLiteral* lir);
    void visitParameter(LParameter* lir);
    void visitCallee(LCallee* lir);
    void visitIsConstructing(LIsConstructing* lir);
    void visitStart(LStart* lir);
    void visitReturn(LReturn* ret);
    void visitDefVar(LDefVar* lir);
    void visitDefLexical(LDefLexical* lir);
    void visitDefFun(LDefFun* lir);
    void visitOsrEntry(LOsrEntry* lir);
    void visitOsrEnvironmentChain(LOsrEnvironmentChain* lir);
    void visitOsrValue(LOsrValue* lir);
    void visitOsrReturnValue(LOsrReturnValue* lir);
    void visitOsrArgumentsObject(LOsrArgumentsObject* lir);
    void visitStackArgT(LStackArgT* lir);
    void visitStackArgV(LStackArgV* lir);
    void visitMoveGroup(LMoveGroup* group);
    void visitValueToInt32(LValueToInt32* lir);
    void visitValueToDouble(LValueToDouble* lir);
    void visitValueToFloat32(LValueToFloat32* lir);
    void visitFloat32ToDouble(LFloat32ToDouble* lir);
    void visitDoubleToFloat32(LDoubleToFloat32* lir);
    void visitInt32ToFloat32(LInt32ToFloat32* lir);
    void visitInt32ToDouble(LInt32ToDouble* lir);
    void emitOOLTestObject(Register objreg, Label* ifTruthy, Label* ifFalsy, Register scratch);
    void visitTestOAndBranch(LTestOAndBranch* lir);
    void visitTestVAndBranch(LTestVAndBranch* lir);
    void visitFunctionDispatch(LFunctionDispatch* lir);
    void visitObjectGroupDispatch(LObjectGroupDispatch* lir);
    void visitBooleanToString(LBooleanToString* lir);
    void emitIntToString(Register input, Register output, Label* ool);
    void visitIntToString(LIntToString* lir);
    void visitDoubleToString(LDoubleToString* lir);
    void visitValueToString(LValueToString* lir);
    void visitValueToObject(LValueToObject* lir);
    void visitValueToObjectOrNull(LValueToObjectOrNull* lir);
    void visitInteger(LInteger* lir);
    void visitInteger64(LInteger64* lir);
    void visitRegExp(LRegExp* lir);
    void visitRegExpMatcher(LRegExpMatcher* lir);
    void visitOutOfLineRegExpMatcher(OutOfLineRegExpMatcher* ool);
    void visitRegExpSearcher(LRegExpSearcher* lir);
    void visitOutOfLineRegExpSearcher(OutOfLineRegExpSearcher* ool);
    void visitRegExpTester(LRegExpTester* lir);
    void visitOutOfLineRegExpTester(OutOfLineRegExpTester* ool);
    void visitRegExpPrototypeOptimizable(LRegExpPrototypeOptimizable* lir);
    void visitOutOfLineRegExpPrototypeOptimizable(OutOfLineRegExpPrototypeOptimizable* ool);
    void visitRegExpInstanceOptimizable(LRegExpInstanceOptimizable* lir);
    void visitOutOfLineRegExpInstanceOptimizable(OutOfLineRegExpInstanceOptimizable* ool);
    void visitGetFirstDollarIndex(LGetFirstDollarIndex* lir);
    void visitStringReplace(LStringReplace* lir);
    void emitSharedStub(ICStub::Kind kind, LInstruction* lir);
    void visitBinarySharedStub(LBinarySharedStub* lir);
    void visitUnarySharedStub(LUnarySharedStub* lir);
    void visitNullarySharedStub(LNullarySharedStub* lir);
    void visitClassConstructor(LClassConstructor* lir);
    void visitLambda(LLambda* lir);
    void visitOutOfLineLambdaArrow(OutOfLineLambdaArrow* ool);
    void visitLambdaArrow(LLambdaArrow* lir);
    void visitLambdaForSingleton(LLambdaForSingleton* lir);
    void visitSetFunName(LSetFunName* lir);
    void visitPointer(LPointer* lir);
    void visitKeepAliveObject(LKeepAliveObject* lir);
    void visitSlots(LSlots* lir);
    void visitLoadSlotT(LLoadSlotT* lir);
    void visitLoadSlotV(LLoadSlotV* lir);
    void visitStoreSlotT(LStoreSlotT* lir);
    void visitStoreSlotV(LStoreSlotV* lir);
    void visitElements(LElements* lir);
    void visitConvertElementsToDoubles(LConvertElementsToDoubles* lir);
    void visitMaybeToDoubleElement(LMaybeToDoubleElement* lir);
    void visitMaybeCopyElementsForWrite(LMaybeCopyElementsForWrite* lir);
    void visitGuardShape(LGuardShape* guard);
    void visitGuardObjectGroup(LGuardObjectGroup* guard);
    void visitGuardObjectIdentity(LGuardObjectIdentity* guard);
    void visitGuardReceiverPolymorphic(LGuardReceiverPolymorphic* lir);
    void visitGuardUnboxedExpando(LGuardUnboxedExpando* lir);
    void visitLoadUnboxedExpando(LLoadUnboxedExpando* lir);
    void visitTypeBarrierV(LTypeBarrierV* lir);
    void visitTypeBarrierO(LTypeBarrierO* lir);
    void emitPostWriteBarrier(const LAllocation* obj);
    void emitPostWriteBarrier(Register objreg);
    void emitPostWriteBarrierS(Address address, Register prev, Register next);
    template <class LPostBarrierType, MIRType nurseryType>
    void visitPostWriteBarrierCommon(LPostBarrierType* lir, OutOfLineCode* ool);
    template <class LPostBarrierType>
    void visitPostWriteBarrierCommonV(LPostBarrierType* lir, OutOfLineCode* ool);
    void visitPostWriteBarrierO(LPostWriteBarrierO* lir);
    void visitPostWriteElementBarrierO(LPostWriteElementBarrierO* lir);
    void visitPostWriteBarrierV(LPostWriteBarrierV* lir);
    void visitPostWriteElementBarrierV(LPostWriteElementBarrierV* lir);
    void visitPostWriteBarrierS(LPostWriteBarrierS* lir);
    void visitPostWriteElementBarrierS(LPostWriteElementBarrierS* lir);
    void visitOutOfLineCallPostWriteBarrier(OutOfLineCallPostWriteBarrier* ool);
    void visitOutOfLineCallPostWriteElementBarrier(OutOfLineCallPostWriteElementBarrier* ool);
    void visitCallNative(LCallNative* call);
    void emitCallInvokeFunction(LInstruction* call, Register callereg,
                                bool isConstructing, bool ignoresReturnValue,
                                uint32_t argc, uint32_t unusedStack);
    void visitCallGeneric(LCallGeneric* call);
    void emitCallInvokeFunctionShuffleNewTarget(LCallKnown *call,
                                                Register calleeReg,
                                                uint32_t numFormals,
                                                uint32_t unusedStack);
    void visitCallKnown(LCallKnown* call);
    template<typename T> void emitApplyGeneric(T* apply);
    template<typename T> void emitCallInvokeFunction(T* apply, Register extraStackSize);
    void emitAllocateSpaceForApply(Register argcreg, Register extraStackSpace, Label* end);
    void emitCopyValuesForApply(Register argvSrcBase, Register argvIndex, Register copyreg,
                                size_t argvSrcOffset, size_t argvDstOffset);
    void emitPopArguments(Register extraStackSize);
    void emitPushArguments(LApplyArgsGeneric* apply, Register extraStackSpace);
    void visitApplyArgsGeneric(LApplyArgsGeneric* apply);
    void emitPushArguments(LApplyArrayGeneric* apply, Register extraStackSpace);
    void visitApplyArrayGeneric(LApplyArrayGeneric* apply);
    void visitBail(LBail* lir);
    void visitUnreachable(LUnreachable* unreachable);
    void visitEncodeSnapshot(LEncodeSnapshot* lir);
    void visitGetDynamicName(LGetDynamicName* lir);
    void visitCallDirectEval(LCallDirectEval* lir);
    void visitDoubleToInt32(LDoubleToInt32* lir);
    void visitFloat32ToInt32(LFloat32ToInt32* lir);
    void visitNewArrayCallVM(LNewArray* lir);
    void visitNewArray(LNewArray* lir);
    void visitOutOfLineNewArray(OutOfLineNewArray* ool);
    void visitNewArrayCopyOnWrite(LNewArrayCopyOnWrite* lir);
    void visitNewArrayDynamicLength(LNewArrayDynamicLength* lir);
    void visitNewIterator(LNewIterator* lir);
    void visitNewTypedArray(LNewTypedArray* lir);
    void visitNewTypedArrayDynamicLength(LNewTypedArrayDynamicLength* lir);
    void visitNewObjectVMCall(LNewObject* lir);
    void visitNewObject(LNewObject* lir);
    void visitOutOfLineNewObject(OutOfLineNewObject* ool);
    void visitNewTypedObject(LNewTypedObject* lir);
    void visitSimdBox(LSimdBox* lir);
    void visitSimdUnbox(LSimdUnbox* lir);
    void visitNewNamedLambdaObject(LNewNamedLambdaObject* lir);
    void visitNewCallObject(LNewCallObject* lir);
    void visitNewSingletonCallObject(LNewSingletonCallObject* lir);
    void visitNewStringObject(LNewStringObject* lir);
    void visitNewDerivedTypedObject(LNewDerivedTypedObject* lir);
    void visitInitElem(LInitElem* lir);
    void visitInitElemGetterSetter(LInitElemGetterSetter* lir);
    void visitMutateProto(LMutateProto* lir);
    void visitInitPropGetterSetter(LInitPropGetterSetter* lir);
    void visitCreateThis(LCreateThis* lir);
    void visitCreateThisWithProto(LCreateThisWithProto* lir);
    void visitCreateThisWithTemplate(LCreateThisWithTemplate* lir);
    void visitCreateArgumentsObject(LCreateArgumentsObject* lir);
    void visitGetArgumentsObjectArg(LGetArgumentsObjectArg* lir);
    void visitSetArgumentsObjectArg(LSetArgumentsObjectArg* lir);
    void visitReturnFromCtor(LReturnFromCtor* lir);
    void visitComputeThis(LComputeThis* lir);
    void visitImplicitThis(LImplicitThis* lir);
    void visitArrayLength(LArrayLength* lir);
    void visitSetArrayLength(LSetArrayLength* lir);
    void visitGetNextEntryForIterator(LGetNextEntryForIterator* lir);
    void visitTypedArrayLength(LTypedArrayLength* lir);
    void visitTypedArrayElements(LTypedArrayElements* lir);
    void visitSetDisjointTypedElements(LSetDisjointTypedElements* lir);
    void visitTypedObjectElements(LTypedObjectElements* lir);
    void visitSetTypedObjectOffset(LSetTypedObjectOffset* lir);
    void visitTypedObjectDescr(LTypedObjectDescr* ins);
    void visitStringLength(LStringLength* lir);
    void visitSubstr(LSubstr* lir);
    void visitInitializedLength(LInitializedLength* lir);
    void visitSetInitializedLength(LSetInitializedLength* lir);
    void visitNotO(LNotO* ins);
    void visitNotV(LNotV* ins);
    void visitBoundsCheck(LBoundsCheck* lir);
    void visitBoundsCheckRange(LBoundsCheckRange* lir);
    void visitBoundsCheckLower(LBoundsCheckLower* lir);
    void visitSpectreMaskIndex(LSpectreMaskIndex* lir);
    void visitLoadFixedSlotV(LLoadFixedSlotV* ins);
    void visitLoadFixedSlotAndUnbox(LLoadFixedSlotAndUnbox* lir);
    void visitLoadFixedSlotT(LLoadFixedSlotT* ins);
    void visitStoreFixedSlotV(LStoreFixedSlotV* ins);
    void visitStoreFixedSlotT(LStoreFixedSlotT* ins);
    void emitGetPropertyPolymorphic(LInstruction* lir, Register obj, Register expandoScratch,
                                    Register scratch, const TypedOrValueRegister& output);
    void visitGetPropertyPolymorphicV(LGetPropertyPolymorphicV* ins);
    void visitGetPropertyPolymorphicT(LGetPropertyPolymorphicT* ins);
    void emitSetPropertyPolymorphic(LInstruction* lir, Register obj, Register expandoScratch,
                                    Register scratch, const ConstantOrRegister& value);
    void visitSetPropertyPolymorphicV(LSetPropertyPolymorphicV* ins);
    void visitSetPropertyPolymorphicT(LSetPropertyPolymorphicT* ins);
    void visitAbsI(LAbsI* lir);
    void visitAtan2D(LAtan2D* lir);
    void visitHypot(LHypot* lir);
    void visitPowI(LPowI* lir);
    void visitPowD(LPowD* lir);
    void visitPowV(LPowV* lir);
    void visitMathFunctionD(LMathFunctionD* ins);
    void visitMathFunctionF(LMathFunctionF* ins);
    void visitModD(LModD* ins);
    void visitMinMaxI(LMinMaxI* lir);
    void visitBinaryV(LBinaryV* lir);
    void emitCompareS(LInstruction* lir, JSOp op, Register left, Register right, Register output);
    void visitCompareS(LCompareS* lir);
    void visitCompareStrictS(LCompareStrictS* lir);
    void visitCompareVM(LCompareVM* lir);
    void visitIsNullOrLikeUndefinedV(LIsNullOrLikeUndefinedV* lir);
    void visitIsNullOrLikeUndefinedT(LIsNullOrLikeUndefinedT* lir);
    void visitIsNullOrLikeUndefinedAndBranchV(LIsNullOrLikeUndefinedAndBranchV* lir);
    void visitIsNullOrLikeUndefinedAndBranchT(LIsNullOrLikeUndefinedAndBranchT* lir);
    void emitSameValue(FloatRegister left, FloatRegister right, FloatRegister temp,
                       Register output);
    void visitSameValueD(LSameValueD* lir);
    void visitSameValueV(LSameValueV* lir);
    void visitSameValueVM(LSameValueVM* lir);
    void emitConcat(LInstruction* lir, Register lhs, Register rhs, Register output);
    void visitConcat(LConcat* lir);
    void visitCharCodeAt(LCharCodeAt* lir);
    void visitFromCharCode(LFromCharCode* lir);
    void visitFromCodePoint(LFromCodePoint* lir);
    void visitStringConvertCase(LStringConvertCase* lir);
    void visitSinCos(LSinCos *lir);
    void visitStringSplit(LStringSplit* lir);
    void visitFunctionEnvironment(LFunctionEnvironment* lir);
    void visitHomeObject(LHomeObject* lir);
    void visitHomeObjectSuperBase(LHomeObjectSuperBase* lir);
    void visitNewLexicalEnvironmentObject(LNewLexicalEnvironmentObject* lir);
    void visitCopyLexicalEnvironmentObject(LCopyLexicalEnvironmentObject* lir);
    void visitCallGetProperty(LCallGetProperty* lir);
    void visitCallGetElement(LCallGetElement* lir);
    void visitCallSetElement(LCallSetElement* lir);
    void visitCallInitElementArray(LCallInitElementArray* lir);
    void visitThrow(LThrow* lir);
    void visitTypeOfV(LTypeOfV* lir);
    void visitOutOfLineTypeOfV(OutOfLineTypeOfV* ool);
    void visitToAsync(LToAsync* lir);
    void visitToAsyncGen(LToAsyncGen* lir);
    void visitToAsyncIter(LToAsyncIter* lir);
    void visitToIdV(LToIdV* lir);
    template<typename T> void emitLoadElementT(LLoadElementT* lir, const T& source);
    void visitLoadElementT(LLoadElementT* lir);
    void visitLoadElementV(LLoadElementV* load);
    void visitLoadElementHole(LLoadElementHole* lir);
    void visitLoadUnboxedPointerV(LLoadUnboxedPointerV* lir);
    void visitLoadUnboxedPointerT(LLoadUnboxedPointerT* lir);
    void visitUnboxObjectOrNull(LUnboxObjectOrNull* lir);
    template <SwitchTableType tableType>
    void visitOutOfLineSwitch(OutOfLineSwitch<tableType>* ool);
    void visitLoadElementFromStateV(LLoadElementFromStateV* lir);
    void visitStoreElementT(LStoreElementT* lir);
    void visitStoreElementV(LStoreElementV* lir);
    template <typename T> void emitStoreElementHoleT(T* lir);
    template <typename T> void emitStoreElementHoleV(T* lir);
    void visitStoreElementHoleT(LStoreElementHoleT* lir);
    void visitStoreElementHoleV(LStoreElementHoleV* lir);
    void visitFallibleStoreElementV(LFallibleStoreElementV* lir);
    void visitFallibleStoreElementT(LFallibleStoreElementT* lir);
    void visitStoreUnboxedPointer(LStoreUnboxedPointer* lir);
    void visitConvertUnboxedObjectToNative(LConvertUnboxedObjectToNative* lir);
    void emitArrayPopShift(LInstruction* lir, const MArrayPopShift* mir, Register obj,
                           Register elementsTemp, Register lengthTemp, TypedOrValueRegister out);
    void visitArrayPopShiftV(LArrayPopShiftV* lir);
    void visitArrayPopShiftT(LArrayPopShiftT* lir);
    void emitArrayPush(LInstruction* lir, Register obj,
                       const ConstantOrRegister& value, Register elementsTemp, Register length,
                       Register spectreTemp);
    void visitArrayPushV(LArrayPushV* lir);
    void visitArrayPushT(LArrayPushT* lir);
    void visitArraySlice(LArraySlice* lir);
    void visitArrayJoin(LArrayJoin* lir);
    void visitLoadUnboxedScalar(LLoadUnboxedScalar* lir);
    void visitLoadTypedArrayElementHole(LLoadTypedArrayElementHole* lir);
    void visitStoreUnboxedScalar(LStoreUnboxedScalar* lir);
    void visitStoreTypedArrayElementHole(LStoreTypedArrayElementHole* lir);
    void visitAtomicIsLockFree(LAtomicIsLockFree* lir);
    void visitGuardSharedTypedArray(LGuardSharedTypedArray* lir);
    void visitClampIToUint8(LClampIToUint8* lir);
    void visitClampDToUint8(LClampDToUint8* lir);
    void visitClampVToUint8(LClampVToUint8* lir);
    void visitGetIteratorCache(LGetIteratorCache* lir);
    void visitIteratorMore(LIteratorMore* lir);
    void visitIsNoIterAndBranch(LIsNoIterAndBranch* lir);
    void visitIteratorEnd(LIteratorEnd* lir);
    void visitArgumentsLength(LArgumentsLength* lir);
    void visitGetFrameArgument(LGetFrameArgument* lir);
    void visitSetFrameArgumentT(LSetFrameArgumentT* lir);
    void visitSetFrameArgumentC(LSetFrameArgumentC* lir);
    void visitSetFrameArgumentV(LSetFrameArgumentV* lir);
    void visitRunOncePrologue(LRunOncePrologue* lir);
    void emitRest(LInstruction* lir, Register array, Register numActuals,
                  Register temp0, Register temp1, unsigned numFormals,
                  JSObject* templateObject, bool saveAndRestore, Register resultreg);
    void visitRest(LRest* lir);
    void visitCallSetProperty(LCallSetProperty* ins);
    void visitCallDeleteProperty(LCallDeleteProperty* lir);
    void visitCallDeleteElement(LCallDeleteElement* lir);
    void visitBitNotV(LBitNotV* lir);
    void visitBitOpV(LBitOpV* lir);
    void emitInstanceOf(LInstruction* ins, JSObject* prototypeObject);
    void visitInCache(LInCache* ins);
    void visitInArray(LInArray* ins);
    void visitInstanceOfO(LInstanceOfO* ins);
    void visitInstanceOfV(LInstanceOfV* ins);
    void visitInstanceOfCache(LInstanceOfCache* ins);
    void visitGetDOMProperty(LGetDOMProperty* lir);
    void visitGetDOMMemberV(LGetDOMMemberV* lir);
    void visitGetDOMMemberT(LGetDOMMemberT* lir);
    void visitSetDOMProperty(LSetDOMProperty* lir);
    void visitCallDOMNative(LCallDOMNative* lir);
    void visitCallGetIntrinsicValue(LCallGetIntrinsicValue* lir);
    void visitCallBindVar(LCallBindVar* lir);
    enum CallableOrConstructor {
        Callable,
        Constructor
    };
    template <CallableOrConstructor mode>
    void emitIsCallableOrConstructor(Register object, Register output, Label* failure);
    void visitIsCallableO(LIsCallableO* lir);
    void visitIsCallableV(LIsCallableV* lir);
    void visitOutOfLineIsCallable(OutOfLineIsCallable* ool);
    void visitIsConstructor(LIsConstructor* lir);
    void visitOutOfLineIsConstructor(OutOfLineIsConstructor* ool);
    void visitIsArrayO(LIsArrayO* lir);
    void visitIsArrayV(LIsArrayV* lir);
    void visitIsTypedArray(LIsTypedArray* lir);
    void visitIsObject(LIsObject* lir);
    void visitIsObjectAndBranch(LIsObjectAndBranch* lir);
    void visitHasClass(LHasClass* lir);
    void visitGuardToClass(LGuardToClass* lir);
    void visitObjectClassToString(LObjectClassToString* lir);
    void visitWasmParameter(LWasmParameter* lir);
    void visitWasmParameterI64(LWasmParameterI64* lir);
    void visitWasmReturn(LWasmReturn* ret);
    void visitWasmReturnI64(LWasmReturnI64* ret);
    void visitWasmReturnVoid(LWasmReturnVoid* ret);
    void visitLexicalCheck(LLexicalCheck* ins);
    void visitThrowRuntimeLexicalError(LThrowRuntimeLexicalError* ins);
    void visitGlobalNameConflictsCheck(LGlobalNameConflictsCheck* ins);
    void visitDebugger(LDebugger* ins);
    void visitNewTarget(LNewTarget* ins);
    void visitArrowNewTarget(LArrowNewTarget* ins);
    void visitCheckReturn(LCheckReturn* ins);
    void visitCheckIsObj(LCheckIsObj* ins);
    void visitCheckIsCallable(LCheckIsCallable* ins);
    void visitCheckObjCoercible(LCheckObjCoercible* ins);
    void visitDebugCheckSelfHosted(LDebugCheckSelfHosted* ins);
    void visitNaNToZero(LNaNToZero* ins);
    void visitOutOfLineNaNToZero(OutOfLineNaNToZero* ool);
    void visitFinishBoundFunctionInit(LFinishBoundFunctionInit* lir);
    void visitIsPackedArray(LIsPackedArray* lir);
    void visitGetPrototypeOf(LGetPrototypeOf* lir);

    void visitCheckOverRecursed(LCheckOverRecursed* lir);
    void visitCheckOverRecursedFailure(CheckOverRecursedFailure* ool);

    void visitUnboxFloatingPoint(LUnboxFloatingPoint* lir);
    void visitOutOfLineUnboxFloatingPoint(OutOfLineUnboxFloatingPoint* ool);
    void visitOutOfLineStoreElementHole(OutOfLineStoreElementHole* ool);

    void loadJSScriptForBlock(MBasicBlock* block, Register reg);
    void loadOutermostJSScript(Register reg);

    void visitOutOfLineICFallback(OutOfLineICFallback* ool);

    void visitGetPropertyCacheV(LGetPropertyCacheV* ins);
    void visitGetPropertyCacheT(LGetPropertyCacheT* ins);
    void visitGetPropSuperCacheV(LGetPropSuperCacheV* ins);
    void visitBindNameCache(LBindNameCache* ins);
    void visitCallSetProperty(LInstruction* ins);
    void visitSetPropertyCache(LSetPropertyCache* ins);
    void visitGetNameCache(LGetNameCache* ins);
    void visitHasOwnCache(LHasOwnCache* ins);

    void visitAssertRangeI(LAssertRangeI* ins);
    void visitAssertRangeD(LAssertRangeD* ins);
    void visitAssertRangeF(LAssertRangeF* ins);
    void visitAssertRangeV(LAssertRangeV* ins);

    void visitAssertResultV(LAssertResultV* ins);
    void visitAssertResultT(LAssertResultT* ins);

#ifdef DEBUG
    void emitAssertResultV(const ValueOperand output, const TemporaryTypeSet* typeset);
    void emitAssertObjectOrStringResult(Register input, MIRType type, const TemporaryTypeSet* typeset);
#endif

    void visitInterruptCheck(LInterruptCheck* lir);
    void visitOutOfLineInterruptCheckImplicit(OutOfLineInterruptCheckImplicit* ins);
    void visitWasmTrap(LWasmTrap* lir);
    void visitWasmLoadTls(LWasmLoadTls* ins);
    void visitWasmBoundsCheck(LWasmBoundsCheck* ins);
    void visitWasmAlignmentCheck(LWasmAlignmentCheck* ins);
    void visitRecompileCheck(LRecompileCheck* ins);
    void visitRotate(LRotate* ins);

    void visitRandom(LRandom* ins);
    void visitSignExtendInt32(LSignExtendInt32* ins);

#ifdef DEBUG
    void emitDebugForceBailing(LInstruction* lir);
#endif

    IonScriptCounts* extractScriptCounts() {
        IonScriptCounts* counts = scriptCounts_;
        scriptCounts_ = nullptr;  // prevent delete in dtor
        return counts;
    }

  private:
    void addGetPropertyCache(LInstruction* ins, LiveRegisterSet liveRegs,
                             TypedOrValueRegister value, const ConstantOrRegister& id,
                             TypedOrValueRegister output, Register maybeTemp,
                             GetPropertyResultFlags flags);
    void addSetPropertyCache(LInstruction* ins, LiveRegisterSet liveRegs, Register objReg,
                             Register temp, FloatRegister tempDouble,
                             FloatRegister tempF32, const ConstantOrRegister& id,
                             const ConstantOrRegister& value,
                             bool strict, bool needsPostBarrier, bool needsTypeBarrier,
                             bool guardHoles);

    MOZ_MUST_USE bool generateBranchV(const ValueOperand& value, Label* ifTrue, Label* ifFalse,
                                      FloatRegister fr);

    void emitLambdaInit(Register resultReg, Register envChainReg,
                        const LambdaFunctionInfo& info);

    void emitFilterArgumentsOrEval(LInstruction* lir, Register string, Register temp1,
                                   Register temp2);

    template <class IteratorObject, class OrderedHashTable>
    void emitGetNextEntryForIterator(LGetNextEntryForIterator* lir);

    template <class OrderedHashTable>
    void emitLoadIteratorValues(Register result, Register temp, Register front);

    IonScriptCounts* maybeCreateScriptCounts();

    // This function behaves like testValueTruthy with the exception that it can
    // choose to let control flow fall through when the object is truthy, as
    // an optimization. Use testValueTruthy when it's required to branch to one
    // of the two labels.
    void testValueTruthyKernel(const ValueOperand& value,
                               const LDefinition* scratch1, const LDefinition* scratch2,
                               FloatRegister fr,
                               Label* ifTruthy, Label* ifFalsy,
                               OutOfLineTestObject* ool,
                               MDefinition* valueMIR);

    // Test whether value is truthy or not and jump to the corresponding label.
    // If the value can be an object that emulates |undefined|, |ool| must be
    // non-null; otherwise it may be null (and the scratch definitions should
    // be bogus), in which case an object encountered here will always be
    // truthy.
    void testValueTruthy(const ValueOperand& value,
                         const LDefinition* scratch1, const LDefinition* scratch2,
                         FloatRegister fr,
                         Label* ifTruthy, Label* ifFalsy,
                         OutOfLineTestObject* ool,
                         MDefinition* valueMIR);

    // This function behaves like testObjectEmulatesUndefined with the exception
    // that it can choose to let control flow fall through when the object
    // doesn't emulate undefined, as an optimization. Use the regular
    // testObjectEmulatesUndefined when it's required to branch to one of the
    // two labels.
    void testObjectEmulatesUndefinedKernel(Register objreg,
                                           Label* ifEmulatesUndefined,
                                           Label* ifDoesntEmulateUndefined,
                                           Register scratch, OutOfLineTestObject* ool);

    // Test whether an object emulates |undefined|.  If it does, jump to
    // |ifEmulatesUndefined|; the caller is responsible for binding this label.
    // If it doesn't, fall through; the label |ifDoesntEmulateUndefined| (which
    // must be initially unbound) will be bound at this point.
    void branchTestObjectEmulatesUndefined(Register objreg,
                                           Label* ifEmulatesUndefined,
                                           Label* ifDoesntEmulateUndefined,
                                           Register scratch, OutOfLineTestObject* ool);

    // Test whether an object emulates |undefined|, and jump to the
    // corresponding label.
    //
    // This method should be used when subsequent code can't be laid out in a
    // straight line; if it can, branchTest* should be used instead.
    void testObjectEmulatesUndefined(Register objreg,
                                     Label* ifEmulatesUndefined,
                                     Label* ifDoesntEmulateUndefined,
                                     Register scratch, OutOfLineTestObject* ool);

    void emitStoreElementTyped(const LAllocation* value, MIRType valueType, MIRType elementType,
                               Register elements, const LAllocation* index,
                               int32_t offsetAdjustment);

    // Bailout if an element about to be written to is a hole.
    void emitStoreHoleCheck(Register elements, const LAllocation* index, int32_t offsetAdjustment,
                            LSnapshot* snapshot);

    void emitAssertRangeI(const Range* r, Register input);
    void emitAssertRangeD(const Range* r, FloatRegister input, FloatRegister temp);

    void maybeEmitGlobalBarrierCheck(const LAllocation* maybeGlobal, OutOfLineCode* ool);

    Vector<CodeOffset, 0, JitAllocPolicy> ionScriptLabels_;

    struct SharedStub {
        ICStub::Kind kind;
        IonICEntry entry;
        CodeOffset label;

        SharedStub(ICStub::Kind kind, IonICEntry entry, CodeOffset label)
          : kind(kind), entry(entry), label(label)
        {}
    };

    Vector<SharedStub, 0, SystemAllocPolicy> sharedStubs_;

    void branchIfInvalidated(Register temp, Label* invalidated);

#ifdef DEBUG
    void emitDebugResultChecks(LInstruction* ins);
    void emitObjectOrStringResultChecks(LInstruction* lir, MDefinition* mir);
    void emitValueResultChecks(LInstruction* lir, MDefinition* mir);
#endif

    // Script counts created during code generation.
    IonScriptCounts* scriptCounts_;

#if defined(JS_ION_PERF)
    PerfSpewer perfSpewer_;
#endif

    // This integer is a bit mask of all SimdTypeDescr::Type indexes.  When a
    // MSimdBox instruction is encoded, it might have either been created by
    // IonBuilder, or by the Eager Simd Unbox phase.
    //
    // As the template objects are weak references, the JitCompartment is using
    // Read Barriers, but such barrier cannot be used during the compilation. To
    // work around this issue, the barriers are captured during
    // CodeGenerator::link.
    //
    // Instead of saving the pointers, we just save the index of the Read
    // Barriered objects in a bit mask.
    uint32_t simdTemplatesToReadBarrier_;

    // Bit mask of JitCompartment stubs that are to be read-barriered.
    uint32_t compartmentStubsToReadBarrier_;

    void addSimdTemplateToReadBarrier(SimdType simdType);
};

} // namespace jit
} // namespace js

#endif /* jit_CodeGenerator_h */

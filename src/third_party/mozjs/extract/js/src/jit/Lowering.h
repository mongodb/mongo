/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_Lowering_h
#define jit_Lowering_h

// This file declares the structures that are used for attaching LIR to a
// MIRGraph.

#include "jit/LIR.h"
#if defined(JS_CODEGEN_X86)
# include "jit/x86/Lowering-x86.h"
#elif defined(JS_CODEGEN_X64)
# include "jit/x64/Lowering-x64.h"
#elif defined(JS_CODEGEN_ARM)
# include "jit/arm/Lowering-arm.h"
#elif defined(JS_CODEGEN_ARM64)
# include "jit/arm64/Lowering-arm64.h"
#elif defined(JS_CODEGEN_MIPS32)
# include "jit/mips32/Lowering-mips32.h"
#elif defined(JS_CODEGEN_MIPS64)
# include "jit/mips64/Lowering-mips64.h"
#elif defined(JS_CODEGEN_NONE)
# include "jit/none/Lowering-none.h"
#else
# error "Unknown architecture!"
#endif

namespace js {
namespace jit {

class LIRGenerator : public LIRGeneratorSpecific
{
    void updateResumeState(MInstruction* ins);
    void updateResumeState(MBasicBlock* block);

    // The maximum depth, for framesizeclass determination.
    uint32_t maxargslots_;

  public:
    LIRGenerator(MIRGenerator* gen, MIRGraph& graph, LIRGraph& lirGraph)
      : LIRGeneratorSpecific(gen, graph, lirGraph),
        maxargslots_(0)
    { }

    MOZ_MUST_USE bool generate();

  private:
    LBoxAllocation useBoxFixedAtStart(MDefinition* mir, Register reg1, Register reg2) {
        return useBoxFixed(mir, reg1, reg2, /* useAtStart = */ true);
    }

    LBoxAllocation useBoxFixedAtStart(MDefinition* mir, ValueOperand op);
    LBoxAllocation useBoxAtStart(MDefinition* mir, LUse::Policy policy = LUse::REGISTER);

    void lowerBitOp(JSOp op, MInstruction* ins);
    void lowerShiftOp(JSOp op, MShiftInstruction* ins);
    void lowerBinaryV(JSOp op, MBinaryInstruction* ins);
    void definePhis();

    MOZ_MUST_USE bool lowerCallArguments(MCall* call);

    template <typename LClass>
    LInstruction* lowerWasmCall(MWasmCall* ins, bool needsBoundsCheck);

  public:
    MOZ_MUST_USE bool visitInstruction(MInstruction* ins);
    MOZ_MUST_USE bool visitBlock(MBasicBlock* block);

    // Visitor hooks are explicit, to give CPU-specific versions a chance to
    // intercept without a bunch of explicit gunk in the .cpp.
    void visitCloneLiteral(MCloneLiteral* ins) override;
    void visitParameter(MParameter* param) override;
    void visitCallee(MCallee* callee) override;
    void visitIsConstructing(MIsConstructing* ins) override;
    void visitGoto(MGoto* ins) override;
    void visitTableSwitch(MTableSwitch* tableswitch) override;
    void visitNewArray(MNewArray* ins) override;
    void visitNewArrayCopyOnWrite(MNewArrayCopyOnWrite* ins) override;
    void visitNewArrayDynamicLength(MNewArrayDynamicLength* ins) override;
    void visitNewIterator(MNewIterator* ins) override;
    void visitNewTypedArray(MNewTypedArray* ins) override;
    void visitNewTypedArrayDynamicLength(MNewTypedArrayDynamicLength* ins) override;
    void visitNewObject(MNewObject* ins) override;
    void visitNewTypedObject(MNewTypedObject* ins) override;
    void visitNewNamedLambdaObject(MNewNamedLambdaObject* ins) override;
    void visitNewCallObject(MNewCallObject* ins) override;
    void visitNewSingletonCallObject(MNewSingletonCallObject* ins) override;
    void visitNewStringObject(MNewStringObject* ins) override;
    void visitNewDerivedTypedObject(MNewDerivedTypedObject* ins) override;
    void visitInitElem(MInitElem* ins) override;
    void visitInitElemGetterSetter(MInitElemGetterSetter* ins) override;
    void visitMutateProto(MMutateProto* ins) override;
    void visitInitPropGetterSetter(MInitPropGetterSetter* ins) override;
    void visitCheckOverRecursed(MCheckOverRecursed* ins) override;
    void visitDefVar(MDefVar* ins) override;
    void visitDefLexical(MDefLexical* ins) override;
    void visitDefFun(MDefFun* ins) override;
    void visitCreateThisWithTemplate(MCreateThisWithTemplate* ins) override;
    void visitCreateThisWithProto(MCreateThisWithProto* ins) override;
    void visitCreateThis(MCreateThis* ins) override;
    void visitCreateArgumentsObject(MCreateArgumentsObject* ins) override;
    void visitGetArgumentsObjectArg(MGetArgumentsObjectArg* ins) override;
    void visitSetArgumentsObjectArg(MSetArgumentsObjectArg* ins) override;
    void visitReturnFromCtor(MReturnFromCtor* ins) override;
    void visitComputeThis(MComputeThis* ins) override;
    void visitImplicitThis(MImplicitThis* ins) override;
    void visitCall(MCall* call) override;
    void visitApplyArgs(MApplyArgs* apply) override;
    void visitApplyArray(MApplyArray* apply) override;
    void visitBail(MBail* bail) override;
    void visitUnreachable(MUnreachable* unreachable) override;
    void visitEncodeSnapshot(MEncodeSnapshot* ins) override;
    void visitAssertFloat32(MAssertFloat32* ins) override;
    void visitAssertRecoveredOnBailout(MAssertRecoveredOnBailout* ins) override;
    void visitGetDynamicName(MGetDynamicName* ins) override;
    void visitCallDirectEval(MCallDirectEval* ins) override;
    void visitTest(MTest* test) override;
    void visitGotoWithFake(MGotoWithFake* ins) override;
    void visitFunctionDispatch(MFunctionDispatch* ins) override;
    void visitObjectGroupDispatch(MObjectGroupDispatch* ins) override;
    void visitCompare(MCompare* comp) override;
    void visitSameValue(MSameValue* comp) override;
    void visitTypeOf(MTypeOf* ins) override;
    void visitToAsync(MToAsync* ins) override;
    void visitToAsyncGen(MToAsyncGen* ins) override;
    void visitToAsyncIter(MToAsyncIter* ins) override;
    void visitToId(MToId* ins) override;
    void visitBitNot(MBitNot* ins) override;
    void visitBitAnd(MBitAnd* ins) override;
    void visitBitOr(MBitOr* ins) override;
    void visitBitXor(MBitXor* ins) override;
    void visitLsh(MLsh* ins) override;
    void visitRsh(MRsh* ins) override;
    void visitUrsh(MUrsh* ins) override;
    void visitSignExtendInt32(MSignExtendInt32* ins) override;
    void visitRotate(MRotate* ins) override;
    void visitFloor(MFloor* ins) override;
    void visitCeil(MCeil* ins) override;
    void visitRound(MRound* ins) override;
    void visitNearbyInt(MNearbyInt* ins) override;
    void visitMinMax(MMinMax* ins) override;
    void visitAbs(MAbs* ins) override;
    void visitClz(MClz* ins) override;
    void visitCtz(MCtz* ins) override;
    void visitSqrt(MSqrt* ins) override;
    void visitPopcnt(MPopcnt* ins) override;
    void visitAtan2(MAtan2* ins) override;
    void visitHypot(MHypot* ins) override;
    void visitPow(MPow* ins) override;
    void visitMathFunction(MMathFunction* ins) override;
    void visitAdd(MAdd* ins) override;
    void visitSub(MSub* ins) override;
    void visitMul(MMul* ins) override;
    void visitDiv(MDiv* ins) override;
    void visitMod(MMod* ins) override;
    void visitConcat(MConcat* ins) override;
    void visitCharCodeAt(MCharCodeAt* ins) override;
    void visitFromCharCode(MFromCharCode* ins) override;
    void visitFromCodePoint(MFromCodePoint* ins) override;
    void visitStringConvertCase(MStringConvertCase* ins) override;
    void visitSinCos(MSinCos *ins) override;
    void visitStringSplit(MStringSplit* ins) override;
    void visitStart(MStart* start) override;
    void visitOsrEntry(MOsrEntry* entry) override;
    void visitNop(MNop* nop) override;
    void visitLimitedTruncate(MLimitedTruncate* nop) override;
    void visitOsrValue(MOsrValue* value) override;
    void visitOsrEnvironmentChain(MOsrEnvironmentChain* object) override;
    void visitOsrReturnValue(MOsrReturnValue* value) override;
    void visitOsrArgumentsObject(MOsrArgumentsObject* object) override;
    void visitToDouble(MToDouble* convert) override;
    void visitToFloat32(MToFloat32* convert) override;
    void visitToNumberInt32(MToNumberInt32* convert) override;
    void visitTruncateToInt32(MTruncateToInt32* truncate) override;
    void visitWasmTruncateToInt32(MWasmTruncateToInt32* truncate) override;
    void visitWrapInt64ToInt32(MWrapInt64ToInt32* ins) override;
    void visitToString(MToString* convert) override;
    void visitToObject(MToObject* convert) override;
    void visitToObjectOrNull(MToObjectOrNull* convert) override;
    void visitRegExp(MRegExp* ins) override;
    void visitRegExpMatcher(MRegExpMatcher* ins) override;
    void visitRegExpSearcher(MRegExpSearcher* ins) override;
    void visitRegExpTester(MRegExpTester* ins) override;
    void visitRegExpPrototypeOptimizable(MRegExpPrototypeOptimizable* ins) override;
    void visitRegExpInstanceOptimizable(MRegExpInstanceOptimizable* ins) override;
    void visitGetFirstDollarIndex(MGetFirstDollarIndex* ins) override;
    void visitStringReplace(MStringReplace* ins) override;
    void visitBinarySharedStub(MBinarySharedStub* ins) override;
    void visitUnarySharedStub(MUnarySharedStub* ins) override;
    void visitNullarySharedStub(MNullarySharedStub* ins) override;
    void visitClassConstructor(MClassConstructor* ins) override;
    void visitLambda(MLambda* ins) override;
    void visitLambdaArrow(MLambdaArrow* ins) override;
    void visitSetFunName(MSetFunName* ins) override;
    void visitNewLexicalEnvironmentObject(MNewLexicalEnvironmentObject* ins) override;
    void visitCopyLexicalEnvironmentObject(MCopyLexicalEnvironmentObject* ins) override;
    void visitKeepAliveObject(MKeepAliveObject* ins) override;
    void visitSlots(MSlots* ins) override;
    void visitElements(MElements* ins) override;
    void visitConstantElements(MConstantElements* ins) override;
    void visitConvertElementsToDoubles(MConvertElementsToDoubles* ins) override;
    void visitMaybeToDoubleElement(MMaybeToDoubleElement* ins) override;
    void visitMaybeCopyElementsForWrite(MMaybeCopyElementsForWrite* ins) override;
    void visitLoadSlot(MLoadSlot* ins) override;
    void visitLoadFixedSlotAndUnbox(MLoadFixedSlotAndUnbox* ins) override;
    void visitFunctionEnvironment(MFunctionEnvironment* ins) override;
    void visitHomeObject(MHomeObject* ins) override;
    void visitHomeObjectSuperBase(MHomeObjectSuperBase* ins) override;
    void visitInterruptCheck(MInterruptCheck* ins) override;
    void visitWasmTrap(MWasmTrap* ins) override;
    void visitWasmReinterpret(MWasmReinterpret* ins) override;
    void visitStoreSlot(MStoreSlot* ins) override;
    void visitFilterTypeSet(MFilterTypeSet* ins) override;
    void visitTypeBarrier(MTypeBarrier* ins) override;
    void visitPostWriteBarrier(MPostWriteBarrier* ins) override;
    void visitPostWriteElementBarrier(MPostWriteElementBarrier* ins) override;
    void visitArrayLength(MArrayLength* ins) override;
    void visitSetArrayLength(MSetArrayLength* ins) override;
    void visitGetNextEntryForIterator(MGetNextEntryForIterator* ins) override;
    void visitTypedArrayLength(MTypedArrayLength* ins) override;
    void visitTypedArrayElements(MTypedArrayElements* ins) override;
    void visitSetDisjointTypedElements(MSetDisjointTypedElements* ins) override;
    void visitTypedObjectElements(MTypedObjectElements* ins) override;
    void visitSetTypedObjectOffset(MSetTypedObjectOffset* ins) override;
    void visitTypedObjectDescr(MTypedObjectDescr* ins) override;
    void visitInitializedLength(MInitializedLength* ins) override;
    void visitSetInitializedLength(MSetInitializedLength* ins) override;
    void visitNot(MNot* ins) override;
    void visitBoundsCheck(MBoundsCheck* ins) override;
    void visitBoundsCheckLower(MBoundsCheckLower* ins) override;
    void visitSpectreMaskIndex(MSpectreMaskIndex* ins) override;
    void visitLoadElement(MLoadElement* ins) override;
    void visitLoadElementHole(MLoadElementHole* ins) override;
    void visitLoadUnboxedObjectOrNull(MLoadUnboxedObjectOrNull* ins) override;
    void visitLoadUnboxedString(MLoadUnboxedString* ins) override;
    void visitLoadElementFromState(MLoadElementFromState* ins) override;
    void visitStoreElement(MStoreElement* ins) override;
    void visitStoreElementHole(MStoreElementHole* ins) override;
    void visitFallibleStoreElement(MFallibleStoreElement* ins) override;
    void visitStoreUnboxedObjectOrNull(MStoreUnboxedObjectOrNull* ins) override;
    void visitStoreUnboxedString(MStoreUnboxedString* ins) override;
    void visitConvertUnboxedObjectToNative(MConvertUnboxedObjectToNative* ins) override;
    void visitEffectiveAddress(MEffectiveAddress* ins) override;
    void visitArrayPopShift(MArrayPopShift* ins) override;
    void visitArrayPush(MArrayPush* ins) override;
    void visitArraySlice(MArraySlice* ins) override;
    void visitArrayJoin(MArrayJoin* ins) override;
    void visitLoadUnboxedScalar(MLoadUnboxedScalar* ins) override;
    void visitLoadTypedArrayElementHole(MLoadTypedArrayElementHole* ins) override;
    void visitStoreUnboxedScalar(MStoreUnboxedScalar* ins) override;
    void visitStoreTypedArrayElementHole(MStoreTypedArrayElementHole* ins) override;
    void visitClampToUint8(MClampToUint8* ins) override;
    void visitLoadFixedSlot(MLoadFixedSlot* ins) override;
    void visitStoreFixedSlot(MStoreFixedSlot* ins) override;
    void visitGetPropSuperCache(MGetPropSuperCache* ins) override;
    void visitGetPropertyCache(MGetPropertyCache* ins) override;
    void visitGetPropertyPolymorphic(MGetPropertyPolymorphic* ins) override;
    void visitSetPropertyPolymorphic(MSetPropertyPolymorphic* ins) override;
    void visitBindNameCache(MBindNameCache* ins) override;
    void visitCallBindVar(MCallBindVar* ins) override;
    void visitGuardObjectIdentity(MGuardObjectIdentity* ins) override;
    void visitGuardShape(MGuardShape* ins) override;
    void visitGuardObjectGroup(MGuardObjectGroup* ins) override;
    void visitGuardObject(MGuardObject* ins) override;
    void visitGuardString(MGuardString* ins) override;
    void visitGuardReceiverPolymorphic(MGuardReceiverPolymorphic* ins) override;
    void visitGuardUnboxedExpando(MGuardUnboxedExpando* ins) override;
    void visitLoadUnboxedExpando(MLoadUnboxedExpando* ins) override;
    void visitPolyInlineGuard(MPolyInlineGuard* ins) override;
    void visitAssertRange(MAssertRange* ins) override;
    void visitCallGetProperty(MCallGetProperty* ins) override;
    void visitDeleteProperty(MDeleteProperty* ins) override;
    void visitDeleteElement(MDeleteElement* ins) override;
    void visitGetNameCache(MGetNameCache* ins) override;
    void visitCallGetIntrinsicValue(MCallGetIntrinsicValue* ins) override;
    void visitCallGetElement(MCallGetElement* ins) override;
    void visitCallSetElement(MCallSetElement* ins) override;
    void visitCallInitElementArray(MCallInitElementArray* ins) override;
    void visitSetPropertyCache(MSetPropertyCache* ins) override;
    void visitCallSetProperty(MCallSetProperty* ins) override;
    void visitGetIteratorCache(MGetIteratorCache* ins) override;
    void visitIteratorMore(MIteratorMore* ins) override;
    void visitIsNoIter(MIsNoIter* ins) override;
    void visitIteratorEnd(MIteratorEnd* ins) override;
    void visitStringLength(MStringLength* ins) override;
    void visitArgumentsLength(MArgumentsLength* ins) override;
    void visitGetFrameArgument(MGetFrameArgument* ins) override;
    void visitSetFrameArgument(MSetFrameArgument* ins) override;
    void visitRunOncePrologue(MRunOncePrologue* ins) override;
    void visitRest(MRest* ins) override;
    void visitThrow(MThrow* ins) override;
    void visitInCache(MInCache* ins) override;
    void visitInArray(MInArray* ins) override;
    void visitHasOwnCache(MHasOwnCache* ins) override;
    void visitInstanceOf(MInstanceOf* ins) override;
    void visitInstanceOfCache(MInstanceOfCache* ins) override;
    void visitIsCallable(MIsCallable* ins) override;
    void visitIsConstructor(MIsConstructor* ins) override;
    void visitIsArray(MIsArray* ins) override;
    void visitIsTypedArray(MIsTypedArray* ins) override;
    void visitIsObject(MIsObject* ins) override;
    void visitHasClass(MHasClass* ins) override;
    void visitGuardToClass(MGuardToClass* ins) override;
    void visitObjectClassToString(MObjectClassToString* ins) override;
    void visitWasmAddOffset(MWasmAddOffset* ins) override;
    void visitWasmLoadTls(MWasmLoadTls* ins) override;
    void visitWasmBoundsCheck(MWasmBoundsCheck* ins) override;
    void visitWasmAlignmentCheck(MWasmAlignmentCheck* ins) override;
    void visitWasmLoadGlobalVar(MWasmLoadGlobalVar* ins) override;
    void visitWasmStoreGlobalVar(MWasmStoreGlobalVar* ins) override;
    void visitWasmParameter(MWasmParameter* ins) override;
    void visitWasmReturn(MWasmReturn* ins) override;
    void visitWasmReturnVoid(MWasmReturnVoid* ins) override;
    void visitWasmStackArg(MWasmStackArg* ins) override;
    void visitWasmCall(MWasmCall* ins) override;
    void visitSetDOMProperty(MSetDOMProperty* ins) override;
    void visitGetDOMProperty(MGetDOMProperty* ins) override;
    void visitGetDOMMember(MGetDOMMember* ins) override;
    void visitRecompileCheck(MRecompileCheck* ins) override;
    void visitSimdBox(MSimdBox* ins) override;
    void visitSimdUnbox(MSimdUnbox* ins) override;
    void visitSimdUnaryArith(MSimdUnaryArith* ins) override;
    void visitSimdBinaryComp(MSimdBinaryComp* ins) override;
    void visitSimdBinaryBitwise(MSimdBinaryBitwise* ins) override;
    void visitSimdShift(MSimdShift* ins) override;
    void visitSimdConstant(MSimdConstant* ins) override;
    void visitSimdConvert(MSimdConvert* ins) override;
    void visitSimdReinterpretCast(MSimdReinterpretCast* ins) override;
    void visitSimdAllTrue(MSimdAllTrue* ins) override;
    void visitSimdAnyTrue(MSimdAnyTrue* ins) override;
    void visitPhi(MPhi* ins) override;
    void visitBeta(MBeta* ins) override;
    void visitObjectState(MObjectState* ins) override;
    void visitArrayState(MArrayState* ins) override;
    void visitArgumentState(MArgumentState* ins) override;
    void visitUnknownValue(MUnknownValue* ins) override;
    void visitLexicalCheck(MLexicalCheck* ins) override;
    void visitThrowRuntimeLexicalError(MThrowRuntimeLexicalError* ins) override;
    void visitGlobalNameConflictsCheck(MGlobalNameConflictsCheck* ins) override;
    void visitDebugger(MDebugger* ins) override;
    void visitNewTarget(MNewTarget* ins) override;
    void visitArrowNewTarget(MArrowNewTarget* ins) override;
    void visitNaNToZero(MNaNToZero *ins) override;
    void visitAtomicIsLockFree(MAtomicIsLockFree* ins) override;
    void visitGuardSharedTypedArray(MGuardSharedTypedArray* ins) override;
    void visitCheckReturn(MCheckReturn* ins) override;
    void visitCheckIsObj(MCheckIsObj* ins) override;
    void visitCheckIsCallable(MCheckIsCallable* ins) override;
    void visitCheckObjCoercible(MCheckObjCoercible* ins) override;
    void visitDebugCheckSelfHosted(MDebugCheckSelfHosted* ins) override;
    void visitFinishBoundFunctionInit(MFinishBoundFunctionInit* ins) override;
    void visitIsPackedArray(MIsPackedArray* ins) override;
    void visitGetPrototypeOf(MGetPrototypeOf* ins) override;
};

} // namespace jit
} // namespace js

#endif /* jit_Lowering_h */

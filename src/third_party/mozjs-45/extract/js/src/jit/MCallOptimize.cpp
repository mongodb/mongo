/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jsmath.h"

#include "builtin/AtomicsObject.h"
#include "builtin/SIMD.h"
#include "builtin/TestingFunctions.h"
#include "builtin/TypedObject.h"
#include "jit/BaselineInspector.h"
#include "jit/InlinableNatives.h"
#include "jit/IonBuilder.h"
#include "jit/Lowering.h"
#include "jit/MIR.h"
#include "jit/MIRGraph.h"
#include "vm/ArgumentsObject.h"

#include "jsscriptinlines.h"

#include "jit/shared/Lowering-shared-inl.h"
#include "vm/NativeObject-inl.h"
#include "vm/StringObject-inl.h"
#include "vm/UnboxedObject-inl.h"

using mozilla::ArrayLength;

using JS::DoubleNaNValue;
using JS::TrackedOutcome;
using JS::TrackedStrategy;
using JS::TrackedTypeSite;

namespace js {
namespace jit {

IonBuilder::InliningStatus
IonBuilder::inlineNativeCall(CallInfo& callInfo, JSFunction* target)
{
    MOZ_ASSERT(target->isNative());

    if (!optimizationInfo().inlineNative()) {
        trackOptimizationOutcome(TrackedOutcome::CantInlineDisabledIon);
        return InliningStatus_NotInlined;
    }

    if (!target->jitInfo() || target->jitInfo()->type() != JSJitInfo::InlinableNative) {
        // Reaching here means we tried to inline a native for which there is no
        // Ion specialization.
        trackOptimizationOutcome(TrackedOutcome::CantInlineNativeNoSpecialization);
        return InliningStatus_NotInlined;
    }

    // Default failure reason is observing an unsupported type.
    trackOptimizationOutcome(TrackedOutcome::CantInlineNativeBadType);

    if (shouldAbortOnPreliminaryGroups(callInfo.thisArg()))
        return InliningStatus_NotInlined;
    for (size_t i = 0; i < callInfo.argc(); i++) {
        if (shouldAbortOnPreliminaryGroups(callInfo.getArg(i)))
            return InliningStatus_NotInlined;
    }

    switch (InlinableNative inlNative = target->jitInfo()->inlinableNative) {
      // Array natives.
      case InlinableNative::Array:
        return inlineArray(callInfo);
      case InlinableNative::ArrayIsArray:
        return inlineArrayIsArray(callInfo);
      case InlinableNative::ArrayPop:
        return inlineArrayPopShift(callInfo, MArrayPopShift::Pop);
      case InlinableNative::ArrayShift:
        return inlineArrayPopShift(callInfo, MArrayPopShift::Shift);
      case InlinableNative::ArrayPush:
        return inlineArrayPush(callInfo);
      case InlinableNative::ArrayConcat:
        return inlineArrayConcat(callInfo);
      case InlinableNative::ArraySlice:
        return inlineArraySlice(callInfo);
      case InlinableNative::ArraySplice:
        return inlineArraySplice(callInfo);

      // Atomic natives.
      case InlinableNative::AtomicsCompareExchange:
        return inlineAtomicsCompareExchange(callInfo);
      case InlinableNative::AtomicsExchange:
        return inlineAtomicsExchange(callInfo);
      case InlinableNative::AtomicsLoad:
        return inlineAtomicsLoad(callInfo);
      case InlinableNative::AtomicsStore:
        return inlineAtomicsStore(callInfo);
      case InlinableNative::AtomicsFence:
        return inlineAtomicsFence(callInfo);
      case InlinableNative::AtomicsAdd:
      case InlinableNative::AtomicsSub:
      case InlinableNative::AtomicsAnd:
      case InlinableNative::AtomicsOr:
      case InlinableNative::AtomicsXor:
        return inlineAtomicsBinop(callInfo, inlNative);
      case InlinableNative::AtomicsIsLockFree:
        return inlineAtomicsIsLockFree(callInfo);

      // Math natives.
      case InlinableNative::MathAbs:
        return inlineMathAbs(callInfo);
      case InlinableNative::MathFloor:
        return inlineMathFloor(callInfo);
      case InlinableNative::MathCeil:
        return inlineMathCeil(callInfo);
      case InlinableNative::MathRound:
        return inlineMathRound(callInfo);
      case InlinableNative::MathClz32:
        return inlineMathClz32(callInfo);
      case InlinableNative::MathSqrt:
        return inlineMathSqrt(callInfo);
      case InlinableNative::MathATan2:
        return inlineMathAtan2(callInfo);
      case InlinableNative::MathHypot:
        return inlineMathHypot(callInfo);
      case InlinableNative::MathMax:
        return inlineMathMinMax(callInfo, true /* max */);
      case InlinableNative::MathMin:
        return inlineMathMinMax(callInfo, false /* max */);
      case InlinableNative::MathPow:
        return inlineMathPow(callInfo);
      case InlinableNative::MathRandom:
        return inlineMathRandom(callInfo);
      case InlinableNative::MathImul:
        return inlineMathImul(callInfo);
      case InlinableNative::MathFRound:
        return inlineMathFRound(callInfo);
      case InlinableNative::MathSin:
        return inlineMathFunction(callInfo, MMathFunction::Sin);
      case InlinableNative::MathTan:
        return inlineMathFunction(callInfo, MMathFunction::Tan);
      case InlinableNative::MathCos:
        return inlineMathFunction(callInfo, MMathFunction::Cos);
      case InlinableNative::MathExp:
        return inlineMathFunction(callInfo, MMathFunction::Exp);
      case InlinableNative::MathLog:
        return inlineMathFunction(callInfo, MMathFunction::Log);
      case InlinableNative::MathASin:
        return inlineMathFunction(callInfo, MMathFunction::ASin);
      case InlinableNative::MathATan:
        return inlineMathFunction(callInfo, MMathFunction::ATan);
      case InlinableNative::MathACos:
        return inlineMathFunction(callInfo, MMathFunction::ACos);
      case InlinableNative::MathLog10:
        return inlineMathFunction(callInfo, MMathFunction::Log10);
      case InlinableNative::MathLog2:
        return inlineMathFunction(callInfo, MMathFunction::Log2);
      case InlinableNative::MathLog1P:
        return inlineMathFunction(callInfo, MMathFunction::Log1P);
      case InlinableNative::MathExpM1:
        return inlineMathFunction(callInfo, MMathFunction::ExpM1);
      case InlinableNative::MathCosH:
        return inlineMathFunction(callInfo, MMathFunction::CosH);
      case InlinableNative::MathSinH:
        return inlineMathFunction(callInfo, MMathFunction::SinH);
      case InlinableNative::MathTanH:
        return inlineMathFunction(callInfo, MMathFunction::TanH);
      case InlinableNative::MathACosH:
        return inlineMathFunction(callInfo, MMathFunction::ACosH);
      case InlinableNative::MathASinH:
        return inlineMathFunction(callInfo, MMathFunction::ASinH);
      case InlinableNative::MathATanH:
        return inlineMathFunction(callInfo, MMathFunction::ATanH);
      case InlinableNative::MathSign:
        return inlineMathFunction(callInfo, MMathFunction::Sign);
      case InlinableNative::MathTrunc:
        return inlineMathFunction(callInfo, MMathFunction::Trunc);
      case InlinableNative::MathCbrt:
        return inlineMathFunction(callInfo, MMathFunction::Cbrt);

      // RegExp natives.
      case InlinableNative::RegExpExec:
        return CallResultEscapes(pc) ? inlineRegExpExec(callInfo) : inlineRegExpTest(callInfo);
      case InlinableNative::RegExpTest:
        return inlineRegExpTest(callInfo);

      // String natives.
      case InlinableNative::String:
        return inlineStringObject(callInfo);
      case InlinableNative::StringSplit:
        return inlineStringSplit(callInfo);
      case InlinableNative::StringCharCodeAt:
        return inlineStrCharCodeAt(callInfo);
      case InlinableNative::StringFromCharCode:
        return inlineStrFromCharCode(callInfo);
      case InlinableNative::StringCharAt:
        return inlineStrCharAt(callInfo);
      case InlinableNative::StringReplace:
        return inlineStrReplace(callInfo);

      // Object natives.
      case InlinableNative::ObjectCreate:
        return inlineObjectCreate(callInfo);

      // Bound function.
      case InlinableNative::CallBoundFunction:
        return inlineBoundFunction(callInfo, target);

      // SIMD natives.
      case InlinableNative::SimdInt32x4:
        return inlineSimdInt32x4(callInfo, target->native());
      case InlinableNative::SimdFloat32x4:
        return inlineSimdFloat32x4(callInfo, target->native());

      // Testing functions.
      case InlinableNative::TestBailout:
        return inlineBailout(callInfo);
      case InlinableNative::TestAssertFloat32:
        return inlineAssertFloat32(callInfo);
      case InlinableNative::TestAssertRecoveredOnBailout:
        return inlineAssertRecoveredOnBailout(callInfo);

      // Slot intrinsics.
      case InlinableNative::IntrinsicUnsafeSetReservedSlot:
        return inlineUnsafeSetReservedSlot(callInfo);
      case InlinableNative::IntrinsicUnsafeGetReservedSlot:
        return inlineUnsafeGetReservedSlot(callInfo, MIRType_Value);
      case InlinableNative::IntrinsicUnsafeGetObjectFromReservedSlot:
        return inlineUnsafeGetReservedSlot(callInfo, MIRType_Object);
      case InlinableNative::IntrinsicUnsafeGetInt32FromReservedSlot:
        return inlineUnsafeGetReservedSlot(callInfo, MIRType_Int32);
      case InlinableNative::IntrinsicUnsafeGetStringFromReservedSlot:
        return inlineUnsafeGetReservedSlot(callInfo, MIRType_String);
      case InlinableNative::IntrinsicUnsafeGetBooleanFromReservedSlot:
        return inlineUnsafeGetReservedSlot(callInfo, MIRType_Boolean);

      // Utility intrinsics.
      case InlinableNative::IntrinsicIsCallable:
        return inlineIsCallable(callInfo);
      case InlinableNative::IntrinsicToObject:
        return inlineToObject(callInfo);
      case InlinableNative::IntrinsicIsObject:
        return inlineIsObject(callInfo);
      case InlinableNative::IntrinsicToInteger:
        return inlineToInteger(callInfo);
      case InlinableNative::IntrinsicToString:
        return inlineToString(callInfo);
      case InlinableNative::IntrinsicIsConstructing:
        return inlineIsConstructing(callInfo);
      case InlinableNative::IntrinsicSubstringKernel:
        return inlineSubstringKernel(callInfo);
      case InlinableNative::IntrinsicIsArrayIterator:
        return inlineHasClass(callInfo, &ArrayIteratorObject::class_);
      case InlinableNative::IntrinsicIsMapIterator:
        return inlineHasClass(callInfo, &MapIteratorObject::class_);
      case InlinableNative::IntrinsicIsStringIterator:
        return inlineHasClass(callInfo, &StringIteratorObject::class_);
      case InlinableNative::IntrinsicIsListIterator:
        return inlineHasClass(callInfo, &ListIteratorObject::class_);
      case InlinableNative::IntrinsicDefineDataProperty:
        return inlineDefineDataProperty(callInfo);

      // TypedArray intrinsics.
      case InlinableNative::IntrinsicIsTypedArray:
        return inlineIsTypedArray(callInfo);
      case InlinableNative::IntrinsicIsPossiblyWrappedTypedArray:
        return inlineIsPossiblyWrappedTypedArray(callInfo);
      case InlinableNative::IntrinsicTypedArrayLength:
        return inlineTypedArrayLength(callInfo);
      case InlinableNative::IntrinsicSetDisjointTypedElements:
        return inlineSetDisjointTypedElements(callInfo);

      // TypedObject intrinsics.
      case InlinableNative::IntrinsicObjectIsTypedObject:
        return inlineHasClass(callInfo,
                              &OutlineTransparentTypedObject::class_,
                              &OutlineOpaqueTypedObject::class_,
                              &InlineTransparentTypedObject::class_,
                              &InlineOpaqueTypedObject::class_);
      case InlinableNative::IntrinsicObjectIsTransparentTypedObject:
        return inlineHasClass(callInfo,
                              &OutlineTransparentTypedObject::class_,
                              &InlineTransparentTypedObject::class_);
      case InlinableNative::IntrinsicObjectIsOpaqueTypedObject:
        return inlineHasClass(callInfo,
                              &OutlineOpaqueTypedObject::class_,
                              &InlineOpaqueTypedObject::class_);
      case InlinableNative::IntrinsicObjectIsTypeDescr:
        return inlineObjectIsTypeDescr(callInfo);
      case InlinableNative::IntrinsicTypeDescrIsSimpleType:
        return inlineHasClass(callInfo,
                              &ScalarTypeDescr::class_, &ReferenceTypeDescr::class_);
      case InlinableNative::IntrinsicTypeDescrIsArrayType:
        return inlineHasClass(callInfo, &ArrayTypeDescr::class_);
      case InlinableNative::IntrinsicSetTypedObjectOffset:
        return inlineSetTypedObjectOffset(callInfo);
    }

    MOZ_CRASH("Shouldn't get here");
}

IonBuilder::InliningStatus
IonBuilder::inlineNativeGetter(CallInfo& callInfo, JSFunction* target)
{
    MOZ_ASSERT(target->isNative());
    JSNative native = target->native();

    if (!optimizationInfo().inlineNative())
        return InliningStatus_NotInlined;

    TemporaryTypeSet* thisTypes = callInfo.thisArg()->resultTypeSet();
    MOZ_ASSERT(callInfo.argc() == 0);

    // Try to optimize typed array lengths.
    if (thisTypes) {
        Scalar::Type type;

        type = thisTypes->getTypedArrayType(constraints());
        if (type != Scalar::MaxTypedArrayViewType &&
            TypedArrayObject::isOriginalLengthGetter(native))
        {
            MInstruction* length = addTypedArrayLength(callInfo.thisArg());
            current->push(length);
            return InliningStatus_Inlined;
        }
    }

    return InliningStatus_NotInlined;
}

IonBuilder::InliningStatus
IonBuilder::inlineNonFunctionCall(CallInfo& callInfo, JSObject* target)
{
    // Inline a call to a non-function object, invoking the object's call or
    // construct hook.

    if (callInfo.constructing() && target->constructHook() == TypedObject::construct)
        return inlineConstructTypedObject(callInfo, &target->as<TypeDescr>());

    if (!callInfo.constructing() && target->callHook() == SimdTypeDescr::call)
        return inlineConstructSimdObject(callInfo, &target->as<SimdTypeDescr>());

    return InliningStatus_NotInlined;
}

TemporaryTypeSet*
IonBuilder::getInlineReturnTypeSet()
{
    return bytecodeTypes(pc);
}

MIRType
IonBuilder::getInlineReturnType()
{
    TemporaryTypeSet* returnTypes = getInlineReturnTypeSet();
    return returnTypes->getKnownMIRType();
}

IonBuilder::InliningStatus
IonBuilder::inlineMathFunction(CallInfo& callInfo, MMathFunction::Function function)
{
    if (callInfo.constructing())
        return InliningStatus_NotInlined;

    if (callInfo.argc() != 1)
        return InliningStatus_NotInlined;

    if (getInlineReturnType() != MIRType_Double)
        return InliningStatus_NotInlined;
    if (!IsNumberType(callInfo.getArg(0)->type()))
        return InliningStatus_NotInlined;

    const MathCache* cache = compartment->runtime()->maybeGetMathCache();

    callInfo.fun()->setImplicitlyUsedUnchecked();
    callInfo.thisArg()->setImplicitlyUsedUnchecked();

    MMathFunction* ins = MMathFunction::New(alloc(), callInfo.getArg(0), function, cache);
    current->add(ins);
    current->push(ins);
    return InliningStatus_Inlined;
}

IonBuilder::InliningStatus
IonBuilder::inlineArray(CallInfo& callInfo)
{
    uint32_t initLength = 0;

    JSObject* templateObject = inspector->getTemplateObjectForNative(pc, ArrayConstructor);
    if (!templateObject) {
        trackOptimizationOutcome(TrackedOutcome::CantInlineNativeNoTemplateObj);
        return InliningStatus_NotInlined;
    }

    if (templateObject->is<UnboxedArrayObject>()) {
        if (templateObject->group()->unboxedLayout().nativeGroup())
            return InliningStatus_NotInlined;
    }

    // Multiple arguments imply array initialization, not just construction.
    if (callInfo.argc() >= 2) {
        initLength = callInfo.argc();

        TypeSet::ObjectKey* key = TypeSet::ObjectKey::get(templateObject);
        if (!key->unknownProperties()) {
            HeapTypeSetKey elemTypes = key->property(JSID_VOID);

            for (uint32_t i = 0; i < initLength; i++) {
                MDefinition* value = callInfo.getArg(i);
                if (!TypeSetIncludes(elemTypes.maybeTypes(), value->type(), value->resultTypeSet())) {
                    elemTypes.freeze(constraints());
                    return InliningStatus_NotInlined;
                }
            }
        }
    }

    // A single integer argument denotes initial length.
    if (callInfo.argc() == 1) {
        if (callInfo.getArg(0)->type() != MIRType_Int32)
            return InliningStatus_NotInlined;

        MDefinition* arg = callInfo.getArg(0);
        if (!arg->isConstantValue()) {
            callInfo.setImplicitlyUsedUnchecked();
            MNewArrayDynamicLength* ins =
                MNewArrayDynamicLength::New(alloc(), constraints(), templateObject,
                                            templateObject->group()->initialHeap(constraints()),
                                            arg);
            current->add(ins);
            current->push(ins);
            return InliningStatus_Inlined;
        }

        // The next several checks all may fail due to range conditions.
        trackOptimizationOutcome(TrackedOutcome::ArrayRange);

        // Negative lengths generate a RangeError, unhandled by the inline path.
        initLength = arg->constantValue().toInt32();
        if (initLength > NativeObject::MAX_DENSE_ELEMENTS_COUNT)
            return InliningStatus_NotInlined;
        MOZ_ASSERT(initLength <= INT32_MAX);

        // Make sure initLength matches the template object's length. This is
        // not guaranteed to be the case, for instance if we're inlining the
        // MConstant may come from an outer script.
        if (initLength != GetAnyBoxedOrUnboxedArrayLength(templateObject))
            return InliningStatus_NotInlined;

        // Don't inline large allocations.
        if (initLength > ArrayObject::EagerAllocationMaxLength)
            return InliningStatus_NotInlined;
    }

    callInfo.setImplicitlyUsedUnchecked();

    MConstant* templateConst = MConstant::NewConstraintlessObject(alloc(), templateObject);
    current->add(templateConst);

    MNewArray* ins = MNewArray::New(alloc(), constraints(), initLength, templateConst,
                                    templateObject->group()->initialHeap(constraints()), pc);
    current->add(ins);
    current->push(ins);

    if (callInfo.argc() >= 2) {
        JSValueType unboxedType = GetBoxedOrUnboxedType(templateObject);
        for (uint32_t i = 0; i < initLength; i++) {
            MDefinition* value = callInfo.getArg(i);
            if (!initializeArrayElement(ins, i, value, unboxedType, /* addResumePoint = */ false))
                return InliningStatus_Error;
        }

        MInstruction* setLength = setInitializedLength(ins, unboxedType, initLength);
        if (!resumeAfter(setLength))
            return InliningStatus_Error;
    }

    return InliningStatus_Inlined;
}

IonBuilder::InliningStatus
IonBuilder::inlineArrayIsArray(CallInfo& callInfo)
{
    if (callInfo.constructing() || callInfo.argc() != 1) {
        trackOptimizationOutcome(TrackedOutcome::CantInlineNativeBadForm);
        return InliningStatus_NotInlined;
    }

    if (getInlineReturnType() != MIRType_Boolean)
        return InliningStatus_NotInlined;

    MDefinition* arg = callInfo.getArg(0);

    bool isArray;
    if (!arg->mightBeType(MIRType_Object)) {
        isArray = false;
    } else {
        if (arg->type() != MIRType_Object)
            return InliningStatus_NotInlined;

        TemporaryTypeSet* types = arg->resultTypeSet();
        const Class* clasp = types ? types->getKnownClass(constraints()) : nullptr;
        if (!clasp || clasp->isProxy())
            return InliningStatus_NotInlined;

        isArray = (clasp == &ArrayObject::class_ || clasp == &UnboxedArrayObject::class_);
    }

    pushConstant(BooleanValue(isArray));

    callInfo.setImplicitlyUsedUnchecked();
    return InliningStatus_Inlined;
}

IonBuilder::InliningStatus
IonBuilder::inlineArrayPopShift(CallInfo& callInfo, MArrayPopShift::Mode mode)
{
    if (callInfo.constructing()) {
        trackOptimizationOutcome(TrackedOutcome::CantInlineNativeBadForm);
        return InliningStatus_NotInlined;
    }

    MIRType returnType = getInlineReturnType();
    if (returnType == MIRType_Undefined || returnType == MIRType_Null)
        return InliningStatus_NotInlined;
    if (callInfo.thisArg()->type() != MIRType_Object)
        return InliningStatus_NotInlined;

    // Pop and shift are only handled for dense arrays that have never been
    // used in an iterator: popping elements does not account for suppressing
    // deleted properties in active iterators.
    ObjectGroupFlags unhandledFlags =
        OBJECT_FLAG_SPARSE_INDEXES |
        OBJECT_FLAG_LENGTH_OVERFLOW |
        OBJECT_FLAG_ITERATED;

    MDefinition* obj = convertUnboxedObjects(callInfo.thisArg());
    TemporaryTypeSet* thisTypes = obj->resultTypeSet();
    if (!thisTypes)
        return InliningStatus_NotInlined;
    const Class* clasp = thisTypes->getKnownClass(constraints());
    if (clasp != &ArrayObject::class_ && clasp != &UnboxedArrayObject::class_)
        return InliningStatus_NotInlined;
    if (thisTypes->hasObjectFlags(constraints(), unhandledFlags)) {
        trackOptimizationOutcome(TrackedOutcome::ArrayBadFlags);
        return InliningStatus_NotInlined;
    }

    if (ArrayPrototypeHasIndexedProperty(this, script())) {
        trackOptimizationOutcome(TrackedOutcome::ProtoIndexedProps);
        return InliningStatus_NotInlined;
    }

    JSValueType unboxedType = JSVAL_TYPE_MAGIC;
    if (clasp == &UnboxedArrayObject::class_) {
        unboxedType = UnboxedArrayElementType(constraints(), obj, nullptr);
        if (unboxedType == JSVAL_TYPE_MAGIC)
            return InliningStatus_NotInlined;
    }

    callInfo.setImplicitlyUsedUnchecked();

    if (clasp == &ArrayObject::class_)
        obj = addMaybeCopyElementsForWrite(obj, /* checkNative = */ false);

    TemporaryTypeSet* returnTypes = getInlineReturnTypeSet();
    bool needsHoleCheck = thisTypes->hasObjectFlags(constraints(), OBJECT_FLAG_NON_PACKED);
    bool maybeUndefined = returnTypes->hasType(TypeSet::UndefinedType());

    BarrierKind barrier = PropertyReadNeedsTypeBarrier(analysisContext, constraints(),
                                                       obj, nullptr, returnTypes);
    if (barrier != BarrierKind::NoBarrier)
        returnType = MIRType_Value;

    MArrayPopShift* ins = MArrayPopShift::New(alloc(), obj, mode,
                                              unboxedType, needsHoleCheck, maybeUndefined);
    current->add(ins);
    current->push(ins);
    ins->setResultType(returnType);

    if (!resumeAfter(ins))
        return InliningStatus_Error;

    if (!pushTypeBarrier(ins, returnTypes, barrier))
        return InliningStatus_Error;

    return InliningStatus_Inlined;
}

IonBuilder::InliningStatus
IonBuilder::inlineArraySplice(CallInfo& callInfo)
{
    if (callInfo.argc() != 2 || callInfo.constructing()) {
        trackOptimizationOutcome(TrackedOutcome::CantInlineNativeBadForm);
        return InliningStatus_NotInlined;
    }

    // Ensure |this|, argument and result are objects.
    if (getInlineReturnType() != MIRType_Object)
        return InliningStatus_NotInlined;
    if (callInfo.thisArg()->type() != MIRType_Object)
        return InliningStatus_NotInlined;
    if (callInfo.getArg(0)->type() != MIRType_Int32)
        return InliningStatus_NotInlined;
    if (callInfo.getArg(1)->type() != MIRType_Int32)
        return InliningStatus_NotInlined;

    callInfo.setImplicitlyUsedUnchecked();

    // Specialize arr.splice(start, deleteCount) with unused return value and
    // avoid creating the result array in this case.
    if (!BytecodeIsPopped(pc)) {
        trackOptimizationOutcome(TrackedOutcome::CantInlineGeneric);
        return InliningStatus_NotInlined;
    }

    MArraySplice* ins = MArraySplice::New(alloc(),
                                          callInfo.thisArg(),
                                          callInfo.getArg(0),
                                          callInfo.getArg(1));

    current->add(ins);
    pushConstant(UndefinedValue());

    if (!resumeAfter(ins))
        return InliningStatus_Error;
    return InliningStatus_Inlined;
}

IonBuilder::InliningStatus
IonBuilder::inlineArrayJoin(CallInfo& callInfo)
{
    if (callInfo.argc() != 1 || callInfo.constructing()) {
        trackOptimizationOutcome(TrackedOutcome::CantInlineNativeBadForm);
        return InliningStatus_NotInlined;
    }

    if (getInlineReturnType() != MIRType_String)
        return InliningStatus_NotInlined;
    if (callInfo.thisArg()->type() != MIRType_Object)
        return InliningStatus_NotInlined;
    if (callInfo.getArg(0)->type() != MIRType_String)
        return InliningStatus_NotInlined;

    callInfo.setImplicitlyUsedUnchecked();

    MArrayJoin* ins = MArrayJoin::New(alloc(), callInfo.thisArg(), callInfo.getArg(0));

    current->add(ins);
    current->push(ins);

    return InliningStatus_Inlined;
}

IonBuilder::InliningStatus
IonBuilder::inlineArrayPush(CallInfo& callInfo)
{
    if (callInfo.argc() != 1 || callInfo.constructing()) {
        trackOptimizationOutcome(TrackedOutcome::CantInlineNativeBadForm);
        return InliningStatus_NotInlined;
    }

    MDefinition* obj = convertUnboxedObjects(callInfo.thisArg());
    MDefinition* value = callInfo.getArg(0);
    if (PropertyWriteNeedsTypeBarrier(alloc(), constraints(), current,
                                      &obj, nullptr, &value, /* canModify = */ false))
    {
        trackOptimizationOutcome(TrackedOutcome::NeedsTypeBarrier);
        return InliningStatus_NotInlined;
    }

    if (getInlineReturnType() != MIRType_Int32)
        return InliningStatus_NotInlined;
    if (obj->type() != MIRType_Object)
        return InliningStatus_NotInlined;

    TemporaryTypeSet* thisTypes = obj->resultTypeSet();
    if (!thisTypes)
        return InliningStatus_NotInlined;
    const Class* clasp = thisTypes->getKnownClass(constraints());
    if (clasp != &ArrayObject::class_ && clasp != &UnboxedArrayObject::class_)
        return InliningStatus_NotInlined;
    if (thisTypes->hasObjectFlags(constraints(), OBJECT_FLAG_SPARSE_INDEXES |
                                  OBJECT_FLAG_LENGTH_OVERFLOW))
    {
        trackOptimizationOutcome(TrackedOutcome::ArrayBadFlags);
        return InliningStatus_NotInlined;
    }

    if (ArrayPrototypeHasIndexedProperty(this, script())) {
        trackOptimizationOutcome(TrackedOutcome::ProtoIndexedProps);
        return InliningStatus_NotInlined;
    }

    TemporaryTypeSet::DoubleConversion conversion =
        thisTypes->convertDoubleElements(constraints());
    if (conversion == TemporaryTypeSet::AmbiguousDoubleConversion) {
        trackOptimizationOutcome(TrackedOutcome::ArrayDoubleConversion);
        return InliningStatus_NotInlined;
    }

    JSValueType unboxedType = JSVAL_TYPE_MAGIC;
    if (clasp == &UnboxedArrayObject::class_) {
        unboxedType = UnboxedArrayElementType(constraints(), obj, nullptr);
        if (unboxedType == JSVAL_TYPE_MAGIC)
            return InliningStatus_NotInlined;
    }

    callInfo.setImplicitlyUsedUnchecked();

    if (conversion == TemporaryTypeSet::AlwaysConvertToDoubles ||
        conversion == TemporaryTypeSet::MaybeConvertToDoubles)
    {
        MInstruction* valueDouble = MToDouble::New(alloc(), value);
        current->add(valueDouble);
        value = valueDouble;
    }

    if (unboxedType == JSVAL_TYPE_MAGIC)
        obj = addMaybeCopyElementsForWrite(obj, /* checkNative = */ false);

    if (NeedsPostBarrier(value))
        current->add(MPostWriteBarrier::New(alloc(), obj, value));

    MArrayPush* ins = MArrayPush::New(alloc(), obj, value, unboxedType);
    current->add(ins);
    current->push(ins);

    if (!resumeAfter(ins))
        return InliningStatus_Error;
    return InliningStatus_Inlined;
}

IonBuilder::InliningStatus
IonBuilder::inlineArrayConcat(CallInfo& callInfo)
{
    if (callInfo.argc() != 1 || callInfo.constructing()) {
        trackOptimizationOutcome(TrackedOutcome::CantInlineNativeBadForm);
        return InliningStatus_NotInlined;
    }

    MDefinition* thisArg = convertUnboxedObjects(callInfo.thisArg());
    MDefinition* objArg = convertUnboxedObjects(callInfo.getArg(0));

    // Ensure |this|, argument and result are objects.
    if (getInlineReturnType() != MIRType_Object)
        return InliningStatus_NotInlined;
    if (thisArg->type() != MIRType_Object)
        return InliningStatus_NotInlined;
    if (objArg->type() != MIRType_Object)
        return InliningStatus_NotInlined;

    // |this| and the argument must be dense arrays.
    TemporaryTypeSet* thisTypes = thisArg->resultTypeSet();
    TemporaryTypeSet* argTypes = objArg->resultTypeSet();
    if (!thisTypes || !argTypes)
        return InliningStatus_NotInlined;

    const Class* thisClasp = thisTypes->getKnownClass(constraints());
    if (thisClasp != &ArrayObject::class_ && thisClasp != &UnboxedArrayObject::class_)
        return InliningStatus_NotInlined;
    bool unboxedThis = (thisClasp == &UnboxedArrayObject::class_);
    if (thisTypes->hasObjectFlags(constraints(), OBJECT_FLAG_SPARSE_INDEXES |
                                  OBJECT_FLAG_LENGTH_OVERFLOW))
    {
        trackOptimizationOutcome(TrackedOutcome::ArrayBadFlags);
        return InliningStatus_NotInlined;
    }

    const Class* argClasp = argTypes->getKnownClass(constraints());
    if (argClasp != &ArrayObject::class_ && argClasp != &UnboxedArrayObject::class_)
        return InliningStatus_NotInlined;
    bool unboxedArg = (argClasp == &UnboxedArrayObject::class_);
    if (argTypes->hasObjectFlags(constraints(), OBJECT_FLAG_SPARSE_INDEXES |
                                 OBJECT_FLAG_LENGTH_OVERFLOW))
    {
        trackOptimizationOutcome(TrackedOutcome::ArrayBadFlags);
        return InliningStatus_NotInlined;
    }

    // Watch out for indexed properties on the prototype.
    if (ArrayPrototypeHasIndexedProperty(this, script())) {
        trackOptimizationOutcome(TrackedOutcome::ProtoIndexedProps);
        return InliningStatus_NotInlined;
    }

    // Require the 'this' types to have a specific type matching the current
    // global, so we can create the result object inline.
    if (thisTypes->getObjectCount() != 1)
        return InliningStatus_NotInlined;

    ObjectGroup* thisGroup = thisTypes->getGroup(0);
    if (!thisGroup)
        return InliningStatus_NotInlined;
    TypeSet::ObjectKey* thisKey = TypeSet::ObjectKey::get(thisGroup);
    if (thisKey->unknownProperties())
        return InliningStatus_NotInlined;

    // Don't inline if 'this' is packed and the argument may not be packed
    // (the result array will reuse the 'this' type).
    if (!thisTypes->hasObjectFlags(constraints(), OBJECT_FLAG_NON_PACKED) &&
        argTypes->hasObjectFlags(constraints(), OBJECT_FLAG_NON_PACKED))
    {
        trackOptimizationOutcome(TrackedOutcome::ArrayBadFlags);
        return InliningStatus_NotInlined;
    }

    // Constraints modeling this concat have not been generated by inference,
    // so check that type information already reflects possible side effects of
    // this call.
    HeapTypeSetKey thisElemTypes = thisKey->property(JSID_VOID);

    TemporaryTypeSet* resTypes = getInlineReturnTypeSet();
    if (!resTypes->hasType(TypeSet::ObjectType(thisKey)))
        return InliningStatus_NotInlined;

    for (unsigned i = 0; i < argTypes->getObjectCount(); i++) {
        TypeSet::ObjectKey* key = argTypes->getObject(i);
        if (!key)
            continue;

        if (key->unknownProperties())
            return InliningStatus_NotInlined;

        HeapTypeSetKey elemTypes = key->property(JSID_VOID);
        if (!elemTypes.knownSubset(constraints(), thisElemTypes))
            return InliningStatus_NotInlined;

        if (thisGroup->clasp() == &UnboxedArrayObject::class_ &&
            !CanStoreUnboxedType(alloc(), thisGroup->unboxedLayout().elementType(),
                                 MIRType_Value, elemTypes.maybeTypes()))
        {
            return InliningStatus_NotInlined;
        }
    }

    // Inline the call.
    JSObject* templateObj = inspector->getTemplateObjectForNative(pc, js::array_concat);
    if (!templateObj || templateObj->group() != thisGroup)
        return InliningStatus_NotInlined;

    callInfo.setImplicitlyUsedUnchecked();

    MArrayConcat* ins = MArrayConcat::New(alloc(), constraints(), thisArg, objArg,
                                          templateObj,
                                          templateObj->group()->initialHeap(constraints()),
                                          unboxedThis, unboxedArg);
    current->add(ins);
    current->push(ins);

    if (!resumeAfter(ins))
        return InliningStatus_Error;
    return InliningStatus_Inlined;
}

IonBuilder::InliningStatus
IonBuilder::inlineArraySlice(CallInfo& callInfo)
{
    if (callInfo.constructing()) {
        trackOptimizationOutcome(TrackedOutcome::CantInlineNativeBadForm);
        return InliningStatus_NotInlined;
    }

    MDefinition* obj = convertUnboxedObjects(callInfo.thisArg());

    // Ensure |this| and result are objects.
    if (getInlineReturnType() != MIRType_Object)
        return InliningStatus_NotInlined;
    if (obj->type() != MIRType_Object)
        return InliningStatus_NotInlined;

    // Arguments for the sliced region must be integers.
    if (callInfo.argc() > 0) {
        if (callInfo.getArg(0)->type() != MIRType_Int32)
            return InliningStatus_NotInlined;
        if (callInfo.argc() > 1) {
            if (callInfo.getArg(1)->type() != MIRType_Int32)
                return InliningStatus_NotInlined;
        }
    }

    // |this| must be a dense array.
    TemporaryTypeSet* thisTypes = obj->resultTypeSet();
    if (!thisTypes)
        return InliningStatus_NotInlined;

    const Class* clasp = thisTypes->getKnownClass(constraints());
    if (clasp != &ArrayObject::class_ && clasp != &UnboxedArrayObject::class_)
        return InliningStatus_NotInlined;
    if (thisTypes->hasObjectFlags(constraints(), OBJECT_FLAG_SPARSE_INDEXES |
                                  OBJECT_FLAG_LENGTH_OVERFLOW))
    {
        trackOptimizationOutcome(TrackedOutcome::ArrayBadFlags);
        return InliningStatus_NotInlined;
    }

    JSValueType unboxedType = JSVAL_TYPE_MAGIC;
    if (clasp == &UnboxedArrayObject::class_) {
        unboxedType = UnboxedArrayElementType(constraints(), obj, nullptr);
        if (unboxedType == JSVAL_TYPE_MAGIC)
            return InliningStatus_NotInlined;
    }

    // Watch out for indexed properties on the prototype.
    if (ArrayPrototypeHasIndexedProperty(this, script())) {
        trackOptimizationOutcome(TrackedOutcome::ProtoIndexedProps);
        return InliningStatus_NotInlined;
    }

    // The group of the result will be dynamically fixed up to match the input
    // object, allowing us to handle 'this' objects that might have more than
    // one group. Make sure that no singletons can be sliced here.
    for (unsigned i = 0; i < thisTypes->getObjectCount(); i++) {
        TypeSet::ObjectKey* key = thisTypes->getObject(i);
        if (key && key->isSingleton())
            return InliningStatus_NotInlined;
    }

    // Inline the call.
    JSObject* templateObj = inspector->getTemplateObjectForNative(pc, js::array_slice);
    if (!templateObj)
        return InliningStatus_NotInlined;

    if (unboxedType == JSVAL_TYPE_MAGIC) {
        if (!templateObj->is<ArrayObject>())
            return InliningStatus_NotInlined;
    } else {
        if (!templateObj->is<UnboxedArrayObject>())
            return InliningStatus_NotInlined;
        if (templateObj->as<UnboxedArrayObject>().elementType() != unboxedType)
            return InliningStatus_NotInlined;
    }

    callInfo.setImplicitlyUsedUnchecked();

    MDefinition* begin;
    if (callInfo.argc() > 0)
        begin = callInfo.getArg(0);
    else
        begin = constant(Int32Value(0));

    MDefinition* end;
    if (callInfo.argc() > 1) {
        end = callInfo.getArg(1);
    } else if (clasp == &ArrayObject::class_) {
        MElements* elements = MElements::New(alloc(), obj);
        current->add(elements);

        end = MArrayLength::New(alloc(), elements);
        current->add(end->toInstruction());
    } else {
        end = MUnboxedArrayLength::New(alloc(), obj);
        current->add(end->toInstruction());
    }

    MArraySlice* ins = MArraySlice::New(alloc(), constraints(),
                                        obj, begin, end,
                                        templateObj,
                                        templateObj->group()->initialHeap(constraints()),
                                        unboxedType);
    current->add(ins);
    current->push(ins);

    if (!resumeAfter(ins))
        return InliningStatus_Error;
    return InliningStatus_Inlined;
}

IonBuilder::InliningStatus
IonBuilder::inlineMathAbs(CallInfo& callInfo)
{
    if (callInfo.argc() != 1 || callInfo.constructing()) {
        trackOptimizationOutcome(TrackedOutcome::CantInlineNativeBadForm);
        return InliningStatus_NotInlined;
    }

    MIRType returnType = getInlineReturnType();
    MIRType argType = callInfo.getArg(0)->type();
    if (!IsNumberType(argType))
        return InliningStatus_NotInlined;

    // Either argType == returnType, or
    //        argType == Double or Float32, returnType == Int, or
    //        argType == Float32, returnType == Double
    if (argType != returnType && !(IsFloatingPointType(argType) && returnType == MIRType_Int32)
        && !(argType == MIRType_Float32 && returnType == MIRType_Double))
    {
        return InliningStatus_NotInlined;
    }

    callInfo.setImplicitlyUsedUnchecked();

    // If the arg is a Float32, we specialize the op as double, it will be specialized
    // as float32 if necessary later.
    MIRType absType = (argType == MIRType_Float32) ? MIRType_Double : argType;
    MInstruction* ins = MAbs::New(alloc(), callInfo.getArg(0), absType);
    current->add(ins);

    current->push(ins);
    return InliningStatus_Inlined;
}

IonBuilder::InliningStatus
IonBuilder::inlineMathFloor(CallInfo& callInfo)
{
    if (callInfo.argc() != 1 || callInfo.constructing()) {
        trackOptimizationOutcome(TrackedOutcome::CantInlineNativeBadForm);
        return InliningStatus_NotInlined;
    }

    MIRType argType = callInfo.getArg(0)->type();
    MIRType returnType = getInlineReturnType();

    // Math.floor(int(x)) == int(x)
    if (argType == MIRType_Int32 && returnType == MIRType_Int32) {
        callInfo.setImplicitlyUsedUnchecked();
        // The int operand may be something which bails out if the actual value
        // is not in the range of the result type of the MIR. We need to tell
        // the optimizer to preserve this bailout even if the final result is
        // fully truncated.
        MLimitedTruncate* ins = MLimitedTruncate::New(alloc(), callInfo.getArg(0),
                                                      MDefinition::IndirectTruncate);
        current->add(ins);
        current->push(ins);
        return InliningStatus_Inlined;
    }

    if (IsFloatingPointType(argType) && returnType == MIRType_Int32) {
        callInfo.setImplicitlyUsedUnchecked();
        MFloor* ins = MFloor::New(alloc(), callInfo.getArg(0));
        current->add(ins);
        current->push(ins);
        return InliningStatus_Inlined;
    }

    if (IsFloatingPointType(argType) && returnType == MIRType_Double) {
        callInfo.setImplicitlyUsedUnchecked();
        MMathFunction* ins = MMathFunction::New(alloc(), callInfo.getArg(0), MMathFunction::Floor, nullptr);
        current->add(ins);
        current->push(ins);
        return InliningStatus_Inlined;
    }

    return InliningStatus_NotInlined;
}

IonBuilder::InliningStatus
IonBuilder::inlineMathCeil(CallInfo& callInfo)
{
    if (callInfo.argc() != 1 || callInfo.constructing()) {
        trackOptimizationOutcome(TrackedOutcome::CantInlineNativeBadForm);
        return InliningStatus_NotInlined;
    }

    MIRType argType = callInfo.getArg(0)->type();
    MIRType returnType = getInlineReturnType();

    // Math.ceil(int(x)) == int(x)
    if (argType == MIRType_Int32 && returnType == MIRType_Int32) {
        callInfo.setImplicitlyUsedUnchecked();
        // The int operand may be something which bails out if the actual value
        // is not in the range of the result type of the MIR. We need to tell
        // the optimizer to preserve this bailout even if the final result is
        // fully truncated.
        MLimitedTruncate* ins = MLimitedTruncate::New(alloc(), callInfo.getArg(0),
                                                      MDefinition::IndirectTruncate);
        current->add(ins);
        current->push(ins);
        return InliningStatus_Inlined;
    }

    if (IsFloatingPointType(argType) && returnType == MIRType_Int32) {
        callInfo.setImplicitlyUsedUnchecked();
        MCeil* ins = MCeil::New(alloc(), callInfo.getArg(0));
        current->add(ins);
        current->push(ins);
        return InliningStatus_Inlined;
    }

    if (IsFloatingPointType(argType) && returnType == MIRType_Double) {
        callInfo.setImplicitlyUsedUnchecked();
        MMathFunction* ins = MMathFunction::New(alloc(), callInfo.getArg(0), MMathFunction::Ceil, nullptr);
        current->add(ins);
        current->push(ins);
        return InliningStatus_Inlined;
    }

    return InliningStatus_NotInlined;
}

IonBuilder::InliningStatus
IonBuilder::inlineMathClz32(CallInfo& callInfo)
{
    if (callInfo.argc() != 1 || callInfo.constructing()) {
        trackOptimizationOutcome(TrackedOutcome::CantInlineNativeBadForm);
        return InliningStatus_NotInlined;
    }

    MIRType returnType = getInlineReturnType();
    if (returnType != MIRType_Int32)
        return InliningStatus_NotInlined;

    if (!IsNumberType(callInfo.getArg(0)->type()))
        return InliningStatus_NotInlined;

    callInfo.setImplicitlyUsedUnchecked();

    MClz* ins = MClz::New(alloc(), callInfo.getArg(0));
    current->add(ins);
    current->push(ins);
    return InliningStatus_Inlined;

}

IonBuilder::InliningStatus
IonBuilder::inlineMathRound(CallInfo& callInfo)
{
    if (callInfo.argc() != 1 || callInfo.constructing()) {
        trackOptimizationOutcome(TrackedOutcome::CantInlineNativeBadForm);
        return InliningStatus_NotInlined;
    }

    MIRType returnType = getInlineReturnType();
    MIRType argType = callInfo.getArg(0)->type();

    // Math.round(int(x)) == int(x)
    if (argType == MIRType_Int32 && returnType == MIRType_Int32) {
        callInfo.setImplicitlyUsedUnchecked();
        // The int operand may be something which bails out if the actual value
        // is not in the range of the result type of the MIR. We need to tell
        // the optimizer to preserve this bailout even if the final result is
        // fully truncated.
        MLimitedTruncate* ins = MLimitedTruncate::New(alloc(), callInfo.getArg(0),
                                                      MDefinition::IndirectTruncate);
        current->add(ins);
        current->push(ins);
        return InliningStatus_Inlined;
    }

    if (IsFloatingPointType(argType) && returnType == MIRType_Int32) {
        callInfo.setImplicitlyUsedUnchecked();
        MRound* ins = MRound::New(alloc(), callInfo.getArg(0));
        current->add(ins);
        current->push(ins);
        return InliningStatus_Inlined;
    }

    if (IsFloatingPointType(argType) && returnType == MIRType_Double) {
        callInfo.setImplicitlyUsedUnchecked();
        MMathFunction* ins = MMathFunction::New(alloc(), callInfo.getArg(0), MMathFunction::Round, nullptr);
        current->add(ins);
        current->push(ins);
        return InliningStatus_Inlined;
    }

    return InliningStatus_NotInlined;
}

IonBuilder::InliningStatus
IonBuilder::inlineMathSqrt(CallInfo& callInfo)
{
    if (callInfo.argc() != 1 || callInfo.constructing()) {
        trackOptimizationOutcome(TrackedOutcome::CantInlineNativeBadForm);
        return InliningStatus_NotInlined;
    }

    MIRType argType = callInfo.getArg(0)->type();
    if (getInlineReturnType() != MIRType_Double)
        return InliningStatus_NotInlined;
    if (!IsNumberType(argType))
        return InliningStatus_NotInlined;

    callInfo.setImplicitlyUsedUnchecked();

    MSqrt* sqrt = MSqrt::New(alloc(), callInfo.getArg(0));
    current->add(sqrt);
    current->push(sqrt);
    return InliningStatus_Inlined;
}

IonBuilder::InliningStatus
IonBuilder::inlineMathAtan2(CallInfo& callInfo)
{
    if (callInfo.argc() != 2 || callInfo.constructing()) {
        trackOptimizationOutcome(TrackedOutcome::CantInlineNativeBadForm);
        return InliningStatus_NotInlined;
    }

    if (getInlineReturnType() != MIRType_Double)
        return InliningStatus_NotInlined;

    MIRType argType0 = callInfo.getArg(0)->type();
    MIRType argType1 = callInfo.getArg(1)->type();

    if (!IsNumberType(argType0) || !IsNumberType(argType1))
        return InliningStatus_NotInlined;

    callInfo.setImplicitlyUsedUnchecked();

    MAtan2* atan2 = MAtan2::New(alloc(), callInfo.getArg(0), callInfo.getArg(1));
    current->add(atan2);
    current->push(atan2);
    return InliningStatus_Inlined;
}

IonBuilder::InliningStatus
IonBuilder::inlineMathHypot(CallInfo& callInfo)
{
    if (callInfo.constructing()) {
        trackOptimizationOutcome(TrackedOutcome::CantInlineNativeBadForm);
        return InliningStatus_NotInlined;
    }

    uint32_t argc = callInfo.argc();
    if (argc < 2 || argc > 4) {
        trackOptimizationOutcome(TrackedOutcome::CantInlineNativeBadForm);
        return InliningStatus_NotInlined;
    }

    if (getInlineReturnType() != MIRType_Double)
        return InliningStatus_NotInlined;

    MDefinitionVector vector(alloc());
    if (!vector.reserve(argc))
        return InliningStatus_NotInlined;

    for (uint32_t i = 0; i < argc; ++i) {
        MDefinition * arg = callInfo.getArg(i);
        if (!IsNumberType(arg->type()))
            return InliningStatus_NotInlined;
        vector.infallibleAppend(arg);
    }

    callInfo.setImplicitlyUsedUnchecked();
    MHypot* hypot = MHypot::New(alloc(), vector);

    if (!hypot)
        return InliningStatus_NotInlined;

    current->add(hypot);
    current->push(hypot);
    return InliningStatus_Inlined;
}

IonBuilder::InliningStatus
IonBuilder::inlineMathPowHelper(MDefinition* lhs, MDefinition* rhs, MIRType outputType)
{
    // Typechecking.
    MIRType baseType = lhs->type();
    MIRType powerType = rhs->type();

    if (outputType != MIRType_Int32 && outputType != MIRType_Double)
        return InliningStatus_NotInlined;
    if (!IsNumberType(baseType))
        return InliningStatus_NotInlined;
    if (!IsNumberType(powerType))
        return InliningStatus_NotInlined;

    MDefinition* base = lhs;
    MDefinition* power = rhs;
    MDefinition* output = nullptr;

    // Optimize some constant powers.
    if (rhs->isConstantValue() && rhs->constantValue().isNumber()) {
        double pow = rhs->constantValue().toNumber();

        // Math.pow(x, 0.5) is a sqrt with edge-case detection.
        if (pow == 0.5) {
            MPowHalf* half = MPowHalf::New(alloc(), base);
            current->add(half);
            output = half;
        }

        // Math.pow(x, -0.5) == 1 / Math.pow(x, 0.5), even for edge cases.
        if (pow == -0.5) {
            MPowHalf* half = MPowHalf::New(alloc(), base);
            current->add(half);
            MConstant* one = MConstant::New(alloc(), DoubleValue(1.0));
            current->add(one);
            MDiv* div = MDiv::New(alloc(), one, half, MIRType_Double);
            current->add(div);
            output = div;
        }

        // Math.pow(x, 1) == x.
        if (pow == 1.0)
            output = base;

        // Math.pow(x, 2) == x*x.
        if (pow == 2.0) {
            MMul* mul = MMul::New(alloc(), base, base, outputType);
            current->add(mul);
            output = mul;
        }

        // Math.pow(x, 3) == x*x*x.
        if (pow == 3.0) {
            MMul* mul1 = MMul::New(alloc(), base, base, outputType);
            current->add(mul1);
            MMul* mul2 = MMul::New(alloc(), base, mul1, outputType);
            current->add(mul2);
            output = mul2;
        }

        // Math.pow(x, 4) == y*y, where y = x*x.
        if (pow == 4.0) {
            MMul* y = MMul::New(alloc(), base, base, outputType);
            current->add(y);
            MMul* mul = MMul::New(alloc(), y, y, outputType);
            current->add(mul);
            output = mul;
        }
    }

    // Use MPow for other powers
    if (!output) {
        if (powerType == MIRType_Float32)
            powerType = MIRType_Double;
        MPow* pow = MPow::New(alloc(), base, power, powerType);
        current->add(pow);
        output = pow;
    }

    // Cast to the right type
    if (outputType == MIRType_Int32 && output->type() != MIRType_Int32) {
        MToInt32* toInt = MToInt32::New(alloc(), output);
        current->add(toInt);
        output = toInt;
    }
    if (outputType == MIRType_Double && output->type() != MIRType_Double) {
        MToDouble* toDouble = MToDouble::New(alloc(), output);
        current->add(toDouble);
        output = toDouble;
    }

    current->push(output);
    return InliningStatus_Inlined;
}

IonBuilder::InliningStatus
IonBuilder::inlineMathPow(CallInfo& callInfo)
{
    if (callInfo.argc() != 2 || callInfo.constructing()) {
        trackOptimizationOutcome(TrackedOutcome::CantInlineNativeBadForm);
        return InliningStatus_NotInlined;
    }

    IonBuilder::InliningStatus status =
        inlineMathPowHelper(callInfo.getArg(0), callInfo.getArg(1), getInlineReturnType());

    if (status == IonBuilder::InliningStatus_Inlined)
        callInfo.setImplicitlyUsedUnchecked();

    return status;
}

IonBuilder::InliningStatus
IonBuilder::inlineMathRandom(CallInfo& callInfo)
{
    if (callInfo.constructing()) {
        trackOptimizationOutcome(TrackedOutcome::CantInlineNativeBadForm);
        return InliningStatus_NotInlined;
    }

    if (getInlineReturnType() != MIRType_Double)
        return InliningStatus_NotInlined;

    // MRandom JIT code directly accesses the RNG. It's (barely) possible to
    // inline Math.random without it having been called yet, so ensure RNG
    // state that isn't guaranteed to be initialized already.
    script()->compartment()->ensureRandomNumberGenerator();

    callInfo.setImplicitlyUsedUnchecked();

    MRandom* rand = MRandom::New(alloc());
    current->add(rand);
    current->push(rand);
    return InliningStatus_Inlined;
}

IonBuilder::InliningStatus
IonBuilder::inlineMathImul(CallInfo& callInfo)
{
    if (callInfo.argc() != 2 || callInfo.constructing()) {
        trackOptimizationOutcome(TrackedOutcome::CantInlineNativeBadForm);
        return InliningStatus_NotInlined;
    }

    MIRType returnType = getInlineReturnType();
    if (returnType != MIRType_Int32)
        return InliningStatus_NotInlined;

    if (!IsNumberType(callInfo.getArg(0)->type()))
        return InliningStatus_NotInlined;
    if (!IsNumberType(callInfo.getArg(1)->type()))
        return InliningStatus_NotInlined;

    callInfo.setImplicitlyUsedUnchecked();

    MInstruction* first = MTruncateToInt32::New(alloc(), callInfo.getArg(0));
    current->add(first);

    MInstruction* second = MTruncateToInt32::New(alloc(), callInfo.getArg(1));
    current->add(second);

    MMul* ins = MMul::New(alloc(), first, second, MIRType_Int32, MMul::Integer);
    current->add(ins);
    current->push(ins);
    return InliningStatus_Inlined;
}

IonBuilder::InliningStatus
IonBuilder::inlineMathFRound(CallInfo& callInfo)
{
    if (callInfo.argc() != 1 || callInfo.constructing()) {
        trackOptimizationOutcome(TrackedOutcome::CantInlineNativeBadForm);
        return InliningStatus_NotInlined;
    }

    // MIRType can't be Float32, as this point, as getInlineReturnType uses JSVal types
    // to infer the returned MIR type.
    TemporaryTypeSet* returned = getInlineReturnTypeSet();
    if (returned->empty()) {
        // As there's only one possible returned type, just add it to the observed
        // returned typeset
        returned->addType(TypeSet::DoubleType(), alloc_->lifoAlloc());
    } else {
        MIRType returnType = getInlineReturnType();
        if (!IsNumberType(returnType))
            return InliningStatus_NotInlined;
    }

    MIRType arg = callInfo.getArg(0)->type();
    if (!IsNumberType(arg))
        return InliningStatus_NotInlined;

    callInfo.setImplicitlyUsedUnchecked();

    MToFloat32* ins = MToFloat32::New(alloc(), callInfo.getArg(0));
    current->add(ins);
    current->push(ins);
    return InliningStatus_Inlined;
}

IonBuilder::InliningStatus
IonBuilder::inlineMathMinMax(CallInfo& callInfo, bool max)
{
    if (callInfo.argc() < 1 || callInfo.constructing()) {
        trackOptimizationOutcome(TrackedOutcome::CantInlineNativeBadForm);
        return InliningStatus_NotInlined;
    }

    MIRType returnType = getInlineReturnType();
    if (!IsNumberType(returnType))
        return InliningStatus_NotInlined;

    MDefinitionVector int32_cases(alloc());
    for (unsigned i = 0; i < callInfo.argc(); i++) {
        MDefinition* arg = callInfo.getArg(i);

        switch (arg->type()) {
          case MIRType_Int32:
            if (!int32_cases.append(arg))
                return InliningStatus_Error;
            break;
          case MIRType_Double:
          case MIRType_Float32:
            // Don't force a double MMinMax for arguments that would be a NOP
            // when doing an integer MMinMax.
            if (arg->isConstantValue()) {
                double cte = arg->constantValue().toDouble();
                // min(int32, cte >= INT32_MAX) = int32
                if (cte >= INT32_MAX && !max)
                    break;
                // max(int32, cte <= INT32_MIN) = int32
                if (cte <= INT32_MIN && max)
                    break;
            }

            // Force double MMinMax if argument is a "effectfull" double.
            returnType = MIRType_Double;
            break;
          default:
            return InliningStatus_NotInlined;
        }
    }

    if (int32_cases.length() == 0)
        returnType = MIRType_Double;

    callInfo.setImplicitlyUsedUnchecked();

    MDefinitionVector& cases = (returnType == MIRType_Int32) ? int32_cases : callInfo.argv();

    if (cases.length() == 1) {
        MLimitedTruncate* limit = MLimitedTruncate::New(alloc(), cases[0], MDefinition::NoTruncate);
        current->add(limit);
        current->push(limit);
        return InliningStatus_Inlined;
    }

    // Chain N-1 MMinMax instructions to compute the MinMax.
    MMinMax* last = MMinMax::New(alloc(), cases[0], cases[1], returnType, max);
    current->add(last);

    for (unsigned i = 2; i < cases.length(); i++) {
        MMinMax* ins = MMinMax::New(alloc(), last, cases[i], returnType, max);
        current->add(ins);
        last = ins;
    }

    current->push(last);
    return InliningStatus_Inlined;
}

IonBuilder::InliningStatus
IonBuilder::inlineStringObject(CallInfo& callInfo)
{
    if (callInfo.argc() != 1 || !callInfo.constructing()) {
        trackOptimizationOutcome(TrackedOutcome::CantInlineNativeBadForm);
        return InliningStatus_NotInlined;
    }

    // ConvertToString doesn't support objects.
    if (callInfo.getArg(0)->mightBeType(MIRType_Object))
        return InliningStatus_NotInlined;

    JSObject* templateObj = inspector->getTemplateObjectForNative(pc, StringConstructor);
    if (!templateObj)
        return InliningStatus_NotInlined;
    MOZ_ASSERT(templateObj->is<StringObject>());

    callInfo.setImplicitlyUsedUnchecked();

    MNewStringObject* ins = MNewStringObject::New(alloc(), callInfo.getArg(0), templateObj);
    current->add(ins);
    current->push(ins);

    if (!resumeAfter(ins))
        return InliningStatus_Error;

    return InliningStatus_Inlined;
}

IonBuilder::InliningStatus
IonBuilder::inlineConstantStringSplit(CallInfo& callInfo)
{
    if (!callInfo.thisArg()->isConstant())
        return InliningStatus_NotInlined;

    if (!callInfo.getArg(0)->isConstant())
        return InliningStatus_NotInlined;

    const js::Value* argval = callInfo.getArg(0)->toConstant()->vp();
    if (!argval->isString())
        return InliningStatus_NotInlined;

    const js::Value* strval = callInfo.thisArg()->toConstant()->vp();
    if (!strval->isString())
        return InliningStatus_NotInlined;

    MOZ_ASSERT(callInfo.getArg(0)->type() == MIRType_String);
    MOZ_ASSERT(callInfo.thisArg()->type() == MIRType_String);

    // Check if exist a template object in stub.
    JSString* stringThis = nullptr;
    JSString* stringArg = nullptr;
    JSObject* templateObject = nullptr;
    if (!inspector->isOptimizableCallStringSplit(pc, &stringThis, &stringArg, &templateObject))
        return InliningStatus_NotInlined;

    MOZ_ASSERT(stringThis);
    MOZ_ASSERT(stringArg);
    MOZ_ASSERT(templateObject);

    if (strval->toString() != stringThis)
        return InliningStatus_NotInlined;

    if (argval->toString() != stringArg)
        return InliningStatus_NotInlined;

    // Check if |templateObject| is valid.
    TypeSet::ObjectKey* retType = TypeSet::ObjectKey::get(templateObject);
    if (retType->unknownProperties())
        return InliningStatus_NotInlined;

    HeapTypeSetKey key = retType->property(JSID_VOID);
    if (!key.maybeTypes())
        return InliningStatus_NotInlined;

    if (!key.maybeTypes()->hasType(TypeSet::StringType()))
        return InliningStatus_NotInlined;

    uint32_t initLength = GetAnyBoxedOrUnboxedArrayLength(templateObject);
    if (GetAnyBoxedOrUnboxedInitializedLength(templateObject) != initLength)
        return InliningStatus_NotInlined;

    Vector<MConstant*, 0, SystemAllocPolicy> arrayValues;
    for (uint32_t i = 0; i < initLength; i++) {
        Value str = GetAnyBoxedOrUnboxedDenseElement(templateObject, i);
        MOZ_ASSERT(str.toString()->isAtom());
        MConstant* value = MConstant::New(alloc(), str, constraints());
        if (!TypeSetIncludes(key.maybeTypes(), value->type(), value->resultTypeSet()))
            return InliningStatus_NotInlined;

        if (!arrayValues.append(value))
            return InliningStatus_Error;
    }
    callInfo.setImplicitlyUsedUnchecked();

    TemporaryTypeSet::DoubleConversion conversion =
            getInlineReturnTypeSet()->convertDoubleElements(constraints());
    if (conversion == TemporaryTypeSet::AlwaysConvertToDoubles)
        return InliningStatus_NotInlined;

    MConstant* templateConst = MConstant::NewConstraintlessObject(alloc(), templateObject);
    current->add(templateConst);

    MNewArray* ins = MNewArray::New(alloc(), constraints(), initLength, templateConst,
                                    templateObject->group()->initialHeap(constraints()), pc);

    current->add(ins);
    current->push(ins);

    if (!initLength) {
        if (!resumeAfter(ins))
            return InliningStatus_Error;
        return InliningStatus_Inlined;
    }

    JSValueType unboxedType = GetBoxedOrUnboxedType(templateObject);

    // Store all values, no need to initialize the length after each as
    // jsop_initelem_array is doing because we do not expect to bailout
    // because the memory is supposed to be allocated by now.
    for (uint32_t i = 0; i < initLength; i++) {
       MConstant* value = arrayValues[i];
       current->add(value);

       if (!initializeArrayElement(ins, i, value, unboxedType, /* addResumePoint = */ false))
           return InliningStatus_Error;
    }

    MInstruction* setLength = setInitializedLength(ins, unboxedType, initLength);
    if (!resumeAfter(setLength))
        return InliningStatus_Error;

    return InliningStatus_Inlined;
}

IonBuilder::InliningStatus
IonBuilder::inlineStringSplit(CallInfo& callInfo)
{
    if (callInfo.argc() != 1 || callInfo.constructing()) {
        trackOptimizationOutcome(TrackedOutcome::CantInlineNativeBadForm);
        return InliningStatus_NotInlined;
    }

    if (callInfo.thisArg()->type() != MIRType_String)
        return InliningStatus_NotInlined;
    if (callInfo.getArg(0)->type() != MIRType_String)
        return InliningStatus_NotInlined;

    IonBuilder::InliningStatus resultConstStringSplit = inlineConstantStringSplit(callInfo);
    if (resultConstStringSplit != InliningStatus_NotInlined)
        return resultConstStringSplit;

    JSObject* templateObject = inspector->getTemplateObjectForNative(pc, js::str_split);
    if (!templateObject)
        return InliningStatus_NotInlined;

    TypeSet::ObjectKey* retKey = TypeSet::ObjectKey::get(templateObject);
    if (retKey->unknownProperties())
        return InliningStatus_NotInlined;

    HeapTypeSetKey key = retKey->property(JSID_VOID);
    if (!key.maybeTypes())
        return InliningStatus_NotInlined;

    if (!key.maybeTypes()->hasType(TypeSet::StringType())) {
        key.freeze(constraints());
        return InliningStatus_NotInlined;
    }

    callInfo.setImplicitlyUsedUnchecked();
    MConstant* templateObjectDef = MConstant::New(alloc(), ObjectValue(*templateObject),
                                                  constraints());
    current->add(templateObjectDef);

    MStringSplit* ins = MStringSplit::New(alloc(), constraints(), callInfo.thisArg(),
                                          callInfo.getArg(0), templateObjectDef);
    current->add(ins);
    current->push(ins);

    return InliningStatus_Inlined;
}

IonBuilder::InliningStatus
IonBuilder::inlineStrCharCodeAt(CallInfo& callInfo)
{
    if (callInfo.argc() != 1 || callInfo.constructing()) {
        trackOptimizationOutcome(TrackedOutcome::CantInlineNativeBadForm);
        return InliningStatus_NotInlined;
    }

    if (getInlineReturnType() != MIRType_Int32)
        return InliningStatus_NotInlined;
    if (callInfo.thisArg()->type() != MIRType_String && callInfo.thisArg()->type() != MIRType_Value)
        return InliningStatus_NotInlined;
    MIRType argType = callInfo.getArg(0)->type();
    if (argType != MIRType_Int32 && argType != MIRType_Double)
        return InliningStatus_NotInlined;

    // Check for STR.charCodeAt(IDX) where STR is a constant string and IDX is a
    // constant integer.
    InliningStatus constInlineStatus = inlineConstantCharCodeAt(callInfo);
    if (constInlineStatus != InliningStatus_NotInlined)
        return constInlineStatus;

    callInfo.setImplicitlyUsedUnchecked();

    MInstruction* index = MToInt32::New(alloc(), callInfo.getArg(0));
    current->add(index);

    MStringLength* length = MStringLength::New(alloc(), callInfo.thisArg());
    current->add(length);

    index = addBoundsCheck(index, length);

    MCharCodeAt* charCode = MCharCodeAt::New(alloc(), callInfo.thisArg(), index);
    current->add(charCode);
    current->push(charCode);
    return InliningStatus_Inlined;
}

IonBuilder::InliningStatus
IonBuilder::inlineConstantCharCodeAt(CallInfo& callInfo)
{
    if (!callInfo.thisArg()->isConstantValue() || !callInfo.getArg(0)->isConstantValue()) {
        trackOptimizationOutcome(TrackedOutcome::CantInlineGeneric);
        return InliningStatus_NotInlined;
    }

    const js::Value* strval = callInfo.thisArg()->constantVp();
    const js::Value* idxval  = callInfo.getArg(0)->constantVp();

    if (!strval->isString() || !idxval->isInt32())
        return InliningStatus_NotInlined;

    JSString* str = strval->toString();
    if (!str->isLinear()) {
        trackOptimizationOutcome(TrackedOutcome::CantInlineGeneric);
        return InliningStatus_NotInlined;
    }

    int32_t idx = idxval->toInt32();
    if (idx < 0 || (uint32_t(idx) >= str->length())) {
        trackOptimizationOutcome(TrackedOutcome::OutOfBounds);
        return InliningStatus_NotInlined;
    }

    callInfo.setImplicitlyUsedUnchecked();

    JSLinearString& linstr = str->asLinear();
    char16_t ch = linstr.latin1OrTwoByteChar(idx);
    MConstant* result = MConstant::New(alloc(), Int32Value(ch));
    current->add(result);
    current->push(result);
    return InliningStatus_Inlined;
}

IonBuilder::InliningStatus
IonBuilder::inlineStrFromCharCode(CallInfo& callInfo)
{
    if (callInfo.argc() != 1 || callInfo.constructing()) {
        trackOptimizationOutcome(TrackedOutcome::CantInlineNativeBadForm);
        return InliningStatus_NotInlined;
    }

    if (getInlineReturnType() != MIRType_String)
        return InliningStatus_NotInlined;
    if (callInfo.getArg(0)->type() != MIRType_Int32)
        return InliningStatus_NotInlined;

    callInfo.setImplicitlyUsedUnchecked();

    MToInt32* charCode = MToInt32::New(alloc(), callInfo.getArg(0));
    current->add(charCode);

    MFromCharCode* string = MFromCharCode::New(alloc(), charCode);
    current->add(string);
    current->push(string);
    return InliningStatus_Inlined;
}

IonBuilder::InliningStatus
IonBuilder::inlineStrCharAt(CallInfo& callInfo)
{
    if (callInfo.argc() != 1 || callInfo.constructing()) {
        trackOptimizationOutcome(TrackedOutcome::CantInlineNativeBadForm);
        return InliningStatus_NotInlined;
    }

    if (getInlineReturnType() != MIRType_String)
        return InliningStatus_NotInlined;
    if (callInfo.thisArg()->type() != MIRType_String)
        return InliningStatus_NotInlined;
    MIRType argType = callInfo.getArg(0)->type();
    if (argType != MIRType_Int32 && argType != MIRType_Double)
        return InliningStatus_NotInlined;

    callInfo.setImplicitlyUsedUnchecked();

    MInstruction* index = MToInt32::New(alloc(), callInfo.getArg(0));
    current->add(index);

    MStringLength* length = MStringLength::New(alloc(), callInfo.thisArg());
    current->add(length);

    index = addBoundsCheck(index, length);

    // String.charAt(x) = String.fromCharCode(String.charCodeAt(x))
    MCharCodeAt* charCode = MCharCodeAt::New(alloc(), callInfo.thisArg(), index);
    current->add(charCode);

    MFromCharCode* string = MFromCharCode::New(alloc(), charCode);
    current->add(string);
    current->push(string);
    return InliningStatus_Inlined;
}

IonBuilder::InliningStatus
IonBuilder::inlineRegExpExec(CallInfo& callInfo)
{
    if (callInfo.argc() != 1 || callInfo.constructing()) {
        trackOptimizationOutcome(TrackedOutcome::CantInlineNativeBadForm);
        return InliningStatus_NotInlined;
    }

    if (callInfo.thisArg()->type() != MIRType_Object)
        return InliningStatus_NotInlined;

    TemporaryTypeSet* thisTypes = callInfo.thisArg()->resultTypeSet();
    const Class* clasp = thisTypes ? thisTypes->getKnownClass(constraints()) : nullptr;
    if (clasp != &RegExpObject::class_)
        return InliningStatus_NotInlined;

    if (callInfo.getArg(0)->mightBeType(MIRType_Object))
        return InliningStatus_NotInlined;

    JSContext* cx = GetJitContext()->cx;
    if (!cx->compartment()->jitCompartment()->ensureRegExpExecStubExists(cx))
        return InliningStatus_Error;

    callInfo.setImplicitlyUsedUnchecked();

    MInstruction* exec = MRegExpExec::New(alloc(), callInfo.thisArg(), callInfo.getArg(0));
    current->add(exec);
    current->push(exec);

    if (!resumeAfter(exec))
        return InliningStatus_Error;

    if (!pushTypeBarrier(exec, getInlineReturnTypeSet(), BarrierKind::TypeSet))
        return InliningStatus_Error;

    return InliningStatus_Inlined;
}

IonBuilder::InliningStatus
IonBuilder::inlineRegExpTest(CallInfo& callInfo)
{
    if (callInfo.argc() != 1 || callInfo.constructing()) {
        trackOptimizationOutcome(TrackedOutcome::CantInlineNativeBadForm);
        return InliningStatus_NotInlined;
    }

    // TI can infer a nullptr return type of regexp_test with eager compilation.
    if (CallResultEscapes(pc) && getInlineReturnType() != MIRType_Boolean)
        return InliningStatus_NotInlined;

    if (callInfo.thisArg()->type() != MIRType_Object)
        return InliningStatus_NotInlined;
    TemporaryTypeSet* thisTypes = callInfo.thisArg()->resultTypeSet();
    const Class* clasp = thisTypes ? thisTypes->getKnownClass(constraints()) : nullptr;
    if (clasp != &RegExpObject::class_)
        return InliningStatus_NotInlined;
    if (callInfo.getArg(0)->mightBeType(MIRType_Object))
        return InliningStatus_NotInlined;

    JSContext* cx = GetJitContext()->cx;
    if (!cx->compartment()->jitCompartment()->ensureRegExpTestStubExists(cx))
        return InliningStatus_Error;

    callInfo.setImplicitlyUsedUnchecked();

    MInstruction* match = MRegExpTest::New(alloc(), callInfo.thisArg(), callInfo.getArg(0));
    current->add(match);
    current->push(match);
    if (!resumeAfter(match))
        return InliningStatus_Error;

    return InliningStatus_Inlined;
}

IonBuilder::InliningStatus
IonBuilder::inlineStrReplace(CallInfo& callInfo)
{
    if (callInfo.argc() != 2 || callInfo.constructing()) {
        trackOptimizationOutcome(TrackedOutcome::CantInlineNativeBadForm);
        return InliningStatus_NotInlined;
    }

    // Return: String.
    if (getInlineReturnType() != MIRType_String)
        return InliningStatus_NotInlined;

    // This: String.
    if (callInfo.thisArg()->type() != MIRType_String)
        return InliningStatus_NotInlined;

    // Arg 0: RegExp.
    TemporaryTypeSet* arg0Type = callInfo.getArg(0)->resultTypeSet();
    const Class* clasp = arg0Type ? arg0Type->getKnownClass(constraints()) : nullptr;
    if (clasp != &RegExpObject::class_ && callInfo.getArg(0)->type() != MIRType_String)
        return InliningStatus_NotInlined;

    // Arg 1: String.
    if (callInfo.getArg(1)->type() != MIRType_String)
        return InliningStatus_NotInlined;

    callInfo.setImplicitlyUsedUnchecked();

    MInstruction* cte;
    if (callInfo.getArg(0)->type() == MIRType_String) {
        cte = MStringReplace::New(alloc(), callInfo.thisArg(), callInfo.getArg(0),
                                  callInfo.getArg(1));
    } else {
        cte = MRegExpReplace::New(alloc(), callInfo.thisArg(), callInfo.getArg(0),
                                  callInfo.getArg(1));
    }
    current->add(cte);
    current->push(cte);
    if (cte->isEffectful() && !resumeAfter(cte))
        return InliningStatus_Error;
    return InliningStatus_Inlined;
}

IonBuilder::InliningStatus
IonBuilder::inlineSubstringKernel(CallInfo& callInfo)
{
    MOZ_ASSERT(callInfo.argc() == 3);
    MOZ_ASSERT(!callInfo.constructing());

    // Return: String.
    if (getInlineReturnType() != MIRType_String)
        return InliningStatus_NotInlined;

    // Arg 0: String.
    if (callInfo.getArg(0)->type() != MIRType_String)
        return InliningStatus_NotInlined;

    // Arg 1: Int.
    if (callInfo.getArg(1)->type() != MIRType_Int32)
        return InliningStatus_NotInlined;

    // Arg 2: Int.
    if (callInfo.getArg(2)->type() != MIRType_Int32)
        return InliningStatus_NotInlined;

    callInfo.setImplicitlyUsedUnchecked();

    MSubstr* substr = MSubstr::New(alloc(), callInfo.getArg(0), callInfo.getArg(1),
                                            callInfo.getArg(2));
    current->add(substr);
    current->push(substr);

    return InliningStatus_Inlined;
}

IonBuilder::InliningStatus
IonBuilder::inlineObjectCreate(CallInfo& callInfo)
{
    if (callInfo.argc() != 1 || callInfo.constructing())
        return InliningStatus_NotInlined;

    JSObject* templateObject = inspector->getTemplateObjectForNative(pc, obj_create);
    if (!templateObject)
        return InliningStatus_NotInlined;

    MOZ_ASSERT(templateObject->is<PlainObject>());
    MOZ_ASSERT(!templateObject->isSingleton());

    // Ensure the argument matches the template object's prototype.
    MDefinition* arg = callInfo.getArg(0);
    if (JSObject* proto = templateObject->getProto()) {
        if (IsInsideNursery(proto))
            return InliningStatus_NotInlined;

        TemporaryTypeSet* types = arg->resultTypeSet();
        if (!types || types->maybeSingleton() != proto)
            return InliningStatus_NotInlined;

        MOZ_ASSERT(types->getKnownMIRType() == MIRType_Object);
    } else {
        if (arg->type() != MIRType_Null)
            return InliningStatus_NotInlined;
    }

    callInfo.setImplicitlyUsedUnchecked();

    MConstant* templateConst = MConstant::NewConstraintlessObject(alloc(), templateObject);
    current->add(templateConst);
    MNewObject* ins = MNewObject::New(alloc(), constraints(), templateConst,
                                      templateObject->group()->initialHeap(constraints()),
                                      MNewObject::ObjectCreate);
    current->add(ins);
    current->push(ins);
    if (!resumeAfter(ins))
        return InliningStatus_Error;

    return InliningStatus_Inlined;
}

IonBuilder::InliningStatus
IonBuilder::inlineDefineDataProperty(CallInfo& callInfo)
{
    MOZ_ASSERT(!callInfo.constructing());

    // Only handle definitions of plain data properties.
    if (callInfo.argc() != 3)
        return InliningStatus_NotInlined;

    MDefinition* obj = convertUnboxedObjects(callInfo.getArg(0));
    MDefinition* id = callInfo.getArg(1);
    MDefinition* value = callInfo.getArg(2);

    if (ElementAccessHasExtraIndexedProperty(this, obj))
        return InliningStatus_NotInlined;

    // setElemTryDense will push the value as the result of the define instead
    // of |undefined|, but this is fine if the rval is ignored (as it should be
    // in self hosted code.)
    MOZ_ASSERT(*GetNextPc(pc) == JSOP_POP);

    bool emitted = false;
    if (!setElemTryDense(&emitted, obj, id, value, /* writeHole = */ true))
        return InliningStatus_Error;
    if (!emitted)
        return InliningStatus_NotInlined;

    callInfo.setImplicitlyUsedUnchecked();
    return InliningStatus_Inlined;
}

IonBuilder::InliningStatus
IonBuilder::inlineHasClass(CallInfo& callInfo,
                           const Class* clasp1, const Class* clasp2,
                           const Class* clasp3, const Class* clasp4)
{
    if (callInfo.constructing() || callInfo.argc() != 1) {
        trackOptimizationOutcome(TrackedOutcome::CantInlineNativeBadForm);
        return InliningStatus_NotInlined;
    }

    if (callInfo.getArg(0)->type() != MIRType_Object)
        return InliningStatus_NotInlined;
    if (getInlineReturnType() != MIRType_Boolean)
        return InliningStatus_NotInlined;

    TemporaryTypeSet* types = callInfo.getArg(0)->resultTypeSet();
    const Class* knownClass = types ? types->getKnownClass(constraints()) : nullptr;
    if (knownClass) {
        pushConstant(BooleanValue(knownClass == clasp1 ||
                                  knownClass == clasp2 ||
                                  knownClass == clasp3 ||
                                  knownClass == clasp4));
    } else {
        MHasClass* hasClass1 = MHasClass::New(alloc(), callInfo.getArg(0), clasp1);
        current->add(hasClass1);

        if (!clasp2 && !clasp3 && !clasp4) {
            current->push(hasClass1);
        } else {
            const Class* remaining[] = { clasp2, clasp3, clasp4 };
            MDefinition* last = hasClass1;
            for (size_t i = 0; i < ArrayLength(remaining); i++) {
                MHasClass* hasClass = MHasClass::New(alloc(), callInfo.getArg(0), remaining[i]);
                current->add(hasClass);
                MBitOr* either = MBitOr::New(alloc(), last, hasClass);
                either->infer(inspector, pc);
                current->add(either);
                last = either;
            }

            // Convert to bool with the '!!' idiom
            MNot* resultInverted = MNot::New(alloc(), last, constraints());
            current->add(resultInverted);
            MNot* result = MNot::New(alloc(), resultInverted, constraints());
            current->add(result);
            current->push(result);
        }
    }

    callInfo.setImplicitlyUsedUnchecked();
    return InliningStatus_Inlined;
}

IonBuilder::InliningStatus
IonBuilder::inlineIsTypedArrayHelper(CallInfo& callInfo, WrappingBehavior wrappingBehavior)
{
    MOZ_ASSERT(!callInfo.constructing());
    MOZ_ASSERT(callInfo.argc() == 1);

    if (callInfo.getArg(0)->type() != MIRType_Object)
        return InliningStatus_NotInlined;
    if (getInlineReturnType() != MIRType_Boolean)
        return InliningStatus_NotInlined;

    // The test is elaborate: in-line only if there is exact
    // information.

    TemporaryTypeSet* types = callInfo.getArg(0)->resultTypeSet();
    if (!types)
        return InliningStatus_NotInlined;

    bool result = false;
    switch (types->forAllClasses(constraints(), IsTypedArrayClass)) {
      case TemporaryTypeSet::ForAllResult::ALL_FALSE:
      case TemporaryTypeSet::ForAllResult::EMPTY: {
        // Wrapped typed arrays won't appear to be typed arrays per a
        // |forAllClasses| query.  If wrapped typed arrays are to be considered
        // typed arrays, a negative answer is not conclusive.  Don't inline in
        // that case.
        if (wrappingBehavior == AllowWrappedTypedArrays)
            return InliningStatus_NotInlined;

        result = false;
        break;
      }

      case TemporaryTypeSet::ForAllResult::ALL_TRUE:
        result = true;
        break;

      case TemporaryTypeSet::ForAllResult::MIXED:
        return InliningStatus_NotInlined;
    }

    pushConstant(BooleanValue(result));

    callInfo.setImplicitlyUsedUnchecked();
    return InliningStatus_Inlined;
}

IonBuilder::InliningStatus
IonBuilder::inlineIsTypedArray(CallInfo& callInfo)
{
    return inlineIsTypedArrayHelper(callInfo, RejectWrappedTypedArrays);
}

IonBuilder::InliningStatus
IonBuilder::inlineIsPossiblyWrappedTypedArray(CallInfo& callInfo)
{
    return inlineIsTypedArrayHelper(callInfo, AllowWrappedTypedArrays);
}

static bool
IsTypedArrayObject(CompilerConstraintList* constraints, MDefinition* def)
{
    MOZ_ASSERT(def->type() == MIRType_Object);

    TemporaryTypeSet* types = def->resultTypeSet();
    if (!types)
        return false;

    return types->forAllClasses(constraints, IsTypedArrayClass) ==
           TemporaryTypeSet::ForAllResult::ALL_TRUE;
}

IonBuilder::InliningStatus
IonBuilder::inlineTypedArrayLength(CallInfo& callInfo)
{
    MOZ_ASSERT(!callInfo.constructing());
    MOZ_ASSERT(callInfo.argc() == 1);
    if (callInfo.getArg(0)->type() != MIRType_Object)
        return InliningStatus_NotInlined;
    if (getInlineReturnType() != MIRType_Int32)
        return InliningStatus_NotInlined;

    // Note that the argument we see here is not necessarily a typed array.
    // If it's not, this call should be unreachable though.
    if (!IsTypedArrayObject(constraints(), callInfo.getArg(0)))
        return InliningStatus_NotInlined;

    MInstruction* length = addTypedArrayLength(callInfo.getArg(0));
    current->push(length);

    callInfo.setImplicitlyUsedUnchecked();
    return InliningStatus_Inlined;
}

IonBuilder::InliningStatus
IonBuilder::inlineSetDisjointTypedElements(CallInfo& callInfo)
{
    MOZ_ASSERT(!callInfo.constructing());
    MOZ_ASSERT(callInfo.argc() == 3);

    // Initial argument requirements.

    MDefinition* target = callInfo.getArg(0);
    if (target->type() != MIRType_Object)
        return InliningStatus_NotInlined;

    if (getInlineReturnType() != MIRType_Undefined)
        return InliningStatus_NotInlined;

    MDefinition* targetOffset = callInfo.getArg(1);
    MOZ_ASSERT(targetOffset->type() == MIRType_Int32);

    MDefinition* sourceTypedArray = callInfo.getArg(2);
    if (sourceTypedArray->type() != MIRType_Object)
        return InliningStatus_NotInlined;

    // Only attempt to optimize if |target| and |sourceTypedArray| are both
    // definitely typed arrays.  (The former always is.  The latter is not,
    // necessarily, because of wrappers.)
    if (!IsTypedArrayObject(constraints(), target) ||
        !IsTypedArrayObject(constraints(), sourceTypedArray))
    {
        return InliningStatus_NotInlined;
    }

    auto sets = MSetDisjointTypedElements::New(alloc(), target, targetOffset, sourceTypedArray);
    current->add(sets);

    pushConstant(UndefinedValue());

    if (!resumeAfter(sets))
        return InliningStatus_Error;

    callInfo.setImplicitlyUsedUnchecked();
    return InliningStatus_Inlined;
}

IonBuilder::InliningStatus
IonBuilder::inlineObjectIsTypeDescr(CallInfo& callInfo)
{
    if (callInfo.constructing() || callInfo.argc() != 1) {
        trackOptimizationOutcome(TrackedOutcome::CantInlineNativeBadForm);
        return InliningStatus_NotInlined;
    }

    if (callInfo.getArg(0)->type() != MIRType_Object)
        return InliningStatus_NotInlined;
    if (getInlineReturnType() != MIRType_Boolean)
        return InliningStatus_NotInlined;

    // The test is elaborate: in-line only if there is exact
    // information.

    TemporaryTypeSet* types = callInfo.getArg(0)->resultTypeSet();
    if (!types)
        return InliningStatus_NotInlined;

    bool result = false;
    switch (types->forAllClasses(constraints(), IsTypeDescrClass)) {
    case TemporaryTypeSet::ForAllResult::ALL_FALSE:
    case TemporaryTypeSet::ForAllResult::EMPTY:
        result = false;
        break;
    case TemporaryTypeSet::ForAllResult::ALL_TRUE:
        result = true;
        break;
    case TemporaryTypeSet::ForAllResult::MIXED:
        return InliningStatus_NotInlined;
    }

    pushConstant(BooleanValue(result));

    callInfo.setImplicitlyUsedUnchecked();
    return InliningStatus_Inlined;
}

IonBuilder::InliningStatus
IonBuilder::inlineSetTypedObjectOffset(CallInfo& callInfo)
{
    if (callInfo.argc() != 2 || callInfo.constructing()) {
        trackOptimizationOutcome(TrackedOutcome::CantInlineNativeBadForm);
        return InliningStatus_NotInlined;
    }

    MDefinition* typedObj = callInfo.getArg(0);
    MDefinition* offset = callInfo.getArg(1);

    // Return type should be undefined or something wacky is going on.
    if (getInlineReturnType() != MIRType_Undefined)
        return InliningStatus_NotInlined;

    // Check typedObj is a, well, typed object. Go ahead and use TI
    // data. If this check should fail, that is almost certainly a bug
    // in self-hosted code -- either because it's not being careful
    // with TI or because of something else -- but we'll just let it
    // fall through to the SetTypedObjectOffset intrinsic in such
    // cases.
    TemporaryTypeSet* types = typedObj->resultTypeSet();
    if (typedObj->type() != MIRType_Object || !types)
        return InliningStatus_NotInlined;
    switch (types->forAllClasses(constraints(), IsTypedObjectClass)) {
      case TemporaryTypeSet::ForAllResult::ALL_FALSE:
      case TemporaryTypeSet::ForAllResult::EMPTY:
      case TemporaryTypeSet::ForAllResult::MIXED:
        return InliningStatus_NotInlined;
      case TemporaryTypeSet::ForAllResult::ALL_TRUE:
        break;
    }

    // Check type of offset argument is an integer.
    if (offset->type() != MIRType_Int32)
        return InliningStatus_NotInlined;

    callInfo.setImplicitlyUsedUnchecked();
    MInstruction* ins = MSetTypedObjectOffset::New(alloc(), typedObj, offset);
    current->add(ins);
    current->push(ins);
    return InliningStatus_Inlined;
}

IonBuilder::InliningStatus
IonBuilder::inlineUnsafeSetReservedSlot(CallInfo& callInfo)
{
    if (callInfo.argc() != 3 || callInfo.constructing()) {
        trackOptimizationOutcome(TrackedOutcome::CantInlineNativeBadForm);
        return InliningStatus_NotInlined;
    }
    if (getInlineReturnType() != MIRType_Undefined)
        return InliningStatus_NotInlined;
    if (callInfo.getArg(0)->type() != MIRType_Object)
        return InliningStatus_NotInlined;
    if (callInfo.getArg(1)->type() != MIRType_Int32)
        return InliningStatus_NotInlined;

    // Don't inline if we don't have a constant slot.
    MDefinition* arg = callInfo.getArg(1);
    if (!arg->isConstantValue())
        return InliningStatus_NotInlined;
    uint32_t slot = arg->constantValue().toPrivateUint32();

    callInfo.setImplicitlyUsedUnchecked();

    MStoreFixedSlot* store = MStoreFixedSlot::New(alloc(), callInfo.getArg(0), slot, callInfo.getArg(2));
    current->add(store);
    current->push(store);

    if (NeedsPostBarrier(callInfo.getArg(2)))
        current->add(MPostWriteBarrier::New(alloc(), callInfo.getArg(0), callInfo.getArg(2)));

    return InliningStatus_Inlined;
}

IonBuilder::InliningStatus
IonBuilder::inlineUnsafeGetReservedSlot(CallInfo& callInfo, MIRType knownValueType)
{
    if (callInfo.argc() != 2 || callInfo.constructing()) {
        trackOptimizationOutcome(TrackedOutcome::CantInlineNativeBadForm);
        return InliningStatus_NotInlined;
    }
    if (callInfo.getArg(0)->type() != MIRType_Object)
        return InliningStatus_NotInlined;
    if (callInfo.getArg(1)->type() != MIRType_Int32)
        return InliningStatus_NotInlined;

    // Don't inline if we don't have a constant slot.
    MDefinition* arg = callInfo.getArg(1);
    if (!arg->isConstantValue())
        return InliningStatus_NotInlined;
    uint32_t slot = arg->constantValue().toPrivateUint32();

    callInfo.setImplicitlyUsedUnchecked();

    MLoadFixedSlot* load = MLoadFixedSlot::New(alloc(), callInfo.getArg(0), slot);
    current->add(load);
    current->push(load);
    if (knownValueType != MIRType_Value) {
        // We know what type we have in this slot.  Assert that this is in fact
        // what we've seen coming from this slot in the past, then tell the
        // MLoadFixedSlot about its result type.  That will make us do an
        // infallible unbox as part of the slot load and then we'll barrier on
        // the unbox result.  That way the type barrier code won't end up doing
        // MIRType checks and conditional unboxing.
        MOZ_ASSERT_IF(!getInlineReturnTypeSet()->empty(),
                      getInlineReturnType() == knownValueType);
        load->setResultType(knownValueType);
    }

    // We don't track reserved slot types, so always emit a barrier.
    if (!pushTypeBarrier(load, getInlineReturnTypeSet(), BarrierKind::TypeSet))
        return InliningStatus_Error;

    return InliningStatus_Inlined;
}

IonBuilder::InliningStatus
IonBuilder::inlineIsCallable(CallInfo& callInfo)
{
    if (callInfo.argc() != 1 || callInfo.constructing()) {
        trackOptimizationOutcome(TrackedOutcome::CantInlineNativeBadForm);
        return InliningStatus_NotInlined;
    }

    if (getInlineReturnType() != MIRType_Boolean)
        return InliningStatus_NotInlined;
    if (callInfo.getArg(0)->type() != MIRType_Object)
        return InliningStatus_NotInlined;

    // Try inlining with constant true/false: only objects may be callable at
    // all, and if we know the class check if it is callable.
    bool isCallableKnown = false;
    bool isCallableConstant;
    if (callInfo.getArg(0)->type() != MIRType_Object) {
        isCallableKnown = true;
        isCallableConstant = false;
    } else {
        TemporaryTypeSet* types = callInfo.getArg(0)->resultTypeSet();
        const Class* clasp = types ? types->getKnownClass(constraints()) : nullptr;
        if (clasp && !clasp->isProxy()) {
            isCallableKnown = true;
            isCallableConstant = clasp->nonProxyCallable();
        }
    }

    callInfo.setImplicitlyUsedUnchecked();

    if (isCallableKnown) {
        MConstant* constant = MConstant::New(alloc(), BooleanValue(isCallableConstant));
        current->add(constant);
        current->push(constant);
        return InliningStatus_Inlined;
    }

    MIsCallable* isCallable = MIsCallable::New(alloc(), callInfo.getArg(0));
    current->add(isCallable);
    current->push(isCallable);

    return InliningStatus_Inlined;
}

IonBuilder::InliningStatus
IonBuilder::inlineIsObject(CallInfo& callInfo)
{
    if (callInfo.argc() != 1 || callInfo.constructing()) {
        trackOptimizationOutcome(TrackedOutcome::CantInlineNativeBadForm);
        return InliningStatus_NotInlined;
    }
    if (getInlineReturnType() != MIRType_Boolean)
        return InliningStatus_NotInlined;

    callInfo.setImplicitlyUsedUnchecked();
    if (callInfo.getArg(0)->type() == MIRType_Object) {
        pushConstant(BooleanValue(true));
    } else {
        MIsObject* isObject = MIsObject::New(alloc(), callInfo.getArg(0));
        current->add(isObject);
        current->push(isObject);
    }
    return InliningStatus_Inlined;
}

IonBuilder::InliningStatus
IonBuilder::inlineToObject(CallInfo& callInfo)
{
    if (callInfo.argc() != 1 || callInfo.constructing()) {
        trackOptimizationOutcome(TrackedOutcome::CantInlineNativeBadForm);
        return InliningStatus_NotInlined;
    }

    // If we know the input type is an object, nop ToObject.
    if (getInlineReturnType() != MIRType_Object)
        return InliningStatus_NotInlined;
    if (callInfo.getArg(0)->type() != MIRType_Object)
        return InliningStatus_NotInlined;

    callInfo.setImplicitlyUsedUnchecked();
    MDefinition* object = callInfo.getArg(0);

    current->push(object);
    return InliningStatus_Inlined;
}

IonBuilder::InliningStatus
IonBuilder::inlineToInteger(CallInfo& callInfo)
{
    if (callInfo.argc() != 1 || callInfo.constructing()) {
        trackOptimizationOutcome(TrackedOutcome::CantInlineNativeBadForm);
        return InliningStatus_NotInlined;
    }

    MDefinition* input = callInfo.getArg(0);

    // Only optimize cases where input contains only number, null or boolean
    if (input->mightBeType(MIRType_Object) ||
        input->mightBeType(MIRType_String) ||
        input->mightBeType(MIRType_Symbol) ||
        input->mightBeType(MIRType_Undefined) ||
        input->mightBeMagicType())
    {
        return InliningStatus_NotInlined;
    }

    MOZ_ASSERT(input->type() == MIRType_Value || input->type() == MIRType_Null ||
               input->type() == MIRType_Boolean || IsNumberType(input->type()));

    // Only optimize cases where output is int32
    if (getInlineReturnType() != MIRType_Int32)
        return InliningStatus_NotInlined;

    callInfo.setImplicitlyUsedUnchecked();

    MToInt32* toInt32 = MToInt32::New(alloc(), callInfo.getArg(0));
    current->add(toInt32);
    current->push(toInt32);
    return InliningStatus_Inlined;
}

IonBuilder::InliningStatus
IonBuilder::inlineToString(CallInfo& callInfo)
{
    if (callInfo.argc() != 1 || callInfo.constructing())
        return InliningStatus_NotInlined;

    if (getInlineReturnType() != MIRType_String)
        return InliningStatus_NotInlined;

    callInfo.setImplicitlyUsedUnchecked();
    MToString* toString = MToString::New(alloc(), callInfo.getArg(0));
    current->add(toString);
    current->push(toString);
    return InliningStatus_Inlined;
}

IonBuilder::InliningStatus
IonBuilder::inlineBailout(CallInfo& callInfo)
{
    callInfo.setImplicitlyUsedUnchecked();

    current->add(MBail::New(alloc()));

    MConstant* undefined = MConstant::New(alloc(), UndefinedValue());
    current->add(undefined);
    current->push(undefined);
    return InliningStatus_Inlined;
}

IonBuilder::InliningStatus
IonBuilder::inlineAssertFloat32(CallInfo& callInfo)
{
    if (callInfo.argc() != 2)
        return InliningStatus_NotInlined;

    MDefinition* secondArg = callInfo.getArg(1);

    MOZ_ASSERT(secondArg->type() == MIRType_Boolean);
    MOZ_ASSERT(secondArg->isConstantValue());

    bool mustBeFloat32 = secondArg->constantValue().toBoolean();
    current->add(MAssertFloat32::New(alloc(), callInfo.getArg(0), mustBeFloat32));

    MConstant* undefined = MConstant::New(alloc(), UndefinedValue());
    current->add(undefined);
    current->push(undefined);
    callInfo.setImplicitlyUsedUnchecked();
    return InliningStatus_Inlined;
}

IonBuilder::InliningStatus
IonBuilder::inlineAssertRecoveredOnBailout(CallInfo& callInfo)
{
    if (callInfo.argc() != 2)
        return InliningStatus_NotInlined;

    if (JitOptions.checkRangeAnalysis) {
        // If we are checking the range of all instructions, then the guards
        // inserted by Range Analysis prevent the use of recover
        // instruction. Thus, we just disable these checks.
        current->push(constant(UndefinedValue()));
        callInfo.setImplicitlyUsedUnchecked();
        return InliningStatus_Inlined;
    }

    MDefinition* secondArg = callInfo.getArg(1);

    MOZ_ASSERT(secondArg->type() == MIRType_Boolean);
    MOZ_ASSERT(secondArg->isConstantValue());

    bool mustBeRecovered = secondArg->constantValue().toBoolean();
    MAssertRecoveredOnBailout* assert =
        MAssertRecoveredOnBailout::New(alloc(), callInfo.getArg(0), mustBeRecovered);
    current->add(assert);
    current->push(assert);

    // Create an instruction sequence which implies that the argument of the
    // assertRecoveredOnBailout function would be encoded at least in one
    // Snapshot.
    MNop* nop = MNop::New(alloc());
    current->add(nop);
    if (!resumeAfter(nop))
        return InliningStatus_Error;
    current->add(MEncodeSnapshot::New(alloc()));

    current->pop();
    current->push(constant(UndefinedValue()));
    callInfo.setImplicitlyUsedUnchecked();
    return InliningStatus_Inlined;
}

IonBuilder::InliningStatus
IonBuilder::inlineBoundFunction(CallInfo& nativeCallInfo, JSFunction* target)
{
    trackOptimizationOutcome(TrackedOutcome::CantInlineBound);

    if (!target->getBoundFunctionTarget()->is<JSFunction>())
        return InliningStatus_NotInlined;

    JSFunction* scriptedTarget = &(target->getBoundFunctionTarget()->as<JSFunction>());

    // Don't optimize if we're constructing and the callee is not a
    // constructor, so that CallKnown does not have to handle this case
    // (it should always throw).
    if (nativeCallInfo.constructing() && !scriptedTarget->isConstructor())
        return InliningStatus_NotInlined;

    if (nativeCallInfo.constructing() && nativeCallInfo.getNewTarget() != nativeCallInfo.fun())
        return InliningStatus_NotInlined;

    if (gc::IsInsideNursery(scriptedTarget))
        return InliningStatus_NotInlined;

    for (size_t i = 0; i < target->getBoundFunctionArgumentCount(); i++) {
        const Value val = target->getBoundFunctionArgument(i);
        if (val.isObject() && gc::IsInsideNursery(&val.toObject()))
            return InliningStatus_NotInlined;
        if (val.isString() && !val.toString()->isAtom())
            return InliningStatus_NotInlined;
    }

    const Value thisVal = target->getBoundFunctionThis();
    if (thisVal.isObject() && gc::IsInsideNursery(&thisVal.toObject()))
        return InliningStatus_NotInlined;
    if (thisVal.isString() && !thisVal.toString()->isAtom())
        return InliningStatus_NotInlined;

    size_t argc = target->getBoundFunctionArgumentCount() + nativeCallInfo.argc();
    if (argc > ARGS_LENGTH_MAX)
        return InliningStatus_NotInlined;

    nativeCallInfo.thisArg()->setImplicitlyUsedUnchecked();

    CallInfo callInfo(alloc(), nativeCallInfo.constructing());
    callInfo.setFun(constant(ObjectValue(*scriptedTarget)));
    callInfo.setThis(constant(thisVal));

    if (!callInfo.argv().reserve(argc))
        return InliningStatus_Error;

    for (size_t i = 0; i < target->getBoundFunctionArgumentCount(); i++) {
        MConstant* argConst = constant(target->getBoundFunctionArgument(i));
        callInfo.argv().infallibleAppend(argConst);
    }
    for (size_t i = 0; i < nativeCallInfo.argc(); i++)
        callInfo.argv().infallibleAppend(nativeCallInfo.getArg(i));

    // We only inline when it was not a super-call, so just set the newTarget
    // to be the target function, per spec.
    if (nativeCallInfo.constructing())
        callInfo.setNewTarget(callInfo.fun());

    if (!makeCall(scriptedTarget, callInfo))
        return InliningStatus_Error;

    return InliningStatus_Inlined;
}

IonBuilder::InliningStatus
IonBuilder::inlineAtomicsCompareExchange(CallInfo& callInfo)
{
    if (callInfo.argc() != 4 || callInfo.constructing()) {
        trackOptimizationOutcome(TrackedOutcome::CantInlineNativeBadForm);
        return InliningStatus_NotInlined;
    }

    // These guards are desirable here and in subsequent atomics to
    // avoid bad bailouts with MTruncateToInt32, see https://bugzilla.mozilla.org/show_bug.cgi?id=1141986#c20.
    MDefinition* oldval = callInfo.getArg(2);
    if (oldval->mightBeType(MIRType_Object) || oldval->mightBeType(MIRType_Symbol))
        return InliningStatus_NotInlined;

    MDefinition* newval = callInfo.getArg(3);
    if (newval->mightBeType(MIRType_Object) || newval->mightBeType(MIRType_Symbol))
        return InliningStatus_NotInlined;

    Scalar::Type arrayType;
    bool requiresCheck = false;
    if (!atomicsMeetsPreconditions(callInfo, &arrayType, &requiresCheck))
        return InliningStatus_NotInlined;

    callInfo.setImplicitlyUsedUnchecked();

    MInstruction* elements;
    MDefinition* index;
    atomicsCheckBounds(callInfo, &elements, &index);

    if (requiresCheck)
        addSharedTypedArrayGuard(callInfo.getArg(0));

    MCompareExchangeTypedArrayElement* cas =
        MCompareExchangeTypedArrayElement::New(alloc(), elements, index, arrayType, oldval, newval);
    cas->setResultType(getInlineReturnType());
    current->add(cas);
    current->push(cas);

    if (!resumeAfter(cas))
        return InliningStatus_Error;

    return InliningStatus_Inlined;
}

IonBuilder::InliningStatus
IonBuilder::inlineAtomicsExchange(CallInfo& callInfo)
{
    if (callInfo.argc() != 3 || callInfo.constructing()) {
        trackOptimizationOutcome(TrackedOutcome::CantInlineNativeBadForm);
        return InliningStatus_NotInlined;
    }

    MDefinition* value = callInfo.getArg(2);
    if (value->mightBeType(MIRType_Object) || value->mightBeType(MIRType_Symbol))
        return InliningStatus_NotInlined;

    Scalar::Type arrayType;
    bool requiresCheck = false;
    if (!atomicsMeetsPreconditions(callInfo, &arrayType, &requiresCheck))
        return InliningStatus_NotInlined;

    callInfo.setImplicitlyUsedUnchecked();

    MInstruction* elements;
    MDefinition* index;
    atomicsCheckBounds(callInfo, &elements, &index);

    if (requiresCheck)
        addSharedTypedArrayGuard(callInfo.getArg(0));

    MInstruction* exchange =
        MAtomicExchangeTypedArrayElement::New(alloc(), elements, index, value, arrayType);
    exchange->setResultType(getInlineReturnType());
    current->add(exchange);
    current->push(exchange);

    if (!resumeAfter(exchange))
        return InliningStatus_Error;

    return InliningStatus_Inlined;
}

IonBuilder::InliningStatus
IonBuilder::inlineAtomicsLoad(CallInfo& callInfo)
{
    if (callInfo.argc() != 2 || callInfo.constructing()) {
        trackOptimizationOutcome(TrackedOutcome::CantInlineNativeBadForm);
        return InliningStatus_NotInlined;
    }

    Scalar::Type arrayType;
    bool requiresCheck = false;
    if (!atomicsMeetsPreconditions(callInfo, &arrayType, &requiresCheck))
        return InliningStatus_NotInlined;

    callInfo.setImplicitlyUsedUnchecked();

    MInstruction* elements;
    MDefinition* index;
    atomicsCheckBounds(callInfo, &elements, &index);

    if (requiresCheck)
        addSharedTypedArrayGuard(callInfo.getArg(0));

    MLoadUnboxedScalar* load =
        MLoadUnboxedScalar::New(alloc(), elements, index, arrayType,
                                DoesRequireMemoryBarrier);
    load->setResultType(getInlineReturnType());
    current->add(load);
    current->push(load);

    // Loads are considered effectful (they execute a memory barrier).
    if (!resumeAfter(load))
        return InliningStatus_Error;

    return InliningStatus_Inlined;
}

IonBuilder::InliningStatus
IonBuilder::inlineAtomicsStore(CallInfo& callInfo)
{
    if (callInfo.argc() != 3 || callInfo.constructing()) {
        trackOptimizationOutcome(TrackedOutcome::CantInlineNativeBadForm);
        return InliningStatus_NotInlined;
    }

    MDefinition* value = callInfo.getArg(2);
    if (value->mightBeType(MIRType_Object) || value->mightBeType(MIRType_Symbol))
        return InliningStatus_NotInlined;

    Scalar::Type arrayType;
    bool requiresCheck = false;
    if (!atomicsMeetsPreconditions(callInfo, &arrayType, &requiresCheck, DontCheckAtomicResult))
        return InliningStatus_NotInlined;

    callInfo.setImplicitlyUsedUnchecked();

    MInstruction* elements;
    MDefinition* index;
    atomicsCheckBounds(callInfo, &elements, &index);

    if (requiresCheck)
        addSharedTypedArrayGuard(callInfo.getArg(0));

    MDefinition* toWrite = value;
    if (value->type() != MIRType_Int32) {
        toWrite = MTruncateToInt32::New(alloc(), value);
        current->add(toWrite->toInstruction());
    }
    MStoreUnboxedScalar* store =
        MStoreUnboxedScalar::New(alloc(), elements, index, toWrite, arrayType,
                                 MStoreUnboxedScalar::TruncateInput, DoesRequireMemoryBarrier);
    current->add(store);
    current->push(value);

    if (!resumeAfter(store))
        return InliningStatus_Error;

    return InliningStatus_Inlined;
}

IonBuilder::InliningStatus
IonBuilder::inlineAtomicsFence(CallInfo& callInfo)
{
    if (callInfo.argc() != 0 || callInfo.constructing()) {
        trackOptimizationOutcome(TrackedOutcome::CantInlineNativeBadForm);
        return InliningStatus_NotInlined;
    }

    if (!JitSupportsAtomics())
        return InliningStatus_NotInlined;

    callInfo.setImplicitlyUsedUnchecked();

    MMemoryBarrier* fence = MMemoryBarrier::New(alloc());
    current->add(fence);
    pushConstant(UndefinedValue());

    // Fences are considered effectful (they execute a memory barrier).
    if (!resumeAfter(fence))
        return InliningStatus_Error;

    return InliningStatus_Inlined;
}

IonBuilder::InliningStatus
IonBuilder::inlineAtomicsBinop(CallInfo& callInfo, InlinableNative target)
{
    if (callInfo.argc() != 3 || callInfo.constructing()) {
        trackOptimizationOutcome(TrackedOutcome::CantInlineNativeBadForm);
        return InliningStatus_NotInlined;
    }

    MDefinition* value = callInfo.getArg(2);
    if (value->mightBeType(MIRType_Object) || value->mightBeType(MIRType_Symbol))
        return InliningStatus_NotInlined;

    Scalar::Type arrayType;
    bool requiresCheck = false;
    if (!atomicsMeetsPreconditions(callInfo, &arrayType, &requiresCheck))
        return InliningStatus_NotInlined;

    callInfo.setImplicitlyUsedUnchecked();

    if (requiresCheck)
        addSharedTypedArrayGuard(callInfo.getArg(0));

    MInstruction* elements;
    MDefinition* index;
    atomicsCheckBounds(callInfo, &elements, &index);

    AtomicOp k = AtomicFetchAddOp;
    switch (target) {
      case InlinableNative::AtomicsAdd:
        k = AtomicFetchAddOp;
        break;
      case InlinableNative::AtomicsSub:
        k = AtomicFetchSubOp;
        break;
      case InlinableNative::AtomicsAnd:
        k = AtomicFetchAndOp;
        break;
      case InlinableNative::AtomicsOr:
        k = AtomicFetchOrOp;
        break;
      case InlinableNative::AtomicsXor:
        k = AtomicFetchXorOp;
        break;
      default:
        MOZ_CRASH("Bad atomic operation");
    }

    MAtomicTypedArrayElementBinop* binop =
        MAtomicTypedArrayElementBinop::New(alloc(), k, elements, index, arrayType, value);
    binop->setResultType(getInlineReturnType());
    current->add(binop);
    current->push(binop);

    if (!resumeAfter(binop))
        return InliningStatus_Error;

    return InliningStatus_Inlined;
}

IonBuilder::InliningStatus
IonBuilder::inlineAtomicsIsLockFree(CallInfo& callInfo)
{
    if (callInfo.argc() != 1 || callInfo.constructing()) {
        trackOptimizationOutcome(TrackedOutcome::CantInlineNativeBadForm);
        return InliningStatus_NotInlined;
    }

    callInfo.setImplicitlyUsedUnchecked();

    MAtomicIsLockFree* ilf =
        MAtomicIsLockFree::New(alloc(), callInfo.getArg(0));
    current->add(ilf);
    current->push(ilf);

    return InliningStatus_Inlined;
}

bool
IonBuilder::atomicsMeetsPreconditions(CallInfo& callInfo, Scalar::Type* arrayType,
                                      bool* requiresTagCheck, AtomicCheckResult checkResult)
{
    if (!JitSupportsAtomics())
        return false;

    if (callInfo.getArg(0)->type() != MIRType_Object)
        return false;

    if (callInfo.getArg(1)->type() != MIRType_Int32)
        return false;

    // Ensure that the first argument is a TypedArray that maps shared
    // memory.
    //
    // Then check both that the element type is something we can
    // optimize and that the return type is suitable for that element
    // type.

    TemporaryTypeSet* arg0Types = callInfo.getArg(0)->resultTypeSet();
    if (!arg0Types)
        return false;

    TemporaryTypeSet::TypedArraySharedness sharedness;
    *arrayType = arg0Types->getTypedArrayType(constraints(), &sharedness);
    *requiresTagCheck = sharedness != TemporaryTypeSet::KnownShared;
    switch (*arrayType) {
      case Scalar::Int8:
      case Scalar::Uint8:
      case Scalar::Int16:
      case Scalar::Uint16:
      case Scalar::Int32:
        return checkResult == DontCheckAtomicResult || getInlineReturnType() == MIRType_Int32;
      case Scalar::Uint32:
        // Bug 1077305: it would be attractive to allow inlining even
        // if the inline return type is Int32, which it will frequently
        // be.
        return checkResult == DontCheckAtomicResult || getInlineReturnType() == MIRType_Double;
      default:
        // Excludes floating types and Uint8Clamped.
        return false;
    }
}

void
IonBuilder::atomicsCheckBounds(CallInfo& callInfo, MInstruction** elements, MDefinition** index)
{
    // Perform bounds checking and extract the elements vector.
    MDefinition* obj = callInfo.getArg(0);
    MInstruction* length = nullptr;
    *index = callInfo.getArg(1);
    *elements = nullptr;
    addTypedArrayLengthAndData(obj, DoBoundsCheck, index, &length, elements);
}

IonBuilder::InliningStatus
IonBuilder::inlineIsConstructing(CallInfo& callInfo)
{
    MOZ_ASSERT(!callInfo.constructing());
    MOZ_ASSERT(callInfo.argc() == 0);
    MOZ_ASSERT(script()->functionNonDelazifying(),
               "isConstructing() should only be called in function scripts");

    if (getInlineReturnType() != MIRType_Boolean)
        return InliningStatus_NotInlined;

    callInfo.setImplicitlyUsedUnchecked();

    if (inliningDepth_ == 0) {
        MInstruction* ins = MIsConstructing::New(alloc());
        current->add(ins);
        current->push(ins);
        return InliningStatus_Inlined;
    }

    bool constructing = inlineCallInfo_->constructing();
    pushConstant(BooleanValue(constructing));
    return InliningStatus_Inlined;
}

IonBuilder::InliningStatus
IonBuilder::inlineConstructTypedObject(CallInfo& callInfo, TypeDescr* descr)
{
    // Only inline default constructors for now.
    if (callInfo.argc() != 0) {
        trackOptimizationOutcome(TrackedOutcome::CantInlineNativeBadForm);
        return InliningStatus_NotInlined;
    }

    if (size_t(descr->size()) > InlineTypedObject::MaximumSize)
        return InliningStatus_NotInlined;

    JSObject* obj = inspector->getTemplateObjectForClassHook(pc, descr->getClass());
    if (!obj || !obj->is<InlineTypedObject>())
        return InliningStatus_NotInlined;

    InlineTypedObject* templateObject = &obj->as<InlineTypedObject>();
    if (&templateObject->typeDescr() != descr)
        return InliningStatus_NotInlined;

    callInfo.setImplicitlyUsedUnchecked();

    MNewTypedObject* ins = MNewTypedObject::New(alloc(), constraints(), templateObject,
                                                templateObject->group()->initialHeap(constraints()));
    current->add(ins);
    current->push(ins);

    return InliningStatus_Inlined;
}

IonBuilder::InliningStatus
IonBuilder::inlineSimdInt32x4(CallInfo& callInfo, JSNative native)
{
#define INLINE_INT32X4_SIMD_ARITH_(OP)                                                           \
    if (native == js::simd_int32x4_##OP)                                                         \
        return inlineBinarySimd<MSimdBinaryArith>(callInfo, native, MSimdBinaryArith::Op_##OP,   \
                                                  SimdTypeDescr::Int32x4);

    ARITH_COMMONX4_SIMD_OP(INLINE_INT32X4_SIMD_ARITH_)
#undef INLINE_INT32X4_SIMD_ARITH_

#define INLINE_SIMD_BITWISE_(OP)                                                                 \
    if (native == js::simd_int32x4_##OP)                                                         \
        return inlineBinarySimd<MSimdBinaryBitwise>(callInfo, native, MSimdBinaryBitwise::OP##_, \
                                                    SimdTypeDescr::Int32x4);

    BITWISE_COMMONX4_SIMD_OP(INLINE_SIMD_BITWISE_)
#undef INLINE_SIMD_BITWISE_

    if (native == js::simd_int32x4_shiftLeftByScalar)
        return inlineBinarySimd<MSimdShift>(callInfo, native, MSimdShift::lsh, SimdTypeDescr::Int32x4);
    if (native == js::simd_int32x4_shiftRightArithmeticByScalar)
        return inlineBinarySimd<MSimdShift>(callInfo, native, MSimdShift::rsh, SimdTypeDescr::Int32x4);
    if (native == js::simd_int32x4_shiftRightLogicalByScalar)
        return inlineBinarySimd<MSimdShift>(callInfo, native, MSimdShift::ursh, SimdTypeDescr::Int32x4);

#define INLINE_SIMD_COMPARISON_(OP)                                                                \
    if (native == js::simd_int32x4_##OP)                                                           \
        return inlineCompSimd(callInfo, native, MSimdBinaryComp::OP, SimdTypeDescr::Int32x4);

    COMP_COMMONX4_TO_INT32X4_SIMD_OP(INLINE_SIMD_COMPARISON_)
#undef INLINE_SIMD_COMPARISON_

    if (native == js::simd_int32x4_extractLane)
        return inlineSimdExtractLane(callInfo, native, SimdTypeDescr::Int32x4);
    if (native == js::simd_int32x4_replaceLane)
        return inlineSimdReplaceLane(callInfo, native, SimdTypeDescr::Int32x4);

    if (native == js::simd_int32x4_not)
        return inlineUnarySimd(callInfo, native, MSimdUnaryArith::not_, SimdTypeDescr::Int32x4);
    if (native == js::simd_int32x4_neg)
        return inlineUnarySimd(callInfo, native, MSimdUnaryArith::neg, SimdTypeDescr::Int32x4);

    typedef bool IsCast;
    if (native == js::simd_int32x4_fromFloat32x4)
        return inlineSimdConvert(callInfo, native, IsCast(false), SimdTypeDescr::Float32x4, SimdTypeDescr::Int32x4);
    if (native == js::simd_int32x4_fromFloat32x4Bits)
        return inlineSimdConvert(callInfo, native, IsCast(true), SimdTypeDescr::Float32x4, SimdTypeDescr::Int32x4);

    if (native == js::simd_int32x4_splat)
        return inlineSimdSplat(callInfo, native, SimdTypeDescr::Int32x4);

    if (native == js::simd_int32x4_check)
        return inlineSimdCheck(callInfo, native, SimdTypeDescr::Int32x4);

    typedef bool IsElementWise;
    if (native == js::simd_int32x4_select)
        return inlineSimdSelect(callInfo, native, IsElementWise(true), SimdTypeDescr::Int32x4);
    if (native == js::simd_int32x4_selectBits)
        return inlineSimdSelect(callInfo, native, IsElementWise(false), SimdTypeDescr::Int32x4);

    if (native == js::simd_int32x4_swizzle)
        return inlineSimdShuffle(callInfo, native, SimdTypeDescr::Int32x4, 1, 4);
    if (native == js::simd_int32x4_shuffle)
        return inlineSimdShuffle(callInfo, native, SimdTypeDescr::Int32x4, 2, 4);

    if (native == js::simd_int32x4_load)
        return inlineSimdLoad(callInfo, native, SimdTypeDescr::Int32x4, 4);
    if (native == js::simd_int32x4_load1)
        return inlineSimdLoad(callInfo, native, SimdTypeDescr::Int32x4, 1);
    if (native == js::simd_int32x4_load2)
        return inlineSimdLoad(callInfo, native, SimdTypeDescr::Int32x4, 2);
    if (native == js::simd_int32x4_load3)
        return inlineSimdLoad(callInfo, native, SimdTypeDescr::Int32x4, 3);

    if (native == js::simd_int32x4_store)
        return inlineSimdStore(callInfo, native, SimdTypeDescr::Int32x4, 4);
    if (native == js::simd_int32x4_store1)
        return inlineSimdStore(callInfo, native, SimdTypeDescr::Int32x4, 1);
    if (native == js::simd_int32x4_store2)
        return inlineSimdStore(callInfo, native, SimdTypeDescr::Int32x4, 2);
    if (native == js::simd_int32x4_store3)
        return inlineSimdStore(callInfo, native, SimdTypeDescr::Int32x4, 3);

    return InliningStatus_NotInlined;
}

IonBuilder::InliningStatus
IonBuilder::inlineSimdFloat32x4(CallInfo& callInfo, JSNative native)
{
    // Simd functions
#define INLINE_FLOAT32X4_SIMD_ARITH_(OP)                                                         \
    if (native == js::simd_float32x4_##OP)                                                       \
        return inlineBinarySimd<MSimdBinaryArith>(callInfo, native, MSimdBinaryArith::Op_##OP,   \
                                                  SimdTypeDescr::Float32x4);

    ARITH_COMMONX4_SIMD_OP(INLINE_FLOAT32X4_SIMD_ARITH_)
    BINARY_ARITH_FLOAT32X4_SIMD_OP(INLINE_FLOAT32X4_SIMD_ARITH_)
#undef INLINE_FLOAT32X4_SIMD_ARITH_

#define INLINE_SIMD_BITWISE_(OP)                                                                 \
    if (native == js::simd_float32x4_##OP)                                                       \
        return inlineBinarySimd<MSimdBinaryBitwise>(callInfo, native, MSimdBinaryBitwise::OP##_, \
                                                    SimdTypeDescr::Float32x4);

    BITWISE_COMMONX4_SIMD_OP(INLINE_SIMD_BITWISE_)
#undef INLINE_SIMD_BITWISE_

#define INLINE_SIMD_COMPARISON_(OP)                                                                \
    if (native == js::simd_float32x4_##OP)                                                         \
        return inlineCompSimd(callInfo, native, MSimdBinaryComp::OP, SimdTypeDescr::Float32x4);

    COMP_COMMONX4_TO_INT32X4_SIMD_OP(INLINE_SIMD_COMPARISON_)
#undef INLINE_SIMD_COMPARISON_

    if (native == js::simd_float32x4_extractLane)
        return inlineSimdExtractLane(callInfo, native, SimdTypeDescr::Float32x4);
    if (native == js::simd_float32x4_replaceLane)
        return inlineSimdReplaceLane(callInfo, native, SimdTypeDescr::Float32x4);

#define INLINE_SIMD_FLOAT32X4_UNARY_(OP)                                                           \
    if (native == js::simd_float32x4_##OP)                                                         \
        return inlineUnarySimd(callInfo, native, MSimdUnaryArith::OP, SimdTypeDescr::Float32x4);

    UNARY_ARITH_FLOAT32X4_SIMD_OP(INLINE_SIMD_FLOAT32X4_UNARY_)
    INLINE_SIMD_FLOAT32X4_UNARY_(neg)
#undef INLINE_SIMD_FLOAT32X4_UNARY_

    if (native == js::simd_float32x4_not)
        return inlineUnarySimd(callInfo, native, MSimdUnaryArith::not_, SimdTypeDescr::Float32x4);

    typedef bool IsCast;
    if (native == js::simd_float32x4_fromInt32x4)
        return inlineSimdConvert(callInfo, native, IsCast(false), SimdTypeDescr::Int32x4, SimdTypeDescr::Float32x4);
    if (native == js::simd_float32x4_fromInt32x4Bits)
        return inlineSimdConvert(callInfo, native, IsCast(true), SimdTypeDescr::Int32x4, SimdTypeDescr::Float32x4);

    if (native == js::simd_float32x4_splat)
        return inlineSimdSplat(callInfo, native, SimdTypeDescr::Float32x4);

    if (native == js::simd_float32x4_check)
        return inlineSimdCheck(callInfo, native, SimdTypeDescr::Float32x4);

    typedef bool IsElementWise;
    if (native == js::simd_float32x4_select)
        return inlineSimdSelect(callInfo, native, IsElementWise(true), SimdTypeDescr::Float32x4);

    if (native == js::simd_float32x4_swizzle)
        return inlineSimdShuffle(callInfo, native, SimdTypeDescr::Float32x4, 1, 4);
    if (native == js::simd_float32x4_shuffle)
        return inlineSimdShuffle(callInfo, native, SimdTypeDescr::Float32x4, 2, 4);

    if (native == js::simd_float32x4_load)
        return inlineSimdLoad(callInfo, native, SimdTypeDescr::Float32x4, 4);
    if (native == js::simd_float32x4_load1)
        return inlineSimdLoad(callInfo, native, SimdTypeDescr::Float32x4, 1);
    if (native == js::simd_float32x4_load2)
        return inlineSimdLoad(callInfo, native, SimdTypeDescr::Float32x4, 2);
    if (native == js::simd_float32x4_load3)
        return inlineSimdLoad(callInfo, native, SimdTypeDescr::Float32x4, 3);

    if (native == js::simd_float32x4_store)
        return inlineSimdStore(callInfo, native, SimdTypeDescr::Float32x4, 4);
    if (native == js::simd_float32x4_store1)
        return inlineSimdStore(callInfo, native, SimdTypeDescr::Float32x4, 1);
    if (native == js::simd_float32x4_store2)
        return inlineSimdStore(callInfo, native, SimdTypeDescr::Float32x4, 2);
    if (native == js::simd_float32x4_store3)
        return inlineSimdStore(callInfo, native, SimdTypeDescr::Float32x4, 3);

    return InliningStatus_NotInlined;
}

IonBuilder::InliningStatus
IonBuilder::inlineConstructSimdObject(CallInfo& callInfo, SimdTypeDescr* descr)
{
    // Generic constructor of SIMD valuesX4.
    MIRType simdType = SimdTypeDescrToMIRType(descr->type());

    // TODO Happens for Float64x2 (Bug 1124205) and Int8x16/Int16x8 (Bug 1136226)
    if (simdType == MIRType_Undefined)
        return InliningStatus_NotInlined;

    // Take the templateObject out of Baseline ICs, such that we can box
    // SIMD value type in the same kind of objects.
    MOZ_ASSERT(size_t(descr->size(descr->type())) < InlineTypedObject::MaximumSize);
    JSObject* templateObject = inspector->getTemplateObjectForClassHook(pc, descr->getClass());
    if (!templateObject)
        return InliningStatus_NotInlined;

    // The previous assertion ensures this will never fail if we were able to
    // allocate a templateObject in Baseline.
    InlineTypedObject* inlineTypedObject = &templateObject->as<InlineTypedObject>();
    MOZ_ASSERT(&inlineTypedObject->typeDescr() == descr);

    // When there are missing arguments, provide a default value
    // containing the coercion of 'undefined' to the right type.
    MConstant* defVal = nullptr;
    if (callInfo.argc() < SimdTypeToLength(simdType)) {
        MIRType laneType = SimdTypeToLaneType(simdType);
        if (laneType == MIRType_Int32) {
            defVal = constant(Int32Value(0));
        } else {
            MOZ_ASSERT(IsFloatingPointType(laneType));
            defVal = constant(DoubleNaNValue());
            defVal->setResultType(laneType);
        }
    }

    MSimdValueX4* values =
        MSimdValueX4::New(alloc(), simdType,
                          callInfo.getArgWithDefault(0, defVal), callInfo.getArgWithDefault(1, defVal),
                          callInfo.getArgWithDefault(2, defVal), callInfo.getArgWithDefault(3, defVal));
    current->add(values);

    MSimdBox* obj = MSimdBox::New(alloc(), constraints(), values, inlineTypedObject,
                                  inlineTypedObject->group()->initialHeap(constraints()));
    current->add(obj);
    current->push(obj);

    callInfo.setImplicitlyUsedUnchecked();
    return InliningStatus_Inlined;
}

bool
IonBuilder::checkInlineSimd(CallInfo& callInfo, JSNative native, SimdTypeDescr::Type type,
                            unsigned numArgs, InlineTypedObject** templateObj)
{
    if (callInfo.argc() != numArgs)
        return false;

    JSObject* templateObject = inspector->getTemplateObjectForNative(pc, native);
    if (!templateObject)
        return false;;

    InlineTypedObject* inlineTypedObject = &templateObject->as<InlineTypedObject>();
    MOZ_ASSERT(inlineTypedObject->typeDescr().as<SimdTypeDescr>().type() == type);
    *templateObj = inlineTypedObject;
    return true;
}

IonBuilder::InliningStatus
IonBuilder::boxSimd(CallInfo& callInfo, MInstruction* ins, InlineTypedObject* templateObj)
{
    MSimdBox* obj = MSimdBox::New(alloc(), constraints(), ins, templateObj,
                                  templateObj->group()->initialHeap(constraints()));
    current->add(ins);
    current->add(obj);
    current->push(obj);

    callInfo.setImplicitlyUsedUnchecked();
    return InliningStatus_Inlined;
}

template<typename T>
IonBuilder::InliningStatus
IonBuilder::inlineBinarySimd(CallInfo& callInfo, JSNative native, typename T::Operation op,
                             SimdTypeDescr::Type type)
{
    InlineTypedObject* templateObj = nullptr;
    if (!checkInlineSimd(callInfo, native, type, 2, &templateObj))
        return InliningStatus_NotInlined;

    // If the type of any of the arguments is neither a SIMD type, an Object
    // type, or a Value, then the applyTypes phase will add a fallible box &
    // unbox sequence.  This does not matter much as all binary SIMD
    // instructions are supposed to produce a TypeError when they're called
    // with non SIMD-arguments.
    MIRType mirType = SimdTypeDescrToMIRType(type);
    T* ins = T::New(alloc(), callInfo.getArg(0), callInfo.getArg(1), op, mirType);
    return boxSimd(callInfo, ins, templateObj);
}

IonBuilder::InliningStatus
IonBuilder::inlineCompSimd(CallInfo& callInfo, JSNative native, MSimdBinaryComp::Operation op,
                           SimdTypeDescr::Type compType)
{
    InlineTypedObject* templateObj = nullptr;
    if (!checkInlineSimd(callInfo, native, SimdTypeDescr::Int32x4, 2, &templateObj))
        return InliningStatus_NotInlined;

    // If the type of any of the arguments is neither a SIMD type, an Object
    // type, or a Value, then the applyTypes phase will add a fallible box &
    // unbox sequence.  This does not matter much as all binary SIMD
    // instructions are supposed to produce a TypeError when they're called
    // with non SIMD-arguments.
    MIRType mirType = SimdTypeDescrToMIRType(compType);
    MSimdBinaryComp* ins = MSimdBinaryComp::New(alloc(), callInfo.getArg(0), callInfo.getArg(1), op, mirType);
    return boxSimd(callInfo, ins, templateObj);
}

IonBuilder::InliningStatus
IonBuilder::inlineUnarySimd(CallInfo& callInfo, JSNative native, MSimdUnaryArith::Operation op,
                            SimdTypeDescr::Type type)
{
    InlineTypedObject* templateObj = nullptr;
    if (!checkInlineSimd(callInfo, native, type, 1, &templateObj))
        return InliningStatus_NotInlined;

    // See comment in inlineBinarySimd
    MIRType mirType = SimdTypeDescrToMIRType(type);
    MSimdUnaryArith* ins = MSimdUnaryArith::New(alloc(), callInfo.getArg(0), op, mirType);
    return boxSimd(callInfo, ins, templateObj);
}

IonBuilder::InliningStatus
IonBuilder::inlineSimdSplat(CallInfo& callInfo, JSNative native, SimdTypeDescr::Type type)
{
    InlineTypedObject* templateObj = nullptr;
    if (!checkInlineSimd(callInfo, native, type, 1, &templateObj))
        return InliningStatus_NotInlined;

    // See comment in inlineBinarySimd
    MIRType mirType = SimdTypeDescrToMIRType(type);
    MSimdSplatX4* ins = MSimdSplatX4::New(alloc(), callInfo.getArg(0), mirType);
    return boxSimd(callInfo, ins, templateObj);
}

IonBuilder::InliningStatus
IonBuilder::inlineSimdExtractLane(CallInfo& callInfo, JSNative native, SimdTypeDescr::Type type)
{
    InlineTypedObject* templateObj = nullptr;
    if (!checkInlineSimd(callInfo, native, type, 2, &templateObj))
        return InliningStatus_NotInlined;

    MDefinition* arg = callInfo.getArg(1);
    if (!arg->isConstantValue() || arg->type() != MIRType_Int32)
        return InliningStatus_NotInlined;
    int32_t lane = callInfo.getArg(1)->constantValue().toInt32();
    if (lane < 0 || lane >= 4)
        return InliningStatus_NotInlined;

    // See comment in inlineBinarySimd
    MIRType vecType = SimdTypeDescrToMIRType(type);
    MIRType laneType = SimdTypeToLaneType(vecType);
    MSimdExtractElement* ins = MSimdExtractElement::New(alloc(), callInfo.getArg(0),
                                                        vecType, laneType, SimdLane(lane));
    current->add(ins);
    current->push(ins);
    callInfo.setImplicitlyUsedUnchecked();
    return InliningStatus_Inlined;
}

IonBuilder::InliningStatus
IonBuilder::inlineSimdReplaceLane(CallInfo& callInfo, JSNative native, SimdTypeDescr::Type type)
{
    InlineTypedObject* templateObj = nullptr;
    if (!checkInlineSimd(callInfo, native, type, 3, &templateObj))
        return InliningStatus_NotInlined;

    MDefinition* arg = callInfo.getArg(1);
    if (!arg->isConstantValue() || arg->type() != MIRType_Int32)
        return InliningStatus_NotInlined;

    int32_t lane = arg->constantValue().toInt32();
    if (lane < 0 || lane >= 4)
        return InliningStatus_NotInlined;

    // See comment in inlineBinarySimd
    MIRType mirType = SimdTypeDescrToMIRType(type);
    MSimdInsertElement* ins = MSimdInsertElement::New(alloc(), callInfo.getArg(0),
                                                      callInfo.getArg(2), mirType, SimdLane(lane));
    return boxSimd(callInfo, ins, templateObj);
}

IonBuilder::InliningStatus
IonBuilder::inlineSimdConvert(CallInfo& callInfo, JSNative native, bool isCast,
                              SimdTypeDescr::Type from, SimdTypeDescr::Type to)
{
    InlineTypedObject* templateObj = nullptr;
    if (!checkInlineSimd(callInfo, native, to, 1, &templateObj))
        return InliningStatus_NotInlined;

    // See comment in inlineBinarySimd
    MInstruction* ins;
    MIRType fromType = SimdTypeDescrToMIRType(from);
    MIRType toType = SimdTypeDescrToMIRType(to);
    if (isCast)
        ins = MSimdReinterpretCast::New(alloc(), callInfo.getArg(0), fromType, toType);
    else
        ins = MSimdConvert::New(alloc(), callInfo.getArg(0), fromType, toType);

    return boxSimd(callInfo, ins, templateObj);
}

IonBuilder::InliningStatus
IonBuilder::inlineSimdSelect(CallInfo& callInfo, JSNative native, bool isElementWise,
                             SimdTypeDescr::Type type)
{
    InlineTypedObject* templateObj = nullptr;
    if (!checkInlineSimd(callInfo, native, type, 3, &templateObj))
        return InliningStatus_NotInlined;

    // See comment in inlineBinarySimd
    MIRType mirType = SimdTypeDescrToMIRType(type);
    MSimdSelect* ins = MSimdSelect::New(alloc(), callInfo.getArg(0), callInfo.getArg(1),
                                        callInfo.getArg(2), mirType, isElementWise);
    return boxSimd(callInfo, ins, templateObj);
}

IonBuilder::InliningStatus
IonBuilder::inlineSimdCheck(CallInfo& callInfo, JSNative native, SimdTypeDescr::Type type)
{
    InlineTypedObject* templateObj = nullptr;
    if (!checkInlineSimd(callInfo, native, type, 1, &templateObj))
        return InliningStatus_NotInlined;

    MIRType mirType = SimdTypeDescrToMIRType(type);
    MSimdUnbox* unbox = MSimdUnbox::New(alloc(), callInfo.getArg(0), mirType);
    current->add(unbox);
    current->push(callInfo.getArg(0));

    callInfo.setImplicitlyUsedUnchecked();
    return InliningStatus_Inlined;
}

IonBuilder::InliningStatus
IonBuilder::inlineSimdShuffle(CallInfo& callInfo, JSNative native, SimdTypeDescr::Type type,
                              unsigned numVectors, unsigned numLanes)
{
    InlineTypedObject* templateObj = nullptr;
    if (!checkInlineSimd(callInfo, native, type, numVectors + numLanes, &templateObj))
        return InliningStatus_NotInlined;

    MIRType mirType = SimdTypeDescrToMIRType(type);
    MSimdGeneralShuffle* ins = MSimdGeneralShuffle::New(alloc(), numVectors, numLanes, mirType);

    if (!ins->init(alloc()))
        return InliningStatus_Error;

    for (unsigned i = 0; i < numVectors; i++)
        ins->setVector(i, callInfo.getArg(i));
    for (size_t i = 0; i < numLanes; i++)
        ins->setLane(i, callInfo.getArg(numVectors + i));

    return boxSimd(callInfo, ins, templateObj);
}

// Get the typed array element type corresponding to the lanes in a SIMD vector type.
// This only applies to SIMD types that can be loaded and stored to a typed array.
static Scalar::Type
SimdTypeToArrayElementType(SimdTypeDescr::Type type)
{
    switch (type) {
      case SimdTypeDescr::Float32x4: return Scalar::Float32x4;
      case SimdTypeDescr::Int32x4:   return Scalar::Int32x4;
      case SimdTypeDescr::Int8x16:
      case SimdTypeDescr::Int16x8:
      case SimdTypeDescr::Float64x2: break;
    }
    MOZ_CRASH("unexpected simd type");
}

bool
IonBuilder::prepareForSimdLoadStore(CallInfo& callInfo, Scalar::Type simdType, MInstruction** elements,
                                    MDefinition** index, Scalar::Type* arrayType)
{
    MDefinition* array = callInfo.getArg(0);
    *index = callInfo.getArg(1);

    if (!ElementAccessIsAnyTypedArray(constraints(), array, *index, arrayType))
        return false;

    MInstruction* indexAsInt32 = MToInt32::New(alloc(), *index);
    current->add(indexAsInt32);
    *index = indexAsInt32;

    MDefinition* indexForBoundsCheck = *index;

    // Artificially make sure the index is in bounds by adding the difference
    // number of slots needed (e.g. reading from Float32Array we need to make
    // sure to be in bounds for 4 slots, so add 3, etc.).
    MOZ_ASSERT(Scalar::byteSize(simdType) % Scalar::byteSize(*arrayType) == 0);
    int32_t suppSlotsNeeded = Scalar::byteSize(simdType) / Scalar::byteSize(*arrayType) - 1;
    if (suppSlotsNeeded) {
        MConstant* suppSlots = constant(Int32Value(suppSlotsNeeded));
        MAdd* addedIndex = MAdd::New(alloc(), *index, suppSlots);
        // We're fine even with the add overflows, as long as the generated code
        // for the bounds check uses an unsigned comparison.
        addedIndex->setInt32Specialization();
        current->add(addedIndex);
        indexForBoundsCheck = addedIndex;
    }

    MInstruction* length;
    addTypedArrayLengthAndData(array, SkipBoundsCheck, index, &length, elements);

    // It can be that the index is out of bounds, while the added index for the
    // bounds check is in bounds, so we actually need two bounds checks here.
    MInstruction* positiveCheck = MBoundsCheck::New(alloc(), *index, length);
    current->add(positiveCheck);

    MInstruction* fullCheck = MBoundsCheck::New(alloc(), indexForBoundsCheck, length);
    current->add(fullCheck);
    return true;
}

IonBuilder::InliningStatus
IonBuilder::inlineSimdLoad(CallInfo& callInfo, JSNative native, SimdTypeDescr::Type type,
                           unsigned numElems)
{
    InlineTypedObject* templateObj = nullptr;
    if (!checkInlineSimd(callInfo, native, type, 2, &templateObj))
        return InliningStatus_NotInlined;

    Scalar::Type simdType = SimdTypeToArrayElementType(type);

    MDefinition* index = nullptr;
    MInstruction* elements = nullptr;
    Scalar::Type arrayType;
    if (!prepareForSimdLoadStore(callInfo, simdType, &elements, &index, &arrayType))
        return InliningStatus_NotInlined;

    MLoadUnboxedScalar* load = MLoadUnboxedScalar::New(alloc(), elements, index, arrayType);
    load->setResultType(SimdTypeDescrToMIRType(type));
    load->setSimdRead(simdType, numElems);

    return boxSimd(callInfo, load, templateObj);
}

IonBuilder::InliningStatus
IonBuilder::inlineSimdStore(CallInfo& callInfo, JSNative native, SimdTypeDescr::Type type,
                            unsigned numElems)
{
    InlineTypedObject* templateObj = nullptr;
    if (!checkInlineSimd(callInfo, native, type, 3, &templateObj))
        return InliningStatus_NotInlined;

    Scalar::Type simdType = SimdTypeToArrayElementType(type);

    MDefinition* index = nullptr;
    MInstruction* elements = nullptr;
    Scalar::Type arrayType;
    if (!prepareForSimdLoadStore(callInfo, simdType, &elements, &index, &arrayType))
        return InliningStatus_NotInlined;

    MDefinition* valueToWrite = callInfo.getArg(2);
    MStoreUnboxedScalar* store = MStoreUnboxedScalar::New(alloc(), elements, index,
                                                          valueToWrite, arrayType,
                                                          MStoreUnboxedScalar::TruncateInput);
    store->setSimdWrite(simdType, numElems);

    current->add(store);
    current->push(valueToWrite);

    callInfo.setImplicitlyUsedUnchecked();

    if (!resumeAfter(store))
        return InliningStatus_Error;

    return InliningStatus_Inlined;
}

#define ADD_NATIVE(native) const JSJitInfo JitInfo_##native { \
    { nullptr }, { uint16_t(InlinableNative::native) }, 0, JSJitInfo::InlinableNative };
    INLINABLE_NATIVE_LIST(ADD_NATIVE)
#undef ADD_NATIVE

} // namespace jit
} // namespace js

/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_TrackedOptimizationInfo_h
#define js_TrackedOptimizationInfo_h

#include "mozilla/Maybe.h"

namespace JS {

#define TRACKED_STRATEGY_LIST(_)                        \
    _(GetProp_ArgumentsLength)                          \
    _(GetProp_ArgumentsCallee)                          \
    _(GetProp_InferredConstant)                         \
    _(GetProp_Constant)                                 \
    _(GetProp_NotDefined)                               \
    _(GetProp_StaticName)                               \
    _(GetProp_SimdGetter)                               \
    _(GetProp_TypedObject)                              \
    _(GetProp_DefiniteSlot)                             \
    _(GetProp_Unboxed)                                  \
    _(GetProp_CommonGetter)                             \
    _(GetProp_InlineAccess)                             \
    _(GetProp_InlineProtoAccess)                        \
    _(GetProp_Innerize)                                 \
    _(GetProp_InlineCache)                              \
    _(GetProp_ModuleNamespace)                          \
                                                        \
    _(SetProp_CommonSetter)                             \
    _(SetProp_TypedObject)                              \
    _(SetProp_DefiniteSlot)                             \
    _(SetProp_Unboxed)                                  \
    _(SetProp_InlineAccess)                             \
    _(SetProp_InlineCache)                              \
                                                        \
    _(GetElem_TypedObject)                              \
    _(GetElem_Dense)                                    \
    _(GetElem_TypedArray)                               \
    _(GetElem_String)                                   \
    _(GetElem_Arguments)                                \
    _(GetElem_ArgumentsInlinedConstant)                 \
    _(GetElem_ArgumentsInlinedSwitch)                   \
    _(GetElem_InlineCache)                              \
                                                        \
    _(SetElem_TypedObject)                              \
    _(SetElem_TypedArray)                               \
    _(SetElem_Dense)                                    \
    _(SetElem_Arguments)                                \
    _(SetElem_InlineCache)                              \
                                                        \
    _(BinaryArith_Concat)                               \
    _(BinaryArith_SpecializedTypes)                     \
    _(BinaryArith_SpecializedOnBaselineTypes)           \
    _(BinaryArith_SharedCache)                          \
    _(BinaryArith_Call)                                 \
                                                        \
    _(InlineCache_OptimizedStub)                        \
                                                        \
    _(NewArray_TemplateObject)                          \
    _(NewArray_SharedCache)                             \
    _(NewArray_Call)                                    \
                                                        \
    _(NewObject_TemplateObject)                         \
    _(NewObject_SharedCache)                            \
    _(NewObject_Call)                                   \
                                                        \
    _(Compare_SpecializedTypes)                         \
    _(Compare_Bitwise)                                  \
    _(Compare_SpecializedOnBaselineTypes)               \
    _(Compare_SharedCache)                              \
    _(Compare_Call)                                     \
                                                        \
    _(Call_Inline)


// Ordering is important below. All outcomes before GenericSuccess will be
// considered failures, and all outcomes after GenericSuccess will be
// considered successes.
#define TRACKED_OUTCOME_LIST(_)                                         \
    _(GenericFailure)                                                   \
    _(Disabled)                                                         \
    _(NoTypeInfo)                                                       \
    _(NoAnalysisInfo)                                                   \
    _(NoShapeInfo)                                                      \
    _(UnknownObject)                                                    \
    _(UnknownProperties)                                                \
    _(Singleton)                                                        \
    _(NotSingleton)                                                     \
    _(NotFixedSlot)                                                     \
    _(InconsistentFixedSlot)                                            \
    _(NotObject)                                                        \
    _(NotStruct)                                                        \
    _(NotUnboxed)                                                       \
    _(NotUndefined)                                                     \
    _(UnboxedConvertedToNative)                                         \
    _(StructNoField)                                                    \
    _(InconsistentFieldType)                                            \
    _(InconsistentFieldOffset)                                          \
    _(NeedsTypeBarrier)                                                 \
    _(InDictionaryMode)                                                 \
    _(NoProtoFound)                                                     \
    _(MultiProtoPaths)                                                  \
    _(NonWritableProperty)                                              \
    _(ProtoIndexedProps)                                                \
    _(ArrayBadFlags)                                                    \
    _(ArrayDoubleConversion)                                            \
    _(ArrayRange)                                                       \
    _(ArraySeenNegativeIndex)                                           \
    _(TypedObjectHasDetachedBuffer)                                     \
    _(TypedObjectArrayRange)                                            \
    _(AccessNotDense)                                                   \
    _(AccessNotSimdObject)                                              \
    _(AccessNotTypedObject)                                             \
    _(AccessNotTypedArray)                                              \
    _(AccessNotString)                                                  \
    _(OperandNotString)                                                 \
    _(OperandNotNumber)                                                 \
    _(OperandNotStringOrNumber)                                         \
    _(OperandNotSimpleArith)                                            \
    _(OperandNotEasilyCoercibleToString)                                \
    _(OutOfBounds)                                                      \
    _(GetElemStringNotCached)                                           \
    _(NonNativeReceiver)                                                \
    _(IndexType)                                                        \
    _(SetElemNonDenseNonTANotCached)                                    \
    _(NoSimdJitSupport)                                                 \
    _(SimdTypeNotOptimized)                                             \
    _(UnknownSimdProperty)                                              \
    _(NotModuleNamespace)                                               \
    _(UnknownProperty)                                                  \
    _(NoTemplateObject)                                                 \
    _(TemplateObjectIsUnboxedWithoutInlineElements)                     \
    _(TemplateObjectIsPlainObjectWithDynamicSlots)                      \
    _(LengthTooBig)                                                     \
    _(SpeculationOnInputTypesFailed)                                    \
    _(RelationalCompare)                                                \
    _(OperandTypeNotBitwiseComparable)                                  \
    _(OperandMaybeEmulatesUndefined)                                    \
    _(LoosyUndefinedNullCompare)                                        \
    _(LoosyInt32BooleanCompare)                                         \
    _(CallsValueOf)                                                     \
    _(StrictCompare)                                                    \
    _(InitHole)                                                         \
                                                                        \
    _(ICOptStub_GenericSuccess)                                         \
                                                                        \
    _(ICGetPropStub_ReadSlot)                                           \
    _(ICGetPropStub_CallGetter)                                         \
    _(ICGetPropStub_ArrayLength)                                        \
    _(ICGetPropStub_UnboxedRead)                                        \
    _(ICGetPropStub_UnboxedReadExpando)                                 \
    _(ICGetPropStub_UnboxedArrayLength)                                 \
    _(ICGetPropStub_TypedArrayLength)                                   \
    _(ICGetPropStub_DOMProxyShadowed)                                   \
    _(ICGetPropStub_DOMProxyUnshadowed)                                 \
    _(ICGetPropStub_GenericProxy)                                       \
    _(ICGetPropStub_ArgumentsLength)                                    \
                                                                        \
    _(ICSetPropStub_Slot)                                               \
    _(ICSetPropStub_GenericProxy)                                       \
    _(ICSetPropStub_DOMProxyShadowed)                                   \
    _(ICSetPropStub_DOMProxyUnshadowed)                                 \
    _(ICSetPropStub_CallSetter)                                         \
    _(ICSetPropStub_AddSlot)                                            \
    _(ICSetPropStub_SetUnboxed)                                         \
                                                                        \
    _(ICGetElemStub_ReadSlot)                                           \
    _(ICGetElemStub_CallGetter)                                         \
    _(ICGetElemStub_ReadUnboxed)                                        \
    _(ICGetElemStub_Dense)                                              \
    _(ICGetElemStub_DenseHole)                                          \
    _(ICGetElemStub_TypedArray)                                         \
    _(ICGetElemStub_ArgsElementMapped)                                  \
    _(ICGetElemStub_ArgsElementUnmapped)                                \
                                                                        \
    _(ICSetElemStub_Dense)                                              \
    _(ICSetElemStub_TypedArray)                                         \
                                                                        \
    _(ICNameStub_ReadSlot)                                              \
    _(ICNameStub_CallGetter)                                            \
    _(ICNameStub_TypeOfNoProperty)                                      \
                                                                        \
    _(CantInlineGeneric)                                                \
    _(CantInlineNoTarget)                                               \
    _(CantInlineNotInterpreted)                                         \
    _(CantInlineNoBaseline)                                             \
    _(CantInlineLazy)                                                   \
    _(CantInlineNotConstructor)                                         \
    _(CantInlineClassConstructor)                                       \
    _(CantInlineDisabledIon)                                            \
    _(CantInlineTooManyArgs)                                            \
    _(CantInlineNeedsArgsObj)                                           \
    _(CantInlineDebuggee)                                               \
    _(CantInlineExceededDepth)                                          \
    _(CantInlineExceededTotalBytecodeLength)                            \
    _(CantInlineBigCaller)                                              \
    _(CantInlineBigCallee)                                              \
    _(CantInlineBigCalleeInlinedBytecodeLength)                         \
    _(CantInlineNotHot)                                                 \
    _(CantInlineNotInDispatch)                                          \
    _(CantInlineUnreachable)                                            \
    _(CantInlineNativeBadForm)                                          \
    _(CantInlineNativeBadType)                                          \
    _(CantInlineNativeNoTemplateObj)                                    \
    _(CantInlineBound)                                                  \
    _(CantInlineNativeNoSpecialization)                                 \
    _(HasCommonInliningPath)                                            \
                                                                        \
    _(GenericSuccess)                                                   \
    _(Inlined)                                                          \
    _(DOM)                                                              \
    _(Monomorphic)                                                      \
    _(Polymorphic)

#define TRACKED_TYPESITE_LIST(_)                \
    _(Receiver)                                 \
    _(Operand)                                  \
    _(Index)                                    \
    _(Value)                                    \
    _(Call_Target)                              \
    _(Call_This)                                \
    _(Call_Arg)                                 \
    _(Call_Return)

enum class TrackedStrategy : uint32_t {
#define STRATEGY_OP(name) name,
    TRACKED_STRATEGY_LIST(STRATEGY_OP)
#undef STRATEGY_OPT

    Count
};

enum class TrackedOutcome : uint32_t {
#define OUTCOME_OP(name) name,
    TRACKED_OUTCOME_LIST(OUTCOME_OP)
#undef OUTCOME_OP

    Count
};

enum class TrackedTypeSite : uint32_t {
#define TYPESITE_OP(name) name,
    TRACKED_TYPESITE_LIST(TYPESITE_OP)
#undef TYPESITE_OP

    Count
};

JS_PUBLIC_API(const char*)
TrackedStrategyString(TrackedStrategy strategy);

JS_PUBLIC_API(const char*)
TrackedOutcomeString(TrackedOutcome outcome);

JS_PUBLIC_API(const char*)
TrackedTypeSiteString(TrackedTypeSite site);

struct ForEachTrackedOptimizationAttemptOp
{
    virtual void operator()(TrackedStrategy strategy, TrackedOutcome outcome) = 0;
};

struct ForEachTrackedOptimizationTypeInfoOp
{
    // Called 0+ times per entry, once for each type in the type set that Ion
    // saw during MIR construction. readType is always called _before_
    // operator() on the same entry.
    //
    // The keyedBy parameter describes how the type is keyed:
    //   - "primitive"   for primitive types
    //   - "constructor" for object types tied to a scripted constructor
    //                   function.
    //   - "alloc site"  for object types tied to an allocation site.
    //   - "prototype"   for object types tied neither to a constructor nor
    //                   to an allocation site, but to a prototype.
    //   - "singleton"   for object types which only has a single value.
    //   - "function"    for object types referring to scripted functions.
    //   - "native"      for object types referring to native functions.
    //
    // The name parameter is the string representation of the type. If the
    // type is keyed by "constructor", or if the type itself refers to a
    // scripted function, the name is the function's displayAtom. If the type
    // is keyed by "native", this is nullptr.
    //
    // The location parameter is the filename if the type is keyed by
    // "constructor", "alloc site", or if the type itself refers to a scripted
    // function. If the type is keyed by "native", it is the offset of the
    // native function, suitable for use with addr2line on Linux or atos on OS
    // X. Otherwise it is nullptr.
    //
    // The lineno parameter is the line number if the type is keyed by
    // "constructor", "alloc site", or if the type itself refers to a scripted
    // function. Otherwise it is Nothing().
    //
    // The location parameter is the only one that may need escaping if being
    // quoted.
    virtual void readType(const char* keyedBy, const char* name,
                          const char* location, const mozilla::Maybe<unsigned>& lineno) = 0;

    // Called once per entry.
    virtual void operator()(TrackedTypeSite site, const char* mirType) = 0;
};

} // namespace JS

#endif // js_TrackedOptimizationInfo_h

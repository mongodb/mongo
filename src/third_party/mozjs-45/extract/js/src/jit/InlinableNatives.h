/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_InlinableNatives_h
#define jit_InlinableNatives_h

#define INLINABLE_NATIVE_LIST(_)    \
    _(Array)                        \
    _(ArrayIsArray)                 \
    _(ArrayPop)                     \
    _(ArrayShift)                   \
    _(ArrayPush)                    \
    _(ArrayConcat)                  \
    _(ArraySlice)                   \
    _(ArraySplice)                  \
                                    \
    _(AtomicsCompareExchange)       \
    _(AtomicsExchange)              \
    _(AtomicsLoad)                  \
    _(AtomicsStore)                 \
    _(AtomicsFence)                 \
    _(AtomicsAdd)                   \
    _(AtomicsSub)                   \
    _(AtomicsAnd)                   \
    _(AtomicsOr)                    \
    _(AtomicsXor)                   \
    _(AtomicsIsLockFree)            \
                                    \
    _(MathAbs)                      \
    _(MathFloor)                    \
    _(MathCeil)                     \
    _(MathRound)                    \
    _(MathClz32)                    \
    _(MathSqrt)                     \
    _(MathATan2)                    \
    _(MathHypot)                    \
    _(MathMax)                      \
    _(MathMin)                      \
    _(MathPow)                      \
    _(MathRandom)                   \
    _(MathImul)                     \
    _(MathFRound)                   \
    _(MathSin)                      \
    _(MathTan)                      \
    _(MathCos)                      \
    _(MathExp)                      \
    _(MathLog)                      \
    _(MathASin)                     \
    _(MathATan)                     \
    _(MathACos)                     \
    _(MathLog10)                    \
    _(MathLog2)                     \
    _(MathLog1P)                    \
    _(MathExpM1)                    \
    _(MathSinH)                     \
    _(MathTanH)                     \
    _(MathCosH)                     \
    _(MathASinH)                    \
    _(MathATanH)                    \
    _(MathACosH)                    \
    _(MathSign)                     \
    _(MathTrunc)                    \
    _(MathCbrt)                     \
                                    \
    _(RegExpExec)                   \
    _(RegExpTest)                   \
                                    \
    _(String)                       \
    _(StringSplit)                  \
    _(StringCharCodeAt)             \
    _(StringFromCharCode)           \
    _(StringCharAt)                 \
    _(StringReplace)                \
                                    \
    _(ObjectCreate)                 \
                                    \
    _(CallBoundFunction)            \
                                    \
    _(SimdInt32x4)                  \
    _(SimdFloat32x4)                \
                                    \
    _(TestBailout)                  \
    _(TestAssertFloat32)            \
    _(TestAssertRecoveredOnBailout) \
                                    \
    _(IntrinsicUnsafeSetReservedSlot) \
    _(IntrinsicUnsafeGetReservedSlot) \
    _(IntrinsicUnsafeGetObjectFromReservedSlot) \
    _(IntrinsicUnsafeGetInt32FromReservedSlot) \
    _(IntrinsicUnsafeGetStringFromReservedSlot) \
    _(IntrinsicUnsafeGetBooleanFromReservedSlot) \
                                    \
    _(IntrinsicIsCallable)          \
    _(IntrinsicToObject)            \
    _(IntrinsicIsObject)            \
    _(IntrinsicToInteger)           \
    _(IntrinsicToString)            \
    _(IntrinsicIsConstructing)      \
    _(IntrinsicSubstringKernel)     \
    _(IntrinsicDefineDataProperty)  \
                                    \
    _(IntrinsicIsArrayIterator)     \
    _(IntrinsicIsMapIterator)       \
    _(IntrinsicIsStringIterator)    \
    _(IntrinsicIsListIterator)      \
                                    \
    _(IntrinsicIsTypedArray)        \
    _(IntrinsicIsPossiblyWrappedTypedArray) \
    _(IntrinsicTypedArrayLength)    \
    _(IntrinsicSetDisjointTypedElements) \
                                    \
    _(IntrinsicObjectIsTypedObject) \
    _(IntrinsicObjectIsTransparentTypedObject) \
    _(IntrinsicObjectIsOpaqueTypedObject) \
    _(IntrinsicObjectIsTypeDescr)   \
    _(IntrinsicTypeDescrIsSimpleType) \
    _(IntrinsicTypeDescrIsArrayType)\
    _(IntrinsicSetTypedObjectOffset)

struct JSJitInfo;

namespace js {
namespace jit {

enum class InlinableNative : uint16_t {
#define ADD_NATIVE(native) native,
    INLINABLE_NATIVE_LIST(ADD_NATIVE)
#undef ADD_NATIVE
};

#define ADD_NATIVE(native) extern const JSJitInfo JitInfo_##native;
    INLINABLE_NATIVE_LIST(ADD_NATIVE)
#undef ADD_NATIVE

} // namespace jit
} // namespace js

#endif /* jit_InlinableNatives_h */

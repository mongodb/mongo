/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_InlinableNatives_h
#define jit_InlinableNatives_h

#include <stdint.h>  // For uint16_t

#ifdef ENABLE_EXPLICIT_RESOURCE_MANAGEMENT
#  define INLINABLE_EXPLICIT_RESOURCE_MANAGEMENENT_LIST(_) \
    _(IntrinsicGuardToAsyncDisposableStack)                \
    _(IntrinsicGuardToDisposableStack)
#else
#  define INLINABLE_EXPLICIT_RESOURCE_MANAGEMENENT_LIST(_)
#endif

#ifdef FUZZING_JS_FUZZILLI
#  define INLINABLE_NATIVE_FUZZILLI_LIST(_) _(FuzzilliHash)
#else
#  define INLINABLE_NATIVE_FUZZILLI_LIST(_)
#endif

#ifdef NIGHTLY_BUILD
#  define INLINABLE_NATIVE_ITERATOR_RANGE_LIST(_) \
    _(IntrinsicGuardToIteratorRange)
#else
#  define INLINABLE_NATIVE_ITERATOR_RANGE_LIST(_)
#endif

#define INLINABLE_NATIVE_LIST(_)                   \
  _(Array)                                         \
  _(ArrayIsArray)                                  \
  _(ArrayJoin)                                     \
  _(ArrayPop)                                      \
  _(ArrayShift)                                    \
  _(ArrayPush)                                     \
  _(ArraySlice)                                    \
                                                   \
  _(AtomicsCompareExchange)                        \
  _(AtomicsExchange)                               \
  _(AtomicsLoad)                                   \
  _(AtomicsStore)                                  \
  _(AtomicsAdd)                                    \
  _(AtomicsSub)                                    \
  _(AtomicsAnd)                                    \
  _(AtomicsOr)                                     \
  _(AtomicsXor)                                    \
  _(AtomicsIsLockFree)                             \
  _(AtomicsPause)                                  \
                                                   \
  _(BigInt)                                        \
  _(BigIntAsIntN)                                  \
  _(BigIntAsUintN)                                 \
                                                   \
  _(Boolean)                                       \
                                                   \
  _(DataViewGetInt8)                               \
  _(DataViewGetUint8)                              \
  _(DataViewGetInt16)                              \
  _(DataViewGetUint16)                             \
  _(DataViewGetInt32)                              \
  _(DataViewGetUint32)                             \
  _(DataViewGetFloat16)                            \
  _(DataViewGetFloat32)                            \
  _(DataViewGetFloat64)                            \
  _(DataViewGetBigInt64)                           \
  _(DataViewGetBigUint64)                          \
  _(DataViewSetInt8)                               \
  _(DataViewSetUint8)                              \
  _(DataViewSetInt16)                              \
  _(DataViewSetUint16)                             \
  _(DataViewSetInt32)                              \
  _(DataViewSetUint32)                             \
  _(DataViewSetFloat16)                            \
  _(DataViewSetFloat32)                            \
  _(DataViewSetFloat64)                            \
  _(DataViewSetBigInt64)                           \
  _(DataViewSetBigUint64)                          \
                                                   \
  _(DateGetTime)                                   \
  _(DateGetFullYear)                               \
  _(DateGetMonth)                                  \
  _(DateGetDate)                                   \
  _(DateGetDay)                                    \
  _(DateGetHours)                                  \
  _(DateGetMinutes)                                \
  _(DateGetSeconds)                                \
                                                   \
  _(FunctionBind)                                  \
                                                   \
  _(IntlGuardToCollator)                           \
  _(IntlGuardToDateTimeFormat)                     \
  _(IntlGuardToDisplayNames)                       \
  _(IntlGuardToDurationFormat)                     \
  _(IntlGuardToListFormat)                         \
  _(IntlGuardToNumberFormat)                       \
  _(IntlGuardToPluralRules)                        \
  _(IntlGuardToRelativeTimeFormat)                 \
  _(IntlGuardToSegmenter)                          \
  _(IntlGuardToSegments)                           \
  _(IntlGuardToSegmentIterator)                    \
                                                   \
  _(MapConstructor)                                \
  _(MapDelete)                                     \
  _(MapGet)                                        \
  _(MapHas)                                        \
  _(MapSet)                                        \
                                                   \
  _(MathAbs)                                       \
  _(MathFloor)                                     \
  _(MathCeil)                                      \
  _(MathRound)                                     \
  _(MathClz32)                                     \
  _(MathSqrt)                                      \
  _(MathATan2)                                     \
  _(MathHypot)                                     \
  _(MathMax)                                       \
  _(MathMin)                                       \
  _(MathPow)                                       \
  _(MathRandom)                                    \
  _(MathImul)                                      \
  _(MathFRound)                                    \
  _(MathF16Round)                                  \
  _(MathSin)                                       \
  _(MathTan)                                       \
  _(MathCos)                                       \
  _(MathExp)                                       \
  _(MathLog)                                       \
  _(MathASin)                                      \
  _(MathATan)                                      \
  _(MathACos)                                      \
  _(MathLog10)                                     \
  _(MathLog2)                                      \
  _(MathLog1P)                                     \
  _(MathExpM1)                                     \
  _(MathSinH)                                      \
  _(MathTanH)                                      \
  _(MathCosH)                                      \
  _(MathASinH)                                     \
  _(MathATanH)                                     \
  _(MathACosH)                                     \
  _(MathSign)                                      \
  _(MathTrunc)                                     \
  _(MathCbrt)                                      \
                                                   \
  _(Number)                                        \
  _(NumberParseInt)                                \
  _(NumberToString)                                \
                                                   \
  _(ReflectGetPrototypeOf)                         \
                                                   \
  _(RegExpMatcher)                                 \
  _(RegExpSearcher)                                \
  _(RegExpSearcherLastLimit)                       \
  _(RegExpHasCaptureGroups)                        \
  _(IsRegExpObject)                                \
  _(IsOptimizableRegExpObject)                     \
  _(IsPossiblyWrappedRegExpObject)                 \
  _(IsRegExpPrototypeOptimizable)                  \
  _(GetFirstDollarIndex)                           \
                                                   \
  _(SetConstructor)                                \
  _(SetDelete)                                     \
  _(SetHas)                                        \
  _(SetAdd)                                        \
  _(SetSize)                                       \
                                                   \
  _(String)                                        \
  _(StringToString)                                \
  _(StringValueOf)                                 \
  _(StringCharCodeAt)                              \
  _(StringCodePointAt)                             \
  _(StringFromCharCode)                            \
  _(StringFromCodePoint)                           \
  _(StringCharAt)                                  \
  _(StringAt)                                      \
  _(StringIncludes)                                \
  _(StringIndexOf)                                 \
  _(StringLastIndexOf)                             \
  _(StringStartsWith)                              \
  _(StringEndsWith)                                \
  _(StringToLowerCase)                             \
  _(StringToUpperCase)                             \
  _(StringTrim)                                    \
  _(StringTrimStart)                               \
  _(StringTrimEnd)                                 \
                                                   \
  _(IntrinsicStringReplaceString)                  \
  _(IntrinsicStringSplitString)                    \
                                                   \
  _(Object)                                        \
  _(ObjectCreate)                                  \
  _(ObjectIs)                                      \
  _(ObjectIsPrototypeOf)                           \
  _(ObjectKeys)                                    \
  _(ObjectToString)                                \
                                                   \
  _(TestBailout)                                   \
  _(TestAssertFloat32)                             \
  _(TestAssertRecoveredOnBailout)                  \
                                                   \
  _(IntrinsicUnsafeSetReservedSlot)                \
  _(IntrinsicUnsafeGetReservedSlot)                \
  _(IntrinsicUnsafeGetObjectFromReservedSlot)      \
  _(IntrinsicUnsafeGetInt32FromReservedSlot)       \
  _(IntrinsicUnsafeGetStringFromReservedSlot)      \
                                                   \
  _(IntrinsicIsCallable)                           \
  _(IntrinsicIsConstructor)                        \
  _(IntrinsicToObject)                             \
  _(IntrinsicIsObject)                             \
  _(IntrinsicIsCrossRealmArrayConstructor)         \
  _(IntrinsicCanOptimizeArraySpecies)              \
  _(IntrinsicCanOptimizeStringProtoSymbolLookup)   \
  _(IntrinsicToInteger)                            \
  _(IntrinsicToLength)                             \
  _(IntrinsicIsConstructing)                       \
  _(IntrinsicSubstringKernel)                      \
  _(IntrinsicIsPackedArray)                        \
                                                   \
  _(IntrinsicIsSuspendedGenerator)                 \
                                                   \
  _(IntrinsicGuardToArrayIterator)                 \
  _(IntrinsicGuardToMapIterator)                   \
  _(IntrinsicGuardToSetIterator)                   \
  _(IntrinsicGuardToStringIterator)                \
  _(IntrinsicGuardToRegExpStringIterator)          \
  _(IntrinsicGuardToWrapForValidIterator)          \
  _(IntrinsicGuardToIteratorHelper)                \
  _(IntrinsicGuardToAsyncIteratorHelper)           \
  _(IntrinsicGuardToMapObject)                     \
  _(IntrinsicGetNextMapEntryForIterator)           \
                                                   \
  _(IntrinsicGuardToSetObject)                     \
  _(IntrinsicGetNextSetEntryForIterator)           \
                                                   \
  _(IntrinsicNewArrayIterator)                     \
  _(IntrinsicNewStringIterator)                    \
  _(IntrinsicNewRegExpStringIterator)              \
  _(IntrinsicArrayIteratorPrototypeOptimizable)    \
                                                   \
  _(IntrinsicGuardToArrayBuffer)                   \
                                                   \
  _(IntrinsicGuardToSharedArrayBuffer)             \
                                                   \
  _(TypedArrayConstructor)                         \
  _(IntrinsicIsTypedArrayConstructor)              \
  _(IntrinsicIsTypedArray)                         \
  _(IntrinsicIsPossiblyWrappedTypedArray)          \
  _(IntrinsicTypedArrayLength)                     \
  _(IntrinsicTypedArrayLengthZeroOnOutOfBounds)    \
  _(IntrinsicPossiblyWrappedTypedArrayLength)      \
  _(IntrinsicRegExpBuiltinExec)                    \
  _(IntrinsicRegExpBuiltinExecForTest)             \
  _(IntrinsicRegExpExec)                           \
  _(IntrinsicRegExpExecForTest)                    \
  _(IntrinsicTypedArrayByteOffset)                 \
  _(IntrinsicTypedArrayElementSize)                \
                                                   \
  _(IntrinsicThisTimeValue)                        \
                                                   \
  INLINABLE_EXPLICIT_RESOURCE_MANAGEMENENT_LIST(_) \
  INLINABLE_NATIVE_FUZZILLI_LIST(_)                \
  INLINABLE_NATIVE_ITERATOR_RANGE_LIST(_)

struct JSClass;
class JSJitInfo;

namespace js {
namespace jit {

enum class InlinableNative : uint16_t {
#define ADD_NATIVE(native) native,
  INLINABLE_NATIVE_LIST(ADD_NATIVE)
#undef ADD_NATIVE
      Limit
};

#define ADD_NATIVE(native) extern const JSJitInfo JitInfo_##native;
INLINABLE_NATIVE_LIST(ADD_NATIVE)
#undef ADD_NATIVE

const JSClass* InlinableNativeGuardToClass(InlinableNative native);

bool CanInlineNativeCrossRealm(InlinableNative native);

}  // namespace jit
}  // namespace js

#endif /* jit_InlinableNatives_h */

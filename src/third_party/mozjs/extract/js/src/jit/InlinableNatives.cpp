/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/InlinableNatives.h"

#ifdef JS_HAS_INTL_API
#  include "builtin/intl/Collator.h"
#  include "builtin/intl/DateTimeFormat.h"
#  include "builtin/intl/DisplayNames.h"
#  include "builtin/intl/ListFormat.h"
#  include "builtin/intl/NumberFormat.h"
#  include "builtin/intl/PluralRules.h"
#  include "builtin/intl/RelativeTimeFormat.h"
#endif
#include "builtin/MapObject.h"
#include "js/experimental/JitInfo.h"
#include "vm/ArrayBufferObject.h"
#include "vm/AsyncIteration.h"
#include "vm/Iteration.h"
#include "vm/SharedArrayObject.h"

using namespace js;
using namespace js::jit;

#define ADD_NATIVE(native)                   \
  const JSJitInfo js::jit::JitInfo_##native{ \
      {nullptr},                             \
      {uint16_t(InlinableNative::native)},   \
      {0},                                   \
      JSJitInfo::InlinableNative};
INLINABLE_NATIVE_LIST(ADD_NATIVE)
#undef ADD_NATIVE

const JSClass* js::jit::InlinableNativeGuardToClass(InlinableNative native) {
  switch (native) {
#ifdef JS_HAS_INTL_API
    // Intl natives.
    case InlinableNative::IntlGuardToCollator:
      return &CollatorObject::class_;
    case InlinableNative::IntlGuardToDateTimeFormat:
      return &DateTimeFormatObject::class_;
    case InlinableNative::IntlGuardToDisplayNames:
      return &DisplayNamesObject::class_;
    case InlinableNative::IntlGuardToListFormat:
      return &ListFormatObject::class_;
    case InlinableNative::IntlGuardToNumberFormat:
      return &NumberFormatObject::class_;
    case InlinableNative::IntlGuardToPluralRules:
      return &PluralRulesObject::class_;
    case InlinableNative::IntlGuardToRelativeTimeFormat:
      return &RelativeTimeFormatObject::class_;
#else
    case InlinableNative::IntlGuardToCollator:
    case InlinableNative::IntlGuardToDateTimeFormat:
    case InlinableNative::IntlGuardToDisplayNames:
    case InlinableNative::IntlGuardToListFormat:
    case InlinableNative::IntlGuardToNumberFormat:
    case InlinableNative::IntlGuardToPluralRules:
    case InlinableNative::IntlGuardToRelativeTimeFormat:
      MOZ_CRASH("Intl API disabled");
#endif

    // Utility intrinsics.
    case InlinableNative::IntrinsicGuardToArrayIterator:
      return &ArrayIteratorObject::class_;
    case InlinableNative::IntrinsicGuardToMapIterator:
      return &MapIteratorObject::class_;
    case InlinableNative::IntrinsicGuardToSetIterator:
      return &SetIteratorObject::class_;
    case InlinableNative::IntrinsicGuardToStringIterator:
      return &StringIteratorObject::class_;
    case InlinableNative::IntrinsicGuardToRegExpStringIterator:
      return &RegExpStringIteratorObject::class_;
    case InlinableNative::IntrinsicGuardToWrapForValidIterator:
      return &WrapForValidIteratorObject::class_;
    case InlinableNative::IntrinsicGuardToIteratorHelper:
      return &IteratorHelperObject::class_;
    case InlinableNative::IntrinsicGuardToAsyncIteratorHelper:
      return &AsyncIteratorHelperObject::class_;

    case InlinableNative::IntrinsicGuardToMapObject:
      return &MapObject::class_;
    case InlinableNative::IntrinsicGuardToSetObject:
      return &SetObject::class_;
    case InlinableNative::IntrinsicGuardToArrayBuffer:
      return &ArrayBufferObject::class_;
    case InlinableNative::IntrinsicGuardToSharedArrayBuffer:
      return &SharedArrayBufferObject::class_;

    default:
      MOZ_CRASH("Not a GuardTo instruction");
  }
}

// Returns true if |native| can be inlined cross-realm. Especially inlined
// natives that can allocate objects or throw exceptions shouldn't be inlined
// cross-realm without a careful analysis because we might use the wrong realm!
//
// Note that self-hosting intrinsics are never called cross-realm. See the
// MOZ_CRASH below.
//
// If you are adding a new inlinable native, the safe thing is to |return false|
// here.
bool js::jit::CanInlineNativeCrossRealm(InlinableNative native) {
  switch (native) {
    case InlinableNative::MathAbs:
    case InlinableNative::MathFloor:
    case InlinableNative::MathCeil:
    case InlinableNative::MathRound:
    case InlinableNative::MathClz32:
    case InlinableNative::MathSqrt:
    case InlinableNative::MathATan2:
    case InlinableNative::MathHypot:
    case InlinableNative::MathMax:
    case InlinableNative::MathMin:
    case InlinableNative::MathPow:
    case InlinableNative::MathImul:
    case InlinableNative::MathFRound:
    case InlinableNative::MathTrunc:
    case InlinableNative::MathSign:
    case InlinableNative::MathSin:
    case InlinableNative::MathTan:
    case InlinableNative::MathCos:
    case InlinableNative::MathExp:
    case InlinableNative::MathLog:
    case InlinableNative::MathASin:
    case InlinableNative::MathATan:
    case InlinableNative::MathACos:
    case InlinableNative::MathLog10:
    case InlinableNative::MathLog2:
    case InlinableNative::MathLog1P:
    case InlinableNative::MathExpM1:
    case InlinableNative::MathCosH:
    case InlinableNative::MathSinH:
    case InlinableNative::MathTanH:
    case InlinableNative::MathACosH:
    case InlinableNative::MathASinH:
    case InlinableNative::MathATanH:
    case InlinableNative::MathCbrt:
    case InlinableNative::Boolean:
      return true;

    case InlinableNative::Array:
      // Cross-realm case handled by inlineArray.
      return true;

    case InlinableNative::MathRandom:
      // RNG state is per-realm.
      return false;

    case InlinableNative::IntlGuardToCollator:
    case InlinableNative::IntlGuardToDateTimeFormat:
    case InlinableNative::IntlGuardToDisplayNames:
    case InlinableNative::IntlGuardToListFormat:
    case InlinableNative::IntlGuardToNumberFormat:
    case InlinableNative::IntlGuardToPluralRules:
    case InlinableNative::IntlGuardToRelativeTimeFormat:
    case InlinableNative::IsRegExpObject:
    case InlinableNative::IsPossiblyWrappedRegExpObject:
    case InlinableNative::RegExpMatcher:
    case InlinableNative::RegExpSearcher:
    case InlinableNative::RegExpTester:
    case InlinableNative::RegExpPrototypeOptimizable:
    case InlinableNative::RegExpInstanceOptimizable:
    case InlinableNative::GetFirstDollarIndex:
    case InlinableNative::IntrinsicNewArrayIterator:
    case InlinableNative::IntrinsicNewStringIterator:
    case InlinableNative::IntrinsicNewRegExpStringIterator:
    case InlinableNative::IntrinsicStringReplaceString:
    case InlinableNative::IntrinsicStringSplitString:
    case InlinableNative::IntrinsicUnsafeSetReservedSlot:
    case InlinableNative::IntrinsicUnsafeGetReservedSlot:
    case InlinableNative::IntrinsicUnsafeGetObjectFromReservedSlot:
    case InlinableNative::IntrinsicUnsafeGetInt32FromReservedSlot:
    case InlinableNative::IntrinsicUnsafeGetStringFromReservedSlot:
    case InlinableNative::IntrinsicUnsafeGetBooleanFromReservedSlot:
    case InlinableNative::IntrinsicIsCallable:
    case InlinableNative::IntrinsicIsConstructor:
    case InlinableNative::IntrinsicToObject:
    case InlinableNative::IntrinsicIsObject:
    case InlinableNative::IntrinsicIsCrossRealmArrayConstructor:
    case InlinableNative::IntrinsicToInteger:
    case InlinableNative::IntrinsicToLength:
    case InlinableNative::IntrinsicIsConstructing:
    case InlinableNative::IntrinsicIsSuspendedGenerator:
    case InlinableNative::IntrinsicSubstringKernel:
    case InlinableNative::IntrinsicGuardToArrayIterator:
    case InlinableNative::IntrinsicGuardToMapIterator:
    case InlinableNative::IntrinsicGuardToSetIterator:
    case InlinableNative::IntrinsicGuardToStringIterator:
    case InlinableNative::IntrinsicGuardToRegExpStringIterator:
    case InlinableNative::IntrinsicGuardToWrapForValidIterator:
    case InlinableNative::IntrinsicGuardToIteratorHelper:
    case InlinableNative::IntrinsicGuardToAsyncIteratorHelper:
    case InlinableNative::IntrinsicObjectHasPrototype:
    case InlinableNative::IntrinsicFinishBoundFunctionInit:
    case InlinableNative::IntrinsicIsPackedArray:
    case InlinableNative::IntrinsicGuardToMapObject:
    case InlinableNative::IntrinsicGetNextMapEntryForIterator:
    case InlinableNative::IntrinsicGuardToSetObject:
    case InlinableNative::IntrinsicGetNextSetEntryForIterator:
    case InlinableNative::IntrinsicGuardToArrayBuffer:
    case InlinableNative::IntrinsicArrayBufferByteLength:
    case InlinableNative::IntrinsicPossiblyWrappedArrayBufferByteLength:
    case InlinableNative::IntrinsicGuardToSharedArrayBuffer:
    case InlinableNative::IntrinsicIsTypedArrayConstructor:
    case InlinableNative::IntrinsicIsTypedArray:
    case InlinableNative::IntrinsicIsPossiblyWrappedTypedArray:
    case InlinableNative::IntrinsicPossiblyWrappedTypedArrayLength:
    case InlinableNative::IntrinsicTypedArrayLength:
    case InlinableNative::IntrinsicTypedArrayByteOffset:
    case InlinableNative::IntrinsicTypedArrayElementSize:
    case InlinableNative::IntrinsicArrayIteratorPrototypeOptimizable:
      MOZ_CRASH("Unexpected cross-realm intrinsic call");

    case InlinableNative::TestBailout:
    case InlinableNative::TestAssertFloat32:
    case InlinableNative::TestAssertRecoveredOnBailout:
      // Testing functions, not worth inlining cross-realm.
      return false;

    case InlinableNative::ArrayIsArray:
    case InlinableNative::ArrayJoin:
    case InlinableNative::ArrayPop:
    case InlinableNative::ArrayShift:
    case InlinableNative::ArrayPush:
    case InlinableNative::ArraySlice:
    case InlinableNative::AtomicsCompareExchange:
    case InlinableNative::AtomicsExchange:
    case InlinableNative::AtomicsLoad:
    case InlinableNative::AtomicsStore:
    case InlinableNative::AtomicsAdd:
    case InlinableNative::AtomicsSub:
    case InlinableNative::AtomicsAnd:
    case InlinableNative::AtomicsOr:
    case InlinableNative::AtomicsXor:
    case InlinableNative::AtomicsIsLockFree:
    case InlinableNative::BigIntAsIntN:
    case InlinableNative::BigIntAsUintN:
    case InlinableNative::DataViewGetInt8:
    case InlinableNative::DataViewGetUint8:
    case InlinableNative::DataViewGetInt16:
    case InlinableNative::DataViewGetUint16:
    case InlinableNative::DataViewGetInt32:
    case InlinableNative::DataViewGetUint32:
    case InlinableNative::DataViewGetFloat32:
    case InlinableNative::DataViewGetFloat64:
    case InlinableNative::DataViewGetBigInt64:
    case InlinableNative::DataViewGetBigUint64:
    case InlinableNative::DataViewSetInt8:
    case InlinableNative::DataViewSetUint8:
    case InlinableNative::DataViewSetInt16:
    case InlinableNative::DataViewSetUint16:
    case InlinableNative::DataViewSetInt32:
    case InlinableNative::DataViewSetUint32:
    case InlinableNative::DataViewSetFloat32:
    case InlinableNative::DataViewSetFloat64:
    case InlinableNative::DataViewSetBigInt64:
    case InlinableNative::DataViewSetBigUint64:
    case InlinableNative::NumberToString:
    case InlinableNative::ReflectGetPrototypeOf:
    case InlinableNative::String:
    case InlinableNative::StringToString:
    case InlinableNative::StringValueOf:
    case InlinableNative::StringCharCodeAt:
    case InlinableNative::StringFromCharCode:
    case InlinableNative::StringFromCodePoint:
    case InlinableNative::StringCharAt:
    case InlinableNative::StringToLowerCase:
    case InlinableNative::StringToUpperCase:
    case InlinableNative::Object:
    case InlinableNative::ObjectCreate:
    case InlinableNative::ObjectIs:
    case InlinableNative::ObjectIsPrototypeOf:
    case InlinableNative::ObjectToString:
    case InlinableNative::TypedArrayConstructor:
      // Default to false for most natives.
      return false;

    case InlinableNative::Limit:
      break;
  }
  MOZ_CRASH("Unknown native");
}

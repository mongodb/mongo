/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "builtin/JSON.h"

#include "mozilla/CheckedInt.h"
#include "mozilla/FloatingPoint.h"
#include "mozilla/Range.h"
#include "mozilla/ScopeExit.h"
#include "mozilla/Variant.h"

#include <algorithm>

#include "jsnum.h"
#include "jstypes.h"

#include "builtin/Array.h"
#include "builtin/BigInt.h"
#include "builtin/ParseRecordObject.h"
#include "builtin/RawJSONObject.h"
#include "js/friend/ErrorMessages.h"  // js::GetErrorMessage, JSMSG_*
#include "js/friend/StackLimits.h"    // js::AutoCheckRecursionLimit
#include "js/Object.h"                // JS::GetBuiltinClass
#include "js/Prefs.h"                 // JS::Prefs
#include "js/PropertySpec.h"
#include "js/StableStringChars.h"
#include "js/TypeDecls.h"
#include "js/Value.h"
#include "util/StringBuffer.h"
#include "vm/BooleanObject.h"       // js::BooleanObject
#include "vm/EqualityOperations.h"  // js::SameValue
#include "vm/Interpreter.h"
#include "vm/Iteration.h"
#include "vm/JSAtomUtils.h"  // ToAtom
#include "vm/JSContext.h"
#include "vm/JSObject.h"
#include "vm/JSONParser.h"
#include "vm/NativeObject.h"
#include "vm/NumberObject.h"  // js::NumberObject
#include "vm/PlainObject.h"   // js::PlainObject
#include "vm/StringObject.h"  // js::StringObject
#ifdef ENABLE_RECORD_TUPLE
#  include "builtin/RecordObject.h"
#  include "builtin/TupleObject.h"
#  include "vm/RecordType.h"
#endif

#include "builtin/Array-inl.h"
#include "vm/GeckoProfiler-inl.h"
#include "vm/JSAtomUtils-inl.h"  // AtomToId, PrimitiveValueToId, IndexToId, IdToString,
#include "vm/NativeObject-inl.h"

using namespace js;

using mozilla::AsVariant;
using mozilla::CheckedInt;
using mozilla::Maybe;
using mozilla::RangedPtr;
using mozilla::Variant;

using JS::AutoStableStringChars;

/* https://262.ecma-international.org/14.0/#sec-quotejsonstring
 * Requires that the destination has enough space allocated for src after
 * escaping (that is, `2 + 6 * (srcEnd - srcBegin)` characters).
 */
template <typename SrcCharT, typename DstCharT>
static MOZ_ALWAYS_INLINE RangedPtr<DstCharT> InfallibleQuoteJSONString(
    RangedPtr<const SrcCharT> srcBegin, RangedPtr<const SrcCharT> srcEnd,
    RangedPtr<DstCharT> dstPtr) {
  // Maps characters < 256 to the value that must follow the '\\' in the quoted
  // string. Entries with 'u' are handled as \\u00xy, and entries with 0 are not
  // escaped in any way. Characters >= 256 are all assumed to be unescaped.
  static const Latin1Char escapeLookup[256] = {
      // clang-format off
        'u', 'u', 'u', 'u', 'u', 'u', 'u', 'u', 'b', 't',
        'n', 'u', 'f', 'r', 'u', 'u', 'u', 'u', 'u', 'u',
        'u', 'u', 'u', 'u', 'u', 'u', 'u', 'u', 'u', 'u',
        'u', 'u', 0,   0,  '\"', 0,   0,   0,   0,   0,
        0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
        0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
        0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
        0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
        0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
        0,   0,  '\\', // rest are all zeros
      // clang-format on
  };

  /* Step 1. */
  *dstPtr++ = '"';

  auto ToLowerHex = [](uint8_t u) {
    MOZ_ASSERT(u <= 0xF);
    return "0123456789abcdef"[u];
  };

  /* Step 2. */
  while (srcBegin != srcEnd) {
    const SrcCharT c = *srcBegin++;

    // Handle the Latin-1 cases.
    if (MOZ_LIKELY(c < sizeof(escapeLookup))) {
      Latin1Char escaped = escapeLookup[c];

      // Directly copy non-escaped code points.
      if (escaped == 0) {
        *dstPtr++ = c;
        continue;
      }

      // Escape the rest, elaborating Unicode escapes when needed.
      *dstPtr++ = '\\';
      *dstPtr++ = escaped;
      if (escaped == 'u') {
        *dstPtr++ = '0';
        *dstPtr++ = '0';

        uint8_t x = c >> 4;
        MOZ_ASSERT(x < 10);
        *dstPtr++ = '0' + x;

        *dstPtr++ = ToLowerHex(c & 0xF);
      }

      continue;
    }

    // Non-ASCII non-surrogates are directly copied.
    if (!unicode::IsSurrogate(c)) {
      *dstPtr++ = c;
      continue;
    }

    // So too for complete surrogate pairs.
    if (MOZ_LIKELY(unicode::IsLeadSurrogate(c) && srcBegin < srcEnd &&
                   unicode::IsTrailSurrogate(*srcBegin))) {
      *dstPtr++ = c;
      *dstPtr++ = *srcBegin++;
      continue;
    }

    // But lone surrogates are Unicode-escaped.
    char32_t as32 = char32_t(c);
    *dstPtr++ = '\\';
    *dstPtr++ = 'u';
    *dstPtr++ = ToLowerHex(as32 >> 12);
    *dstPtr++ = ToLowerHex((as32 >> 8) & 0xF);
    *dstPtr++ = ToLowerHex((as32 >> 4) & 0xF);
    *dstPtr++ = ToLowerHex(as32 & 0xF);
  }

  /* Steps 3-4. */
  *dstPtr++ = '"';
  return dstPtr;
}

template <typename SrcCharT, typename DstCharT>
static size_t QuoteJSONStringHelper(const JSLinearString& linear,
                                    StringBuffer& sb, size_t sbOffset) {
  size_t len = linear.length();

  JS::AutoCheckCannotGC nogc;
  RangedPtr<const SrcCharT> srcBegin{linear.chars<SrcCharT>(nogc), len};
  RangedPtr<DstCharT> dstBegin{sb.begin<DstCharT>(), sb.begin<DstCharT>(),
                               sb.end<DstCharT>()};
  RangedPtr<DstCharT> dstEnd =
      InfallibleQuoteJSONString(srcBegin, srcBegin + len, dstBegin + sbOffset);

  return dstEnd - dstBegin;
}

static bool QuoteJSONString(JSContext* cx, StringBuffer& sb, JSString* str) {
  JSLinearString* linear = str->ensureLinear(cx);
  if (!linear) {
    return false;
  }

  if (linear->hasTwoByteChars() && !sb.ensureTwoByteChars()) {
    return false;
  }

  // We resize the backing buffer to the maximum size we could possibly need,
  // write the escaped string into it, and shrink it back to the size we ended
  // up needing.

  size_t len = linear->length();
  size_t sbInitialLen = sb.length();

  CheckedInt<size_t> reservedLen = CheckedInt<size_t>(len) * 6 + 2;
  if (MOZ_UNLIKELY(!reservedLen.isValid())) {
    ReportAllocationOverflow(cx);
    return false;
  }

  if (!sb.growByUninitialized(reservedLen.value())) {
    return false;
  }

  size_t newSize;

  if (linear->hasTwoByteChars()) {
    newSize =
        QuoteJSONStringHelper<char16_t, char16_t>(*linear, sb, sbInitialLen);
  } else if (sb.isUnderlyingBufferLatin1()) {
    newSize = QuoteJSONStringHelper<Latin1Char, Latin1Char>(*linear, sb,
                                                            sbInitialLen);
  } else {
    newSize =
        QuoteJSONStringHelper<Latin1Char, char16_t>(*linear, sb, sbInitialLen);
  }

  sb.shrinkTo(newSize);

  return true;
}

namespace {

using ObjectVector = GCVector<JSObject*, 8>;

class StringifyContext {
 public:
  StringifyContext(JSContext* cx, StringBuffer& sb, const StringBuffer& gap,
                   HandleObject replacer, const RootedIdVector& propertyList,
                   bool maybeSafely)
      : sb(sb),
        gap(gap),
        replacer(cx, replacer),
        stack(cx, ObjectVector(cx)),
        propertyList(propertyList),
        depth(0),
        maybeSafely(maybeSafely) {
    MOZ_ASSERT_IF(maybeSafely, !replacer);
    MOZ_ASSERT_IF(maybeSafely, gap.empty());
  }

  StringBuffer& sb;
  const StringBuffer& gap;
  RootedObject replacer;
  Rooted<ObjectVector> stack;
  const RootedIdVector& propertyList;
  uint32_t depth;
  bool maybeSafely;
};

} /* anonymous namespace */

static bool SerializeJSONProperty(JSContext* cx, const Value& v,
                                  StringifyContext* scx);

static bool WriteIndent(StringifyContext* scx, uint32_t limit) {
  if (!scx->gap.empty()) {
    if (!scx->sb.append('\n')) {
      return false;
    }

    if (scx->gap.isUnderlyingBufferLatin1()) {
      for (uint32_t i = 0; i < limit; i++) {
        if (!scx->sb.append(scx->gap.rawLatin1Begin(),
                            scx->gap.rawLatin1End())) {
          return false;
        }
      }
    } else {
      for (uint32_t i = 0; i < limit; i++) {
        if (!scx->sb.append(scx->gap.rawTwoByteBegin(),
                            scx->gap.rawTwoByteEnd())) {
          return false;
        }
      }
    }
  }

  return true;
}

namespace {

template <typename KeyType>
class KeyStringifier {};

template <>
class KeyStringifier<uint32_t> {
 public:
  static JSString* toString(JSContext* cx, uint32_t index) {
    return IndexToString(cx, index);
  }
};

template <>
class KeyStringifier<HandleId> {
 public:
  static JSString* toString(JSContext* cx, HandleId id) {
    return IdToString(cx, id);
  }
};

} /* anonymous namespace */

/*
 * https://262.ecma-international.org/14.0/#sec-serializejsonproperty, steps
 * 2-4, extracted to enable preprocessing of property values when stringifying
 * objects in SerializeJSONObject.
 */
template <typename KeyType>
static bool PreprocessValue(JSContext* cx, HandleObject holder, KeyType key,
                            MutableHandleValue vp, StringifyContext* scx) {
  // We don't want to do any preprocessing here if scx->maybeSafely,
  // since the stuff we do here can have side-effects.
  if (scx->maybeSafely) {
    return true;
  }

  RootedString keyStr(cx);

  // Step 2. Modified by BigInt spec 6.1 to check for a toJSON method on the
  // BigInt prototype when the value is a BigInt, and to pass the BigInt
  // primitive value as receiver.
  if (vp.isObject() || vp.isBigInt()) {
    RootedValue toJSON(cx);
    RootedObject obj(cx, JS::ToObject(cx, vp));
    if (!obj) {
      return false;
    }

    if (!GetProperty(cx, obj, vp, cx->names().toJSON, &toJSON)) {
      return false;
    }

    if (IsCallable(toJSON)) {
      keyStr = KeyStringifier<KeyType>::toString(cx, key);
      if (!keyStr) {
        return false;
      }

      RootedValue arg0(cx, StringValue(keyStr));
      if (!js::Call(cx, toJSON, vp, arg0, vp)) {
        return false;
      }
    }
  }

  /* Step 3. */
  if (scx->replacer && scx->replacer->isCallable()) {
    MOZ_ASSERT(holder != nullptr,
               "holder object must be present when replacer is callable");

    if (!keyStr) {
      keyStr = KeyStringifier<KeyType>::toString(cx, key);
      if (!keyStr) {
        return false;
      }
    }

    RootedValue arg0(cx, StringValue(keyStr));
    RootedValue replacerVal(cx, ObjectValue(*scx->replacer));
    if (!js::Call(cx, replacerVal, holder, arg0, vp, vp)) {
      return false;
    }
  }

  /* Step 4. */
  if (vp.get().isObject()) {
    RootedObject obj(cx, &vp.get().toObject());

    ESClass cls;
    if (!JS::GetBuiltinClass(cx, obj, &cls)) {
      return false;
    }

    if (cls == ESClass::Number) {
      double d;
      if (!ToNumber(cx, vp, &d)) {
        return false;
      }
      vp.setNumber(d);
    } else if (cls == ESClass::String) {
      JSString* str = ToStringSlow<CanGC>(cx, vp);
      if (!str) {
        return false;
      }
      vp.setString(str);
    } else if (cls == ESClass::Boolean || cls == ESClass::BigInt ||
               IF_RECORD_TUPLE(
                   obj->is<RecordObject>() || obj->is<TupleObject>(), false)) {
      if (!Unbox(cx, obj, vp)) {
        return false;
      }
    }
  }

  return true;
}

/*
 * Determines whether a value which has passed by
 * https://262.ecma-international.org/14.0/#sec-serializejsonproperty steps
 * 1-4's gauntlet will result in SerializeJSONProperty returning |undefined|.
 * This function is used to properly omit properties resulting in such values
 * when stringifying objects, while properly stringifying such properties as
 * null when they're encountered in arrays.
 */
static inline bool IsFilteredValue(const Value& v) {
  MOZ_ASSERT_IF(v.isMagic(), v.isMagic(JS_ELEMENTS_HOLE));
  return v.isUndefined() || v.isSymbol() || v.isMagic() || IsCallable(v);
}

class CycleDetector {
 public:
  CycleDetector(StringifyContext* scx, HandleObject obj)
      : stack_(&scx->stack), obj_(obj), appended_(false) {}

  MOZ_ALWAYS_INLINE bool foundCycle(JSContext* cx) {
    JSObject* obj = obj_;
    for (JSObject* obj2 : stack_) {
      if (MOZ_UNLIKELY(obj == obj2)) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                  JSMSG_JSON_CYCLIC_VALUE);
        return false;
      }
    }
    appended_ = stack_.append(obj);
    return appended_;
  }

  ~CycleDetector() {
    if (MOZ_LIKELY(appended_)) {
      MOZ_ASSERT(stack_.back() == obj_);
      stack_.popBack();
    }
  }

 private:
  MutableHandle<ObjectVector> stack_;
  HandleObject obj_;
  bool appended_;
};

static inline JSString* MaybeGetRawJSON(JSContext* cx, JSObject* obj) {
  if (!obj->is<RawJSONObject>()) {
    return nullptr;
  }

  JSString* rawJSON = obj->as<js::RawJSONObject>().rawJSON(cx);
  MOZ_ASSERT(rawJSON);
  return rawJSON;
}

#ifdef ENABLE_RECORD_TUPLE
enum class JOType { Record, Object };
template <JOType type = JOType::Object>
#endif
/* https://262.ecma-international.org/14.0/#sec-serializejsonobject */
static bool SerializeJSONObject(JSContext* cx, HandleObject obj,
                                StringifyContext* scx) {
  /*
   * This method implements the SerializeJSONObject algorithm, but:
   *
   *   * The algorithm is somewhat reformulated to allow the final string to
   *     be streamed into a single buffer, rather than be created and copied
   *     into place incrementally as the algorithm specifies it.  This
   *     requires moving portions of the SerializeJSONProperty call in 8a into
   *     this algorithm (and in SerializeJSONArray as well).
   */

#ifdef ENABLE_RECORD_TUPLE
  RecordType* rec;

  if constexpr (type == JOType::Record) {
    MOZ_ASSERT(obj->is<RecordType>());
    rec = &obj->as<RecordType>();
  } else {
    MOZ_ASSERT(!IsExtendedPrimitive(*obj));
  }
#endif
  MOZ_ASSERT_IF(scx->maybeSafely, obj->is<PlainObject>());

  /* Steps 1-2, 11. */
  CycleDetector detect(scx, obj);
  if (!detect.foundCycle(cx)) {
    return false;
  }

  if (!scx->sb.append('{')) {
    return false;
  }

  /* Steps 5-7. */
  Maybe<RootedIdVector> ids;
  const RootedIdVector* props;
  if (scx->replacer && !scx->replacer->isCallable()) {
    // NOTE: We can't assert |IsArray(scx->replacer)| because the replacer
    //       might have been a revocable proxy to an array.  Such a proxy
    //       satisfies |IsArray|, but any side effect of JSON.stringify
    //       could revoke the proxy so that |!IsArray(scx->replacer)|.  See
    //       bug 1196497.
    props = &scx->propertyList;
  } else {
    MOZ_ASSERT_IF(scx->replacer, scx->propertyList.length() == 0);
    ids.emplace(cx);
    if (!GetPropertyKeys(cx, obj, JSITER_OWNONLY, ids.ptr())) {
      return false;
    }
    props = ids.ptr();
  }

  /* My kingdom for not-quite-initialized-from-the-start references. */
  const RootedIdVector& propertyList = *props;

  /* Steps 8-10, 13. */
  bool wroteMember = false;
  RootedId id(cx);
  for (size_t i = 0, len = propertyList.length(); i < len; i++) {
    if (!CheckForInterrupt(cx)) {
      return false;
    }

    /*
     * Steps 8a-8b.  Note that the call to SerializeJSONProperty is broken up
     * into 1) getting the property; 2) processing for toJSON, calling the
     * replacer, and handling boxed Number/String/Boolean objects; 3) filtering
     * out values which process to |undefined|, and 4) stringifying all values
     * which pass the filter.
     */
    id = propertyList[i];
    RootedValue outputValue(cx);
#ifdef DEBUG
    if (scx->maybeSafely) {
      PropertyResult prop;
      if (!NativeLookupOwnPropertyNoResolve(cx, &obj->as<NativeObject>(), id,
                                            &prop)) {
        return false;
      }
      MOZ_ASSERT(prop.isNativeProperty() &&
                 prop.propertyInfo().isDataDescriptor());
    }
#endif  // DEBUG

#ifdef ENABLE_RECORD_TUPLE
    if constexpr (type == JOType::Record) {
      MOZ_ALWAYS_TRUE(rec->getOwnProperty(cx, id, &outputValue));
    } else
#endif
    {
      RootedValue objValue(cx, ObjectValue(*obj));
      if (!GetProperty(cx, obj, objValue, id, &outputValue)) {
        return false;
      }
    }
    if (!PreprocessValue(cx, obj, HandleId(id), &outputValue, scx)) {
      return false;
    }
    if (IsFilteredValue(outputValue)) {
      continue;
    }

    /* Output a comma unless this is the first member to write. */
    if (wroteMember && !scx->sb.append(',')) {
      return false;
    }
    wroteMember = true;

    if (!WriteIndent(scx, scx->depth)) {
      return false;
    }

    JSString* s = IdToString(cx, id);
    if (!s) {
      return false;
    }

    if (!QuoteJSONString(cx, scx->sb, s) || !scx->sb.append(':') ||
        !(scx->gap.empty() || scx->sb.append(' ')) ||
        !SerializeJSONProperty(cx, outputValue, scx)) {
      return false;
    }
  }

  if (wroteMember && !WriteIndent(scx, scx->depth - 1)) {
    return false;
  }

  return scx->sb.append('}');
}

// For JSON.stringify and JSON.parse with a reviver function, we need to know
// the length of an object for which JS::IsArray returned true. This must be
// either an ArrayObject or a proxy wrapping one.
static MOZ_ALWAYS_INLINE bool GetLengthPropertyForArrayLike(JSContext* cx,
                                                            HandleObject obj,
                                                            uint32_t* lengthp) {
  if (MOZ_LIKELY(obj->is<ArrayObject>())) {
    *lengthp = obj->as<ArrayObject>().length();
    return true;
  }
#ifdef ENABLE_RECORD_TUPLE
  if (obj->is<TupleType>()) {
    *lengthp = obj->as<TupleType>().length();
    return true;
  }
#endif

  MOZ_ASSERT(obj->is<ProxyObject>());

  uint64_t len = 0;
  if (!GetLengthProperty(cx, obj, &len)) {
    return false;
  }

  // A scripted proxy wrapping an array can return a length value larger than
  // UINT32_MAX. Stringification will likely report an OOM in this case. Match
  // other JS engines and report an early error in this case, although
  // technically this is observable, for example when stringifying with a
  // replacer function.
  if (len > UINT32_MAX) {
    ReportAllocationOverflow(cx);
    return false;
  }

  *lengthp = uint32_t(len);
  return true;
}

/* https://262.ecma-international.org/14.0/#sec-serializejsonarray */
static bool SerializeJSONArray(JSContext* cx, HandleObject obj,
                               StringifyContext* scx) {
  /*
   * This method implements the SerializeJSONArray algorithm, but:
   *
   *   * The algorithm is somewhat reformulated to allow the final string to
   *     be streamed into a single buffer, rather than be created and copied
   *     into place incrementally as the algorithm specifies it.  This
   *     requires moving portions of the SerializeJSONProperty call in 8a into
   *     this algorithm (and in SerializeJSONObject as well).
   */

  /* Steps 1-2, 11. */
  CycleDetector detect(scx, obj);
  if (!detect.foundCycle(cx)) {
    return false;
  }

  if (!scx->sb.append('[')) {
    return false;
  }

  /* Step 6. */
  uint32_t length;
  if (!GetLengthPropertyForArrayLike(cx, obj, &length)) {
    return false;
  }

  /* Steps 7-10. */
  if (length != 0) {
    /* Steps 4, 10b(i). */
    if (!WriteIndent(scx, scx->depth)) {
      return false;
    }

    /* Steps 7-10. */
    RootedValue outputValue(cx);
    for (uint32_t i = 0; i < length; i++) {
      if (!CheckForInterrupt(cx)) {
        return false;
      }

      /*
       * Steps 8a-8c.  Again note how the call to the spec's
       * SerializeJSONProperty method is broken up into getting the property,
       * running it past toJSON and the replacer and maybe unboxing, and
       * interpreting some values as |null| in separate steps.
       */
#ifdef DEBUG
      if (scx->maybeSafely) {
        /*
         * Trying to do a JS_AlreadyHasOwnElement runs the risk of
         * hitting OOM on jsid creation.  Let's just assert sanity for
         * small enough indices.
         */
        MOZ_ASSERT(obj->is<ArrayObject>());
        MOZ_ASSERT(obj->is<NativeObject>());
        Rooted<NativeObject*> nativeObj(cx, &obj->as<NativeObject>());
        if (i <= PropertyKey::IntMax) {
          MOZ_ASSERT(
              nativeObj->containsDenseElement(i) != nativeObj->isIndexed(),
              "the array must either be small enough to remain "
              "fully dense (and otherwise un-indexed), *or* "
              "all its initially-dense elements were sparsified "
              "and the object is indexed");
        } else {
          MOZ_ASSERT(nativeObj->isIndexed());
        }
      }
#endif
      if (!GetElement(cx, obj, i, &outputValue)) {
        return false;
      }
      if (!PreprocessValue(cx, obj, i, &outputValue, scx)) {
        return false;
      }
      if (IsFilteredValue(outputValue)) {
        if (!scx->sb.append("null")) {
          return false;
        }
      } else {
        if (!SerializeJSONProperty(cx, outputValue, scx)) {
          return false;
        }
      }

      /* Steps 3, 4, 10b(i). */
      if (i < length - 1) {
        if (!scx->sb.append(',')) {
          return false;
        }
        if (!WriteIndent(scx, scx->depth)) {
          return false;
        }
      }
    }

    /* Step 10(b)(iii). */
    if (!WriteIndent(scx, scx->depth - 1)) {
      return false;
    }
  }

  return scx->sb.append(']');
}

/* https://262.ecma-international.org/14.0/#sec-serializejsonproperty */
static bool SerializeJSONProperty(JSContext* cx, const Value& v,
                                  StringifyContext* scx) {
  /* Step 12 must be handled by the caller. */
  MOZ_ASSERT(!IsFilteredValue(v));

  /*
   * This method implements the SerializeJSONProperty algorithm, but:
   *
   *   * We move property retrieval (step 1) into callers to stream the
   *     stringification process and avoid constantly copying strings.
   *   * We move the preprocessing in steps 2-4 into a helper function to
   *     allow both SerializeJSONObject and SerializeJSONArray to use this
   * method.  While SerializeJSONArray could use it without this move,
   * SerializeJSONObject must omit any |undefined|-valued property per so it
   * can't stream out a value using the SerializeJSONProperty method exactly as
   * defined by the spec.
   *   * We move step 12 into callers, again to ease streaming.
   */

  /* Step 8. */
  if (v.isString()) {
    return QuoteJSONString(cx, scx->sb, v.toString());
  }

  /* Step 5. */
  if (v.isNull()) {
    return scx->sb.append("null");
  }

  /* Steps 6-7. */
  if (v.isBoolean()) {
    return v.toBoolean() ? scx->sb.append("true") : scx->sb.append("false");
  }

  /* Step 9. */
  if (v.isNumber()) {
    if (v.isDouble()) {
      if (!std::isfinite(v.toDouble())) {
        MOZ_ASSERT(!scx->maybeSafely,
                   "input JS::ToJSONMaybeSafely must not include "
                   "reachable non-finite numbers");
        return scx->sb.append("null");
      }
    }

    return NumberValueToStringBuffer(v, scx->sb);
  }

  /* Step 10. */
  if (v.isBigInt()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_BIGINT_NOT_SERIALIZABLE);
    return false;
  }

  AutoCheckRecursionLimit recursion(cx);
  if (!recursion.check(cx)) {
    return false;
  }

  /* Step 11. */
  MOZ_ASSERT(v.hasObjectPayload());
  RootedObject obj(cx, &v.getObjectPayload());

  /* https://tc39.es/proposal-json-parse-with-source/#sec-serializejsonproperty
   * Step 4a.*/
  if (JSString* rawJSON = MaybeGetRawJSON(cx, obj)) {
    return scx->sb.append(rawJSON);
  }

  MOZ_ASSERT(
      !scx->maybeSafely || obj->is<PlainObject>() || obj->is<ArrayObject>(),
      "input to JS::ToJSONMaybeSafely must not include reachable "
      "objects that are neither arrays nor plain objects");

  scx->depth++;
  auto dec = mozilla::MakeScopeExit([&] { scx->depth--; });

#ifdef ENABLE_RECORD_TUPLE
  if (v.isExtendedPrimitive()) {
    if (obj->is<RecordType>()) {
      return SerializeJSONObject<JOType::Record>(cx, obj, scx);
    }
    if (obj->is<TupleType>()) {
      return SerializeJSONArray(cx, obj, scx);
    }
    MOZ_CRASH("Unexpected extended primitive - boxes cannot be stringified.");
  }
#endif

  bool isArray;
  if (!IsArray(cx, obj, &isArray)) {
    return false;
  }

  return isArray ? SerializeJSONArray(cx, obj, scx)
                 : SerializeJSONObject(cx, obj, scx);
}

static bool CanFastStringifyObject(NativeObject* obj) {
  if (ClassCanHaveExtraEnumeratedProperties(obj->getClass())) {
    return false;
  }

  if (obj->is<ArrayObject>()) {
    // Arrays will look up all keys [0..length) so disallow anything that could
    // find those keys anywhere but in the dense elements.
    if (!IsPackedArray(obj) && ObjectMayHaveExtraIndexedProperties(obj)) {
      return false;
    }
  } else {
    // Non-Arrays will only look at own properties, but still disallow any
    // indexed properties other than in the dense elements because they would
    // require sorting.
    if (ObjectMayHaveExtraIndexedOwnProperties(obj)) {
      return false;
    }
  }

  // Only used for internal environment objects that should never be passed to
  // JSON.stringify.
  MOZ_ASSERT(!obj->getOpsLookupProperty());

#ifdef ENABLE_RECORD_TUPLE
  if (ObjectValue(*obj).isExtendedPrimitive()) {
    return false;
  }
#endif

  return true;
}

#define FOR_EACH_STRINGIFY_BAIL_REASON(MACRO) \
  MACRO(NO_REASON)                            \
  MACRO(INELIGIBLE_OBJECT)                    \
  MACRO(DEEP_RECURSION)                       \
  MACRO(NON_DATA_PROPERTY)                    \
  MACRO(TOO_MANY_PROPERTIES)                  \
  MACRO(BIGINT)                               \
  MACRO(API)                                  \
  MACRO(HAVE_REPLACER)                        \
  MACRO(HAVE_SPACE)                           \
  MACRO(PRIMITIVE)                            \
  MACRO(HAVE_TOJSON)                          \
  MACRO(IMPURE_LOOKUP)                        \
  MACRO(INTERRUPT)

enum class BailReason : uint8_t {
#define DECLARE_ENUM(name) name,
  FOR_EACH_STRINGIFY_BAIL_REASON(DECLARE_ENUM)
#undef DECLARE_ENUM
};

static const char* DescribeStringifyBailReason(BailReason whySlow) {
  switch (whySlow) {
#define ENUM_NAME(name)  \
  case BailReason::name: \
    return #name;
    FOR_EACH_STRINGIFY_BAIL_REASON(ENUM_NAME)
#undef ENUM_NAME
    default:
      return "Unknown";
  }
}

// Iterator over all the dense elements of an object. Used
// for both Arrays and non-Arrays.
class DenseElementsIteratorForJSON {
  HeapSlotArray elements;
  uint32_t element;

  // Arrays can have a length less than getDenseInitializedLength(), in which
  // case the remaining Array elements are treated as UndefinedValue.
  uint32_t numElements;
  uint32_t length;

 public:
  explicit DenseElementsIteratorForJSON(NativeObject* nobj)
      : elements(nobj->getDenseElements()),
        element(0),
        numElements(nobj->getDenseInitializedLength()) {
    length = nobj->is<ArrayObject>() ? nobj->as<ArrayObject>().length()
                                     : numElements;
  }

  bool done() const { return element == length; }

  Value next() {
    // For Arrays, steps 6-8 of
    // https://262.ecma-international.org/14.0/#sec-serializejsonarray. For
    // non-Arrays, step 6a of
    // https://262.ecma-international.org/14.0/#sec-serializejsonobject
    // following the order from
    // https://262.ecma-international.org/14.0/#sec-ordinaryownpropertykeys

    MOZ_ASSERT(!done());
    auto i = element++;
    // Consider specializing the iterator for Arrays vs non-Arrays to avoid this
    // branch.
    return i < numElements ? elements.begin()[i] : UndefinedValue();
  }

  uint32_t getIndex() const { return element; }
};

// An iterator over the non-element properties of a Shape, returned in forward
// (creation) order. Note that it is fallible, so after iteration is complete
// isOverflowed() should be called to verify that the results are actually
// complete.

class ShapePropertyForwardIterNoGC {
  // Pointer to the current PropMap with length and an index within it.
  PropMap* map_;
  uint32_t mapLength_;
  uint32_t i_ = 0;

  // Stack of PropMaps to iterate through, oldest properties on top. The current
  // map (map_, above) is never on this stack.
  mozilla::Vector<PropMap*> stack_;

  const NativeShape* shape_;

  MOZ_ALWAYS_INLINE void settle() {
    while (true) {
      if (MOZ_UNLIKELY(i_ == mapLength_)) {
        i_ = 0;
        if (stack_.empty()) {
          mapLength_ = 0;  // Done
          return;
        }
        map_ = stack_.back();
        stack_.popBack();
        mapLength_ =
            stack_.empty() ? shape_->propMapLength() : PropMap::Capacity;
      } else if (MOZ_UNLIKELY(shape_->isDictionary() && !map_->hasKey(i_))) {
        // Dictionary maps can have "holes" for removed properties, so keep
        // going until we find a non-hole slot.
        i_++;
      } else {
        return;
      }
    }
  }

 public:
  explicit ShapePropertyForwardIterNoGC(NativeShape* shape) : shape_(shape) {
    // Set map_ to the PropMap containing the first property (the deepest map in
    // the previous() chain). Push pointers to all other PropMaps onto stack_.
    map_ = shape->propMap();
    if (!map_) {
      // No properties.
      i_ = mapLength_ = 0;
      return;
    }
    while (map_->hasPrevious()) {
      if (!stack_.append(map_)) {
        // Overflowed.
        i_ = mapLength_ = UINT32_MAX;
        return;
      }
      map_ = map_->asLinked()->previous();
    }

    // Set mapLength_ to the number of properties in map_ (including dictionary
    // holes, if any.)
    mapLength_ = stack_.empty() ? shape_->propMapLength() : PropMap::Capacity;

    settle();
  }

  bool done() const { return i_ == mapLength_; }
  bool isOverflowed() const { return i_ == UINT32_MAX; }

  void operator++(int) {
    MOZ_ASSERT(!done());
    i_++;
    settle();
  }

  PropertyInfoWithKey get() const {
    MOZ_ASSERT(!done());
    return map_->getPropertyInfoWithKey(i_);
  }

  PropertyInfoWithKey operator*() const { return get(); }

  // Fake pointer struct to make operator-> work.
  // See https://stackoverflow.com/a/52856349.
  struct FakePtr {
    PropertyInfoWithKey val_;
    const PropertyInfoWithKey* operator->() const { return &val_; }
  };
  FakePtr operator->() const { return {get()}; }
};

// Iterator over EnumerableOwnProperties
// https://262.ecma-international.org/14.0/#sec-enumerableownproperties
// that fails if it encounters any accessor properties, as they are not handled
// by JSON FastSerializeJSONProperty, or if it sees too many properties on one
// object.
class OwnNonIndexKeysIterForJSON {
  ShapePropertyForwardIterNoGC shapeIter;
  bool done_ = false;
  BailReason fastFailed_ = BailReason::NO_REASON;

  void settle() {
    // Skip over any non-enumerable or Symbol properties, and permanently fail
    // if any enumerable non-data properties are encountered.
    for (; !shapeIter.done(); shapeIter++) {
      if (!shapeIter->enumerable()) {
        continue;
      }
      if (!shapeIter->isDataProperty()) {
        fastFailed_ = BailReason::NON_DATA_PROPERTY;
        done_ = true;
        return;
      }
      PropertyKey id = shapeIter->key();
      if (!id.isSymbol()) {
        return;
      }
    }
    done_ = true;
  }

 public:
  explicit OwnNonIndexKeysIterForJSON(const NativeObject* nobj)
      : shapeIter(nobj->shape()) {
    if (MOZ_UNLIKELY(shapeIter.isOverflowed())) {
      fastFailed_ = BailReason::TOO_MANY_PROPERTIES;
      done_ = true;
      return;
    }
    if (!nobj->hasEnumerableProperty()) {
      // Non-Arrays with no enumerable properties can just be skipped.
      MOZ_ASSERT(!nobj->is<ArrayObject>());
      done_ = true;
      return;
    }
    settle();
  }

  bool done() const { return done_ || shapeIter.done(); }
  BailReason cannotFastStringify() const { return fastFailed_; }

  PropertyInfoWithKey next() {
    MOZ_ASSERT(!done());
    PropertyInfoWithKey prop = shapeIter.get();
    shapeIter++;
    settle();
    return prop;
  }
};

// Steps from https://262.ecma-international.org/14.0/#sec-serializejsonproperty
static bool EmitSimpleValue(JSContext* cx, StringBuffer& sb, const Value& v) {
  /* Step 8. */
  if (v.isString()) {
    return QuoteJSONString(cx, sb, v.toString());
  }

  /* Step 5. */
  if (v.isNull()) {
    return sb.append("null");
  }

  /* Steps 6-7. */
  if (v.isBoolean()) {
    return v.toBoolean() ? sb.append("true") : sb.append("false");
  }

  /* Step 9. */
  if (v.isNumber()) {
    if (v.isDouble()) {
      if (!std::isfinite(v.toDouble())) {
        return sb.append("null");
      }
    }

    return NumberValueToStringBuffer(v, sb);
  }

  // Unrepresentable values.
  if (v.isUndefined() || v.isMagic()) {
    MOZ_ASSERT_IF(v.isMagic(), v.isMagic(JS_ELEMENTS_HOLE));
    return sb.append("null");
  }

  /* Step 10. */
  MOZ_CRASH("should have validated printable simple value already");
}

// https://262.ecma-international.org/14.0/#sec-serializejsonproperty step 8b
// where K is an integer index.
static bool EmitQuotedIndexColon(StringBuffer& sb, uint32_t index) {
  Int32ToCStringBuf cbuf;
  size_t cstrlen;
  const char* cstr = ::Int32ToCString(&cbuf, index, &cstrlen);
  if (!sb.reserve(sb.length() + 1 + cstrlen + 1 + 1)) {
    return false;
  }
  sb.infallibleAppend('"');
  sb.infallibleAppend(cstr, cstrlen);
  sb.infallibleAppend('"');
  sb.infallibleAppend(':');
  return true;
}

// Similar to PreprocessValue: replace the value with a simpler one to
// stringify, but also detect whether the value is compatible with the fast
// path. If not, bail out by setting *whySlow and returning true.
static bool PreprocessFastValue(JSContext* cx, Value* vp, StringifyContext* scx,
                                BailReason* whySlow) {
  MOZ_ASSERT(!scx->maybeSafely);

  // Steps are from
  // https://262.ecma-international.org/14.0/#sec-serializejsonproperty

  // Disallow BigInts to avoid caring about BigInt.prototype.toJSON.
  if (vp->isBigInt()) {
    *whySlow = BailReason::BIGINT;
    return true;
  }

  if (!vp->isObject()) {
    return true;
  }

  if (!vp->toObject().is<NativeObject>()) {
    *whySlow = BailReason::INELIGIBLE_OBJECT;
    return true;
  }

  // Step 2: lookup a .toJSON property (and bail if found).
  NativeObject* obj = &vp->toObject().as<NativeObject>();
  PropertyResult toJSON;
  NativeObject* holder;
  PropertyKey id = NameToId(cx->names().toJSON);
  if (!NativeLookupPropertyInline<NoGC, LookupResolveMode::CheckMayResolve>(
          cx, obj, id, &holder, &toJSON)) {
    // Looking up this property would require a side effect.
    *whySlow = BailReason::IMPURE_LOOKUP;
    return true;
  }
  if (toJSON.isFound()) {
    *whySlow = BailReason::HAVE_TOJSON;
    return true;
  }

  // Step 4: convert primitive wrapper objects to primitives. Disallowed for
  // fast path.
  if (obj->is<NumberObject>() || obj->is<StringObject>() ||
      obj->is<BooleanObject>() || obj->is<BigIntObject>() ||
      IF_RECORD_TUPLE(obj->is<RecordObject>() || obj->is<TupleObject>(),
                      false)) {
    // Primitive wrapper objects can invoke arbitrary code when being coerced to
    // their primitive values (eg via @@toStringTag).
    *whySlow = BailReason::INELIGIBLE_OBJECT;
    return true;
  }

  if (obj->isCallable()) {
    // Steps 11,12: Callable objects are treated as undefined.
    vp->setUndefined();
    return true;
  }

  if (!CanFastStringifyObject(obj)) {
    *whySlow = BailReason::INELIGIBLE_OBJECT;
    return true;
  }

  return true;
}

// FastSerializeJSONProperty maintains an explicit stack to handle nested
// objects. For each object, first the dense elements are iterated, then the
// named properties (included sparse indexes, which will cause
// FastSerializeJSONProperty to bail out.)
//
// The iterators for each of those parts are not merged into a single common
// iterator because the interface is different for the two parts, and they are
// handled separately in the FastSerializeJSONProperty code.
struct FastStackEntry {
  NativeObject* nobj;
  Variant<DenseElementsIteratorForJSON, OwnNonIndexKeysIterForJSON> iter;
  bool isArray;  // Cached nobj->is<ArrayObject>()

  // Given an object, a FastStackEntry starts with the dense elements. The
  // caller is expected to inspect the variant to use it differently based on
  // which iterator is active.
  explicit FastStackEntry(NativeObject* obj)
      : nobj(obj),
        iter(AsVariant(DenseElementsIteratorForJSON(obj))),
        isArray(obj->is<ArrayObject>()) {}

  // Called by Vector when moving data around.
  FastStackEntry(FastStackEntry&& other) noexcept
      : nobj(other.nobj), iter(std::move(other.iter)), isArray(other.isArray) {}

  // Move assignment, called when updating the `top` entry.
  void operator=(FastStackEntry&& other) noexcept {
    nobj = other.nobj;
    iter = std::move(other.iter);
    isArray = other.isArray;
  }

  // Advance from dense elements to the named properties.
  void advanceToProperties() {
    iter = AsVariant(OwnNonIndexKeysIterForJSON(nobj));
  }
};

/* https://262.ecma-international.org/14.0/#sec-serializejsonproperty */
static bool FastSerializeJSONProperty(JSContext* cx, Handle<Value> v,
                                      StringifyContext* scx,
                                      BailReason* whySlow) {
  MOZ_ASSERT(*whySlow == BailReason::NO_REASON);
  MOZ_ASSERT(v.isObject());

  if (JSString* rawJSON = MaybeGetRawJSON(cx, &v.toObject())) {
    return scx->sb.append(rawJSON);
  }

  /*
   * FastSerializeJSONProperty is an optimistic fast path for the
   * SerializeJSONProperty algorithm that applies in limited situations. It
   * falls back to SerializeJSONProperty() if:
   *
   *   * Any externally visible code attempts to run: getter, enumerate
   *     hook, toJSON property.
   *   * Sparse index found (this would require accumulating props and sorting.)
   *   * Max stack depth is reached. (This will also detect self-referential
   *     input.)
   *
   *  Algorithm:
   *
   *    stack = []
   *    top = iter(obj)
   *    wroteMember = false
   *    OUTER: while true:
   *      if !wroteMember:
   *        emit("[" or "{")
   *      while !top.done():
   *        key, value = top.next()
   *        if top is a non-Array and value is skippable:
   *          continue
   *        if wroteMember:
   *          emit(",")
   *        wroteMember = true
   *        if value is object:
   *          emit(key + ":") if top is iterating a non-Array
   *          stack.push(top)
   *          top <- value
   *          wroteMember = false
   *          continue OUTER
   *        else:
   *          emit(value) or emit(key + ":" + value)
   *      emit("]" or "}")
   *      if stack is empty: done!
   *      top <- stack.pop()
   *      wroteMember = true
   *
   * except:
   *
   *   * The `while !top.done()` loop is split into the dense element portion
   *     and the slot portion. Each is iterated to completion before advancing
   *     or finishing.
   *
   *   * For Arrays, the named properties are not output, but they are still
   *     scanned to bail if any numeric keys are found that could be indexes.
   */

  // FastSerializeJSONProperty will bail if an interrupt is requested in the
  // middle of an operation, so handle any interrupts now before starting. Note:
  // this can GC, but after this point nothing should be able to GC unless
  // something fails, so rooting is unnecessary.
  if (!CheckForInterrupt(cx)) {
    return false;
  }

  constexpr size_t MAX_STACK_DEPTH = 20;
  Vector<FastStackEntry> stack(cx);
  if (!stack.reserve(MAX_STACK_DEPTH - 1)) {
    return false;
  }
  // Construct an iterator for the object,
  // https://262.ecma-international.org/14.0/#sec-serializejsonobject step 6:
  // EnumerableOwnPropertyNames or
  // https://262.ecma-international.org/14.0/#sec-serializejsonarray step 7-8.
  FastStackEntry top(&v.toObject().as<NativeObject>());
  bool wroteMember = false;

  if (!CanFastStringifyObject(top.nobj)) {
    *whySlow = BailReason::INELIGIBLE_OBJECT;
    return true;
  }

  while (true) {
    if (!wroteMember) {
      if (!scx->sb.append(top.isArray ? '[' : '{')) {
        return false;
      }
    }

    if (top.iter.is<DenseElementsIteratorForJSON>()) {
      auto& iter = top.iter.as<DenseElementsIteratorForJSON>();
      bool nestedObject = false;
      while (!iter.done()) {
        // Interrupts can GC and we are working with unrooted pointers.
        if (cx->hasPendingInterrupt(InterruptReason::CallbackUrgent) ||
            cx->hasPendingInterrupt(InterruptReason::CallbackCanWait)) {
          *whySlow = BailReason::INTERRUPT;
          return true;
        }

        uint32_t index = iter.getIndex();
        Value val = iter.next();

        if (!PreprocessFastValue(cx, &val, scx, whySlow)) {
          return false;
        }
        if (*whySlow != BailReason::NO_REASON) {
          return true;
        }
        if (IsFilteredValue(val)) {
          if (top.isArray) {
            // Arrays convert unrepresentable values to "null".
            val = UndefinedValue();
          } else {
            // Objects skip unrepresentable values.
            continue;
          }
        }

        if (wroteMember && !scx->sb.append(',')) {
          return false;
        }
        wroteMember = true;

        if (!top.isArray) {
          if (!EmitQuotedIndexColon(scx->sb, index)) {
            return false;
          }
        }

        if (val.isObject()) {
          if (JSString* rawJSON = MaybeGetRawJSON(cx, &val.toObject())) {
            if (!scx->sb.append(rawJSON)) {
              return false;
            }
          } else {
            if (stack.length() >= MAX_STACK_DEPTH - 1) {
              *whySlow = BailReason::DEEP_RECURSION;
              return true;
            }
            // Save the current iterator position on the stack and
            // switch to processing the nested value.
            stack.infallibleAppend(std::move(top));
            top = FastStackEntry(&val.toObject().as<NativeObject>());
            wroteMember = false;
            nestedObject = true;  // Break out to the outer loop.
            break;
          }
        } else if (!EmitSimpleValue(cx, scx->sb, val)) {
          return false;
        }
      }

      if (nestedObject) {
        continue;  // Break out to outer loop.
      }

      MOZ_ASSERT(iter.done());
      if (top.isArray) {
        MOZ_ASSERT(!top.nobj->isIndexed() || IsPackedArray(top.nobj));
      } else {
        top.advanceToProperties();
      }
    }

    if (top.iter.is<OwnNonIndexKeysIterForJSON>()) {
      auto& iter = top.iter.as<OwnNonIndexKeysIterForJSON>();
      bool nesting = false;
      while (!iter.done()) {
        // Interrupts can GC and we are working with unrooted pointers.
        if (cx->hasPendingInterrupt(InterruptReason::CallbackUrgent) ||
            cx->hasPendingInterrupt(InterruptReason::CallbackCanWait)) {
          *whySlow = BailReason::INTERRUPT;
          return true;
        }

        PropertyInfoWithKey prop = iter.next();

        // A non-Array with indexed elements would need to sort the indexes
        // numerically, which this code does not support. These objects are
        // skipped when obj->isIndexed(), so no index properties should be found
        // here.
        mozilla::DebugOnly<uint32_t> index = -1;
        MOZ_ASSERT(!IdIsIndex(prop.key(), &index));

        Value val = top.nobj->getSlot(prop.slot());
        if (!PreprocessFastValue(cx, &val, scx, whySlow)) {
          return false;
        }
        if (*whySlow != BailReason::NO_REASON) {
          return true;
        }
        if (IsFilteredValue(val)) {
          // Undefined check in
          // https://262.ecma-international.org/14.0/#sec-serializejsonobject
          // step 8b, covering undefined, symbol
          continue;
        }

        if (wroteMember && !scx->sb.append(",")) {
          return false;
        }
        wroteMember = true;

        MOZ_ASSERT(prop.key().isString());
        if (!QuoteJSONString(cx, scx->sb, prop.key().toString())) {
          return false;
        }

        if (!scx->sb.append(':')) {
          return false;
        }
        if (val.isObject()) {
          if (JSString* rawJSON = MaybeGetRawJSON(cx, &val.toObject())) {
            if (!scx->sb.append(rawJSON)) {
              return false;
            }
          } else {
            if (stack.length() >= MAX_STACK_DEPTH - 1) {
              *whySlow = BailReason::DEEP_RECURSION;
              return true;
            }
            // Save the current iterator position on the stack and
            // switch to processing the nested value.
            stack.infallibleAppend(std::move(top));
            top = FastStackEntry(&val.toObject().as<NativeObject>());
            wroteMember = false;
            nesting = true;  // Break out to the outer loop.
            break;
          }
        } else if (!EmitSimpleValue(cx, scx->sb, val)) {
          return false;
        }
      }
      *whySlow = iter.cannotFastStringify();
      if (*whySlow != BailReason::NO_REASON) {
        return true;
      }
      if (nesting) {
        continue;  // Break out to outer loop.
      }
      MOZ_ASSERT(iter.done());
    }

    if (!scx->sb.append(top.isArray ? ']' : '}')) {
      return false;
    }
    if (stack.empty()) {
      return true;  // Success!
    }
    top = std::move(stack.back());

    stack.popBack();
    wroteMember = true;
  }
}

/* https://262.ecma-international.org/14.0/#sec-json.stringify */
bool js::Stringify(JSContext* cx, MutableHandleValue vp, JSObject* replacer_,
                   const Value& space_, StringBuffer& sb,
                   StringifyBehavior stringifyBehavior) {
  RootedObject replacer(cx, replacer_);
  RootedValue space(cx, space_);

  MOZ_ASSERT_IF(stringifyBehavior == StringifyBehavior::RestrictedSafe,
                space.isNull());
  MOZ_ASSERT_IF(stringifyBehavior == StringifyBehavior::RestrictedSafe,
                vp.isObject());
  /**
   * This uses MOZ_ASSERT, since it's actually asserting something jsapi
   * consumers could get wrong, so needs a better error message.
   */
  MOZ_ASSERT(stringifyBehavior != StringifyBehavior::RestrictedSafe ||
                 vp.toObject().is<PlainObject>() ||
                 vp.toObject().is<ArrayObject>(),
             "input to JS::ToJSONMaybeSafely must be a plain object or array");

  /* Step 5. */
  RootedIdVector propertyList(cx);
  BailReason whySlow = BailReason::NO_REASON;
  if (stringifyBehavior == StringifyBehavior::SlowOnly ||
      stringifyBehavior == StringifyBehavior::RestrictedSafe) {
    whySlow = BailReason::API;
  }
  if (replacer) {
    whySlow = BailReason::HAVE_REPLACER;
    bool isArray;
    if (replacer->isCallable()) {
      /* Step 5a(i): use replacer to transform values.  */
    } else if (!IsArray(cx, replacer, &isArray)) {
      return false;
    } else if (isArray) {
      /* Step 5b(ii). */

      /* Step 5b(ii)(2). */
      uint32_t len;
      if (!GetLengthPropertyForArrayLike(cx, replacer, &len)) {
        return false;
      }

      // Cap the initial size to a moderately small value.  This avoids
      // ridiculous over-allocation if an array with bogusly-huge length
      // is passed in.  If we end up having to add elements past this
      // size, the set will naturally resize to accommodate them.
      const uint32_t MaxInitialSize = 32;
      Rooted<GCHashSet<jsid>> idSet(
          cx, GCHashSet<jsid>(cx, std::min(len, MaxInitialSize)));

      /* Step 5b(ii)(3). */
      uint32_t k = 0;

      /* Step 5b(ii)(4). */
      RootedValue item(cx);
      for (; k < len; k++) {
        if (!CheckForInterrupt(cx)) {
          return false;
        }

        /* Step 5b(ii)(4)(a-b). */
        if (!GetElement(cx, replacer, k, &item)) {
          return false;
        }

        /* Step 5b(ii)(4)(c-g). */
        RootedId id(cx);
        if (item.isNumber() || item.isString()) {
          if (!PrimitiveValueToId<CanGC>(cx, item, &id)) {
            return false;
          }
        } else {
          ESClass cls;
          if (!GetClassOfValue(cx, item, &cls)) {
            return false;
          }

          if (cls != ESClass::String && cls != ESClass::Number) {
            continue;
          }

          JSAtom* atom = ToAtom<CanGC>(cx, item);
          if (!atom) {
            return false;
          }

          id.set(AtomToId(atom));
        }

        /* Step 5b(ii)(4)(g). */
        auto p = idSet.lookupForAdd(id);
        if (!p) {
          /* Step 5b(ii)(4)(g)(i). */
          if (!idSet.add(p, id) || !propertyList.append(id)) {
            return false;
          }
        }
      }
    } else {
      replacer = nullptr;
    }
  }

  /* Step 6. */
  if (space.isObject()) {
    RootedObject spaceObj(cx, &space.toObject());

    ESClass cls;
    if (!JS::GetBuiltinClass(cx, spaceObj, &cls)) {
      return false;
    }

    if (cls == ESClass::Number) {
      double d;
      if (!ToNumber(cx, space, &d)) {
        return false;
      }
      space = NumberValue(d);
    } else if (cls == ESClass::String) {
      JSString* str = ToStringSlow<CanGC>(cx, space);
      if (!str) {
        return false;
      }
      space = StringValue(str);
    }
  }

  StringBuffer gap(cx);

  if (space.isNumber()) {
    /* Step 7. */
    double d;
    MOZ_ALWAYS_TRUE(ToInteger(cx, space, &d));
    d = std::min(10.0, d);
    if (d >= 1 && !gap.appendN(' ', uint32_t(d))) {
      return false;
    }
  } else if (space.isString()) {
    /* Step 8. */
    JSLinearString* str = space.toString()->ensureLinear(cx);
    if (!str) {
      return false;
    }
    size_t len = std::min(size_t(10), str->length());
    if (!gap.appendSubstring(str, 0, len)) {
      return false;
    }
  } else {
    /* Step 9. */
    MOZ_ASSERT(gap.empty());
  }
  if (!gap.empty()) {
    whySlow = BailReason::HAVE_SPACE;
  }

  Rooted<PlainObject*> wrapper(cx);
  RootedId emptyId(cx, NameToId(cx->names().empty_));
  if (replacer && replacer->isCallable()) {
    // We can skip creating the initial wrapper object if no replacer
    // function is present.

    /* Step 10. */
    wrapper = NewPlainObject(cx);
    if (!wrapper) {
      return false;
    }

    /* Step 11. */
    if (!NativeDefineDataProperty(cx, wrapper, emptyId, vp, JSPROP_ENUMERATE)) {
      return false;
    }
  }

  /* Step 13. */
  Rooted<JSAtom*> fastJSON(cx);
  if (whySlow == BailReason::NO_REASON) {
    MOZ_ASSERT(propertyList.empty());
    MOZ_ASSERT(stringifyBehavior != StringifyBehavior::RestrictedSafe);
    StringifyContext scx(cx, sb, gap, nullptr, propertyList, false);
    if (!PreprocessFastValue(cx, vp.address(), &scx, &whySlow)) {
      return false;
    }
    if (!vp.isObject()) {
      // "Fast" stringify of primitives would create a wrapper object and thus
      // be slower than regular stringify.
      whySlow = BailReason::PRIMITIVE;
    }
    if (whySlow == BailReason::NO_REASON) {
      if (!FastSerializeJSONProperty(cx, vp, &scx, &whySlow)) {
        return false;
      }
      if (whySlow == BailReason::NO_REASON) {
        // Fast stringify succeeded!
        if (stringifyBehavior != StringifyBehavior::Compare) {
          return true;
        }
        fastJSON = scx.sb.finishAtom();
        if (!fastJSON) {
          return false;
        }
      }
      scx.sb.clear();  // Preserves allocated space.
    }
  }

  if (MOZ_UNLIKELY((stringifyBehavior == StringifyBehavior::FastOnly) &&
                   (whySlow != BailReason::NO_REASON))) {
    JS_ReportErrorASCII(cx, "JSON stringify failed mandatory fast path: %s",
                        DescribeStringifyBailReason(whySlow));
    return false;
  }

  // Slow, general path.

  StringifyContext scx(cx, sb, gap, replacer, propertyList,
                       stringifyBehavior == StringifyBehavior::RestrictedSafe);
  if (!PreprocessValue(cx, wrapper, HandleId(emptyId), vp, &scx)) {
    return false;
  }
  if (IsFilteredValue(vp)) {
    return true;
  }

  if (!SerializeJSONProperty(cx, vp, &scx)) {
    return false;
  }

  // For StringBehavior::Compare, when the fast path succeeded.
  if (MOZ_UNLIKELY(fastJSON)) {
    JSAtom* slowJSON = scx.sb.finishAtom();
    if (!slowJSON) {
      return false;
    }
    if (fastJSON != slowJSON) {
      MOZ_CRASH("JSON.stringify mismatch between fast and slow paths");
    }
    // Put the JSON back into the StringBuffer for returning.
    if (!sb.append(slowJSON)) {
      return false;
    }
  }

  return true;
}

/* https://262.ecma-international.org/14.0/#sec-internalizejsonproperty */
static bool InternalizeJSONProperty(
    JSContext* cx, HandleObject holder, HandleId name, HandleValue reviver,
    MutableHandle<ParseRecordObject> parseRecord, MutableHandleValue vp) {
  AutoCheckRecursionLimit recursion(cx);
  if (!recursion.check(cx)) {
    return false;
  }

  /* Step 1. */
  RootedValue val(cx);
  if (!GetProperty(cx, holder, holder, name, &val)) {
    return false;
  }

#ifdef ENABLE_JSON_PARSE_WITH_SOURCE
  RootedObject context(cx);
  Rooted<UniquePtr<ParseRecordObject::EntryMap>> entries(cx);
  if (JS::Prefs::experimental_json_parse_with_source()) {
    // https://tc39.es/proposal-json-parse-with-source/#sec-internalizejsonproperty
    bool sameVal = false;
    Rooted<Value> parsedValue(cx, parseRecord.get().value);
    if (!SameValue(cx, parsedValue, val, &sameVal)) {
      return false;
    }
    if (!parseRecord.get().isEmpty() && sameVal) {
      if (parseRecord.get().parseNode) {
        MOZ_ASSERT(!val.isObject());
        Rooted<IdValueVector> props(cx, cx);
        if (!props.emplaceBack(
                IdValuePair(NameToId(cx->names().source),
                            StringValue(parseRecord.get().parseNode)))) {
          return false;
        }
        context = NewPlainObjectWithUniqueNames(cx, props);
        if (!context) {
          return false;
        }
      }
      entries = std::move(parseRecord.get().entries);
    }
    if (!context) {
      context = NewPlainObject(cx);
      if (!context) {
        return false;
      }
    }
  }
#endif

  /* Step 2. */
  if (val.isObject()) {
    RootedObject obj(cx, &val.toObject());

    bool isArray;
    if (!IsArray(cx, obj, &isArray)) {
      return false;
    }

    if (isArray) {
      /* Step 2b(i). */
      uint32_t length;
      if (!GetLengthPropertyForArrayLike(cx, obj, &length)) {
        return false;
      }

      /* Steps 2b(ii-iii). */
      RootedId id(cx);
      RootedValue newElement(cx);
      for (uint32_t i = 0; i < length; i++) {
        if (!CheckForInterrupt(cx)) {
          return false;
        }

        if (!IndexToId(cx, i, &id)) {
          return false;
        }

        /* Step 2a(iii)(1). */
        Rooted<ParseRecordObject> elementRecord(cx);
#ifdef ENABLE_JSON_PARSE_WITH_SOURCE
        if (entries) {
          if (auto entry = entries->lookup(id)) {
            elementRecord = std::move(entry->value());
          }
        }
#endif
        if (!InternalizeJSONProperty(cx, obj, id, reviver, &elementRecord,
                                     &newElement)) {
          return false;
        }

        ObjectOpResult ignored;
        if (newElement.isUndefined()) {
          /* Step 2b(iii)(3). The spec deliberately ignores strict failure. */
          if (!DeleteProperty(cx, obj, id, ignored)) {
            return false;
          }
        } else {
          /* Step 2b(iii)(4). The spec deliberately ignores strict failure. */
          Rooted<PropertyDescriptor> desc(
              cx, PropertyDescriptor::Data(newElement,
                                           {JS::PropertyAttribute::Configurable,
                                            JS::PropertyAttribute::Enumerable,
                                            JS::PropertyAttribute::Writable}));
          if (!DefineProperty(cx, obj, id, desc, ignored)) {
            return false;
          }
        }
      }
    } else {
      /* Step 2c(i). */
      RootedIdVector keys(cx);
      if (!GetPropertyKeys(cx, obj, JSITER_OWNONLY, &keys)) {
        return false;
      }

      /* Step 2c(ii). */
      RootedId id(cx);
      RootedValue newElement(cx);
      for (size_t i = 0, len = keys.length(); i < len; i++) {
        if (!CheckForInterrupt(cx)) {
          return false;
        }

        /* Step 2c(ii)(1). */
        id = keys[i];
        Rooted<ParseRecordObject> entryRecord(cx);
#ifdef ENABLE_JSON_PARSE_WITH_SOURCE
        if (entries) {
          if (auto entry = entries->lookup(id)) {
            entryRecord = std::move(entry->value());
          }
        }
#endif
        if (!InternalizeJSONProperty(cx, obj, id, reviver, &entryRecord,
                                     &newElement)) {
          return false;
        }

        ObjectOpResult ignored;
        if (newElement.isUndefined()) {
          /* Step 2c(ii)(2). The spec deliberately ignores strict failure. */
          if (!DeleteProperty(cx, obj, id, ignored)) {
            return false;
          }
        } else {
          /* Step 2c(ii)(3). The spec deliberately ignores strict failure. */
          Rooted<PropertyDescriptor> desc(
              cx, PropertyDescriptor::Data(newElement,
                                           {JS::PropertyAttribute::Configurable,
                                            JS::PropertyAttribute::Enumerable,
                                            JS::PropertyAttribute::Writable}));
          if (!DefineProperty(cx, obj, id, desc, ignored)) {
            return false;
          }
        }
      }
    }
  }

  /* Step 3. */
  RootedString key(cx, IdToString(cx, name));
  if (!key) {
    return false;
  }

  RootedValue keyVal(cx, StringValue(key));
#ifdef ENABLE_JSON_PARSE_WITH_SOURCE
  if (JS::Prefs::experimental_json_parse_with_source()) {
    RootedValue contextVal(cx, ObjectValue(*context));
    return js::Call(cx, reviver, holder, keyVal, val, contextVal, vp);
  }
#endif
  return js::Call(cx, reviver, holder, keyVal, val, vp);
}

static bool Revive(JSContext* cx, HandleValue reviver,
                   MutableHandle<ParseRecordObject> pro,
                   MutableHandleValue vp) {
  Rooted<PlainObject*> obj(cx, NewPlainObject(cx));
  if (!obj) {
    return false;
  }

  if (!DefineDataProperty(cx, obj, cx->names().empty_, vp)) {
    return false;
  }

#ifdef ENABLE_JSON_PARSE_WITH_SOURCE
  MOZ_ASSERT_IF(JS::Prefs::experimental_json_parse_with_source(),
                pro.get().value == vp.get());
#endif
  Rooted<jsid> id(cx, NameToId(cx->names().empty_));
  return InternalizeJSONProperty(cx, obj, id, reviver, pro, vp);
}

template <typename CharT>
bool ParseJSON(JSContext* cx, const mozilla::Range<const CharT> chars,
               MutableHandleValue vp) {
  Rooted<JSONParser<CharT>> parser(cx, cx, chars,
                                   JSONParser<CharT>::ParseType::JSONParse);
  return parser.parse(vp);
}

template <typename CharT>
bool js::ParseJSONWithReviver(JSContext* cx,
                              const mozilla::Range<const CharT> chars,
                              HandleValue reviver, MutableHandleValue vp) {
  /* https://262.ecma-international.org/14.0/#sec-json.parse steps 2-10. */
  Rooted<ParseRecordObject> pro(cx);
#ifdef ENABLE_JSON_PARSE_WITH_SOURCE
  if (JS::Prefs::experimental_json_parse_with_source() && IsCallable(reviver)) {
    Rooted<JSONReviveParser<CharT>> parser(cx, cx, chars);
    if (!parser.get().parse(vp, &pro)) {
      return false;
    }
  } else
#endif
      if (!ParseJSON(cx, chars, vp)) {
    return false;
  }

  /* Steps 11-12. */
  if (IsCallable(reviver)) {
    return Revive(cx, reviver, &pro, vp);
  }
  return true;
}

template bool js::ParseJSONWithReviver(
    JSContext* cx, const mozilla::Range<const Latin1Char> chars,
    HandleValue reviver, MutableHandleValue vp);

template bool js::ParseJSONWithReviver(
    JSContext* cx, const mozilla::Range<const char16_t> chars,
    HandleValue reviver, MutableHandleValue vp);

static bool json_toSource(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  args.rval().setString(cx->names().JSON);
  return true;
}

/* https://262.ecma-international.org/14.0/#sec-json.parse */
static bool json_parse(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSMethodProfilerEntry pseudoFrame(cx, "JSON", "parse");
  CallArgs args = CallArgsFromVp(argc, vp);

  /* Step 1. */
  JSString* str = (args.length() >= 1) ? ToString<CanGC>(cx, args[0])
                                       : cx->names().undefined;
  if (!str) {
    return false;
  }

  JSLinearString* linear = str->ensureLinear(cx);
  if (!linear) {
    return false;
  }

  AutoStableStringChars linearChars(cx);
  if (!linearChars.init(cx, linear)) {
    return false;
  }

  HandleValue reviver = args.get(1);

  /* Steps 2-12. */
  return linearChars.isLatin1()
             ? ParseJSONWithReviver(cx, linearChars.latin1Range(), reviver,
                                    args.rval())
             : ParseJSONWithReviver(cx, linearChars.twoByteRange(), reviver,
                                    args.rval());
}

#ifdef ENABLE_RECORD_TUPLE
bool BuildImmutableProperty(JSContext* cx, HandleValue value, HandleId name,
                            HandleValue reviver,
                            MutableHandleValue immutableRes) {
  MOZ_ASSERT(!name.isSymbol());

  // Step 1
  if (value.isObject()) {
    RootedValue childValue(cx), newElement(cx);
    RootedId childName(cx);

    // Step 1.a-1.b
    if (value.toObject().is<ArrayObject>()) {
      Rooted<ArrayObject*> arr(cx, &value.toObject().as<ArrayObject>());

      // Step 1.b.iii
      uint32_t len = arr->length();

      TupleType* tup = TupleType::createUninitialized(cx, len);
      if (!tup) {
        return false;
      }
      immutableRes.setExtendedPrimitive(*tup);

      // Step 1.b.iv
      for (uint32_t i = 0; i < len; i++) {
        // Step 1.b.iv.1
        childName.set(PropertyKey::Int(i));

        // Step 1.b.iv.2
        if (!GetProperty(cx, arr, value, childName, &childValue)) {
          return false;
        }

        // Step 1.b.iv.3
        if (!BuildImmutableProperty(cx, childValue, childName, reviver,
                                    &newElement)) {
          return false;
        }
        MOZ_ASSERT(newElement.isPrimitive());

        // Step 1.b.iv.5
        if (!tup->initializeNextElement(cx, newElement)) {
          return false;
        }
      }

      // Step 1.b.v
      tup->finishInitialization(cx);
    } else {
      RootedObject obj(cx, &value.toObject());

      // Step 1.c.i - We only get the property keys rather than the
      // entries, but the difference is not observable from user code
      // because `obj` is a plan object not exposed externally
      RootedIdVector props(cx);
      if (!GetPropertyKeys(cx, obj, JSITER_OWNONLY, &props)) {
        return false;
      }

      RecordType* rec = RecordType::createUninitialized(cx, props.length());
      if (!rec) {
        return false;
      }
      immutableRes.setExtendedPrimitive(*rec);

      for (uint32_t i = 0; i < props.length(); i++) {
        // Step 1.c.iii.1
        childName.set(props[i]);

        // Step 1.c.iii.2
        if (!GetProperty(cx, obj, value, childName, &childValue)) {
          return false;
        }

        // Step 1.c.iii.3
        if (!BuildImmutableProperty(cx, childValue, childName, reviver,
                                    &newElement)) {
          return false;
        }
        MOZ_ASSERT(newElement.isPrimitive());

        // Step 1.c.iii.5
        if (!newElement.isUndefined()) {
          // Step 1.c.iii.5.a-b
          rec->initializeNextProperty(cx, childName, newElement);
        }
      }

      // Step 1.c.iv
      rec->finishInitialization(cx);
    }
  } else {
    // Step 2.a
    immutableRes.set(value);
  }

  // Step 3
  if (IsCallable(reviver)) {
    RootedValue keyVal(cx, StringValue(IdToString(cx, name)));

    // Step 3.a
    if (!Call(cx, reviver, UndefinedHandleValue, keyVal, immutableRes,
              immutableRes)) {
      return false;
    }

    // Step 3.b
    if (!immutableRes.isPrimitive()) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_RECORD_TUPLE_NO_OBJECT);
      return false;
    }
  }

  return true;
}

static bool json_parseImmutable(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSMethodProfilerEntry pseudoFrame(cx, "JSON", "parseImmutable");
  CallArgs args = CallArgsFromVp(argc, vp);

  /* Step 1. */
  JSString* str = (args.length() >= 1) ? ToString<CanGC>(cx, args[0])
                                       : cx->names().undefined;
  if (!str) {
    return false;
  }

  JSLinearString* linear = str->ensureLinear(cx);
  if (!linear) {
    return false;
  }

  AutoStableStringChars linearChars(cx);
  if (!linearChars.init(cx, linear)) {
    return false;
  }

  HandleValue reviver = args.get(1);
  RootedValue unfiltered(cx);

  if (linearChars.isLatin1()) {
    if (!ParseJSON(cx, linearChars.latin1Range(), &unfiltered)) {
      return false;
    }
  } else {
    if (!ParseJSON(cx, linearChars.twoByteRange(), &unfiltered)) {
      return false;
    }
  }

  RootedId id(cx, NameToId(cx->names().empty_));
  return BuildImmutableProperty(cx, unfiltered, id, reviver, args.rval());
}
#endif

#ifdef ENABLE_JSON_PARSE_WITH_SOURCE
/* https://tc39.es/proposal-json-parse-with-source/#sec-json.israwjson */
static bool json_isRawJSON(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSMethodProfilerEntry pseudoFrame(cx, "JSON", "isRawJSON");
  CallArgs args = CallArgsFromVp(argc, vp);

  /* Step 1. */
  if (args.get(0).isObject()) {
    Rooted<JSObject*> obj(cx, &args[0].toObject());
#  ifdef DEBUG
    if (obj->is<RawJSONObject>()) {
      bool objIsFrozen = false;
      MOZ_ASSERT(js::TestIntegrityLevel(cx, obj, IntegrityLevel::Frozen,
                                        &objIsFrozen));
      MOZ_ASSERT(objIsFrozen);
    }
#  endif  // DEBUG
    args.rval().setBoolean(obj->is<RawJSONObject>());
    return true;
  }

  /* Step 2. */
  args.rval().setBoolean(false);
  return true;
}

static inline bool IsJSONWhitespace(char16_t ch) {
  return ch == '\t' || ch == '\n' || ch == '\r' || ch == ' ';
}

/* https://tc39.es/proposal-json-parse-with-source/#sec-json.rawjson */
static bool json_rawJSON(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSMethodProfilerEntry pseudoFrame(cx, "JSON", "rawJSON");
  CallArgs args = CallArgsFromVp(argc, vp);

  /* Step 1. */
  JSString* jsonString = ToString<CanGC>(cx, args.get(0));
  if (!jsonString) {
    return false;
  }

  Rooted<JSLinearString*> linear(cx, jsonString->ensureLinear(cx));
  if (!linear) {
    return false;
  }

  AutoStableStringChars linearChars(cx);
  if (!linearChars.init(cx, linear)) {
    return false;
  }

  /* Step 2. */
  if (linear->empty()) {
    JS_ReportErrorNumberASCII(cx, js::GetErrorMessage, nullptr,
                              JSMSG_JSON_RAW_EMPTY);
    return false;
  }
  if (IsJSONWhitespace(linear->latin1OrTwoByteChar(0)) ||
      IsJSONWhitespace(linear->latin1OrTwoByteChar(linear->length() - 1))) {
    JS_ReportErrorNumberASCII(cx, js::GetErrorMessage, nullptr,
                              JSMSG_JSON_RAW_WHITESPACE);
    return false;
  }

  /* Step 3. */
  RootedValue parsedValue(cx);
  if (linearChars.isLatin1()) {
    if (!ParseJSON(cx, linearChars.latin1Range(), &parsedValue)) {
      return false;
    }
  } else {
    if (!ParseJSON(cx, linearChars.twoByteRange(), &parsedValue)) {
      return false;
    }
  }

  if (parsedValue.isObject()) {
    JS_ReportErrorNumberASCII(cx, js::GetErrorMessage, nullptr,
                              JSMSG_JSON_RAW_ARRAY_OR_OBJECT);
    return false;
  }

  /* Steps 4-6. */
  Rooted<RawJSONObject*> obj(cx, RawJSONObject::create(cx, linear));
  if (!obj) {
    return false;
  }

  /* Step 7. */
  if (!js::FreezeObject(cx, obj)) {
    return false;
  }

  args.rval().setObject(*obj);
  return true;
}
#endif  // ENABLE_JSON_PARSE_WITH_SOURCE

/* https://262.ecma-international.org/14.0/#sec-json.stringify */
bool json_stringify(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSMethodProfilerEntry pseudoFrame(cx, "JSON", "stringify");
  CallArgs args = CallArgsFromVp(argc, vp);

  RootedObject replacer(cx,
                        args.get(1).isObject() ? &args[1].toObject() : nullptr);
  RootedValue value(cx, args.get(0));
  RootedValue space(cx, args.get(2));

#ifdef DEBUG
  StringifyBehavior behavior = StringifyBehavior::Compare;
#else
  StringifyBehavior behavior = StringifyBehavior::Normal;
#endif

  JSStringBuilder sb(cx);
  if (!Stringify(cx, &value, replacer, space, sb, behavior)) {
    return false;
  }

  // XXX This can never happen to nsJSON.cpp, but the JSON object
  // needs to support returning undefined. So this is a little awkward
  // for the API, because we want to support streaming writers.
  if (!sb.empty()) {
    JSString* str = sb.finishString();
    if (!str) {
      return false;
    }
    args.rval().setString(str);
  } else {
    args.rval().setUndefined();
  }

  return true;
}

static const JSFunctionSpec json_static_methods[] = {
    JS_FN("toSource", json_toSource, 0, 0),
    JS_FN("parse", json_parse, 2, 0),
    JS_FN("stringify", json_stringify, 3, 0),
#ifdef ENABLE_RECORD_TUPLE
    JS_FN("parseImmutable", json_parseImmutable, 2, 0),
#endif
#ifdef ENABLE_JSON_PARSE_WITH_SOURCE
    JS_FN("isRawJSON", json_isRawJSON, 1, 0),
    JS_FN("rawJSON", json_rawJSON, 1, 0),
#endif
    JS_FS_END};

static const JSPropertySpec json_static_properties[] = {
    JS_STRING_SYM_PS(toStringTag, "JSON", JSPROP_READONLY), JS_PS_END};

static JSObject* CreateJSONObject(JSContext* cx, JSProtoKey key) {
  RootedObject proto(cx, &cx->global()->getObjectPrototype());
  return NewTenuredObjectWithGivenProto(cx, &JSONClass, proto);
}

static const ClassSpec JSONClassSpec = {
    CreateJSONObject, nullptr, json_static_methods, json_static_properties};

const JSClass js::JSONClass = {"JSON", JSCLASS_HAS_CACHED_PROTO(JSProto_JSON),
                               JS_NULL_CLASS_OPS, &JSONClassSpec};

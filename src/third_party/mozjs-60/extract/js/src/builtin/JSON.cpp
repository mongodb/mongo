/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "builtin/JSON.h"

#include "mozilla/FloatingPoint.h"
#include "mozilla/Range.h"
#include "mozilla/ScopeExit.h"

#include "jsarray.h"
#include "jsnum.h"
#include "jstypes.h"
#include "jsutil.h"

#include "builtin/String.h"
#include "util/StringBuffer.h"
#include "vm/Interpreter.h"
#include "vm/JSAtom.h"
#include "vm/JSContext.h"
#include "vm/JSObject.h"
#include "vm/JSONParser.h"

#include "jsarrayinlines.h"
#include "jsboolinlines.h"

#include "vm/JSAtom-inl.h"
#include "vm/NativeObject-inl.h"

using namespace js;
using namespace js::gc;

using mozilla::IsFinite;
using mozilla::Maybe;
using mozilla::RangedPtr;

const Class js::JSONClass = {
    js_JSON_str,
    JSCLASS_HAS_CACHED_PROTO(JSProto_JSON)
};

/* ES5 15.12.3 Quote.
 * Requires that the destination has enough space allocated for src after escaping
 * (that is, `2 + 6 * (srcEnd - srcBegin)` characters).
 */
template <typename SrcCharT, typename DstCharT>
static MOZ_ALWAYS_INLINE RangedPtr<DstCharT>
InfallibleQuote(RangedPtr<const SrcCharT> srcBegin, RangedPtr<const SrcCharT> srcEnd, RangedPtr<DstCharT> dstPtr)
{
    // Maps characters < 256 to the value that must follow the '\\' in the quoted string.
    // Entries with 'u' are handled as \\u00xy, and entries with 0 are not escaped in any way.
    // Characters >= 256 are all assumed to be unescaped.
    static const Latin1Char escapeLookup[256] = {
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
    };

    /* Step 1. */
    *dstPtr++ = '"';

    /* Step 2. */
    while (srcBegin != srcEnd) {
        SrcCharT c = *srcBegin++;
        size_t escapeIndex = c % sizeof(escapeLookup);
        Latin1Char escaped = escapeLookup[escapeIndex];
        if (MOZ_LIKELY((escapeIndex != size_t(c)) || !escaped)) {
            *dstPtr++ = c;
            continue;
        }
        *dstPtr++ = '\\';
        *dstPtr++ = escaped;
        if (escaped == 'u') {
            MOZ_ASSERT(c < ' ');
            MOZ_ASSERT((c >> 4) < 10);
            uint8_t x = c >> 4, y = c % 16;
            *dstPtr++ = '0';
            *dstPtr++ = '0';
            *dstPtr++ = '0' + x;
            *dstPtr++ = y < 10 ? '0' + y : 'a' + (y - 10);
        }
    }

    /* Steps 3-4. */
    *dstPtr++ = '"';
    return dstPtr;
}

template <typename SrcCharT, typename CharVectorT>
static bool
Quote(CharVectorT& sb, JSLinearString* str)
{
    // We resize the backing buffer to the maximum size we could possibly need,
    // write the escaped string into it, and shrink it back to the size we ended
    // up needing.
    size_t len = str->length();
    size_t sbInitialLen = sb.length();
    if (!sb.growByUninitialized(len * 6 + 2))
        return false;

    typedef typename CharVectorT::ElementType DstCharT;

    JS::AutoCheckCannotGC nogc;
    RangedPtr<const SrcCharT> srcBegin{str->chars<SrcCharT>(nogc), len};
    RangedPtr<DstCharT> dstBegin{sb.begin(), sb.begin(), sb.end()};
    RangedPtr<DstCharT> dstEnd = InfallibleQuote(srcBegin, srcBegin + len, dstBegin + sbInitialLen);
    size_t newSize = dstEnd - dstBegin;
    sb.shrinkTo(newSize);
    return true;
}

static bool
Quote(JSContext* cx, StringBuffer& sb, JSString* str)
{
    JSLinearString* linear = str->ensureLinear(cx);
    if (!linear)
        return false;

    // Check if either has non-latin1 before calling ensure, so that the buffer's
    // hasEnsured flag is set if the converstion to twoByte was automatic.
    if (!sb.isUnderlyingBufferLatin1() || linear->hasTwoByteChars()) {
        if (!sb.ensureTwoByteChars())
            return false;
    }
    if (linear->hasTwoByteChars())
        return Quote<char16_t>(sb.rawTwoByteBuffer(), linear);

    return sb.isUnderlyingBufferLatin1()
           ? Quote<Latin1Char>(sb.latin1Chars(), linear)
           : Quote<Latin1Char>(sb.rawTwoByteBuffer(), linear);
}

namespace {

using ObjectVector = GCVector<JSObject*, 8>;

class StringifyContext
{
  public:
    StringifyContext(JSContext* cx, StringBuffer& sb, const StringBuffer& gap,
                     HandleObject replacer, const AutoIdVector& propertyList,
                     bool maybeSafely)
      : sb(sb),
        gap(gap),
        replacer(cx, replacer),
        stack(cx, ObjectVector(cx)),
        propertyList(propertyList),
        depth(0),
        maybeSafely(maybeSafely)
    {
        MOZ_ASSERT_IF(maybeSafely, !replacer);
        MOZ_ASSERT_IF(maybeSafely, gap.empty());
    }

    StringBuffer& sb;
    const StringBuffer& gap;
    RootedObject replacer;
    Rooted<ObjectVector> stack;
    const AutoIdVector& propertyList;
    uint32_t depth;
    bool maybeSafely;
};

} /* anonymous namespace */

static bool Str(JSContext* cx, const Value& v, StringifyContext* scx);

static bool
WriteIndent(StringifyContext* scx, uint32_t limit)
{
    if (!scx->gap.empty()) {
        if (!scx->sb.append('\n'))
            return false;

        if (scx->gap.isUnderlyingBufferLatin1()) {
            for (uint32_t i = 0; i < limit; i++) {
                if (!scx->sb.append(scx->gap.rawLatin1Begin(), scx->gap.rawLatin1End()))
                    return false;
            }
        } else {
            for (uint32_t i = 0; i < limit; i++) {
                if (!scx->sb.append(scx->gap.rawTwoByteBegin(), scx->gap.rawTwoByteEnd()))
                    return false;
            }
        }
    }

    return true;
}

namespace {

template<typename KeyType>
class KeyStringifier {
};

template<>
class KeyStringifier<uint32_t> {
  public:
    static JSString* toString(JSContext* cx, uint32_t index) {
        return IndexToString(cx, index);
    }
};

template<>
class KeyStringifier<HandleId> {
  public:
    static JSString* toString(JSContext* cx, HandleId id) {
        return IdToString(cx, id);
    }
};

} /* anonymous namespace */

/*
 * ES5 15.12.3 Str, steps 2-4, extracted to enable preprocessing of property
 * values when stringifying objects in JO.
 */
template<typename KeyType>
static bool
PreprocessValue(JSContext* cx, HandleObject holder, KeyType key, MutableHandleValue vp, StringifyContext* scx)
{
    // We don't want to do any preprocessing here if scx->maybeSafely,
    // since the stuff we do here can have side-effects.
    if (scx->maybeSafely)
        return true;

    RootedString keyStr(cx);

    /* Step 2. */
    if (vp.isObject()) {
        RootedValue toJSON(cx);
        RootedObject obj(cx, &vp.toObject());
        if (!GetProperty(cx, obj, obj, cx->names().toJSON, &toJSON))
            return false;

        if (IsCallable(toJSON)) {
            keyStr = KeyStringifier<KeyType>::toString(cx, key);
            if (!keyStr)
                return false;

            RootedValue arg0(cx, StringValue(keyStr));
            if (!js::Call(cx, toJSON, vp, arg0, vp))
                return false;
        }
    }

    /* Step 3. */
    if (scx->replacer && scx->replacer->isCallable()) {
        MOZ_ASSERT(holder != nullptr, "holder object must be present when replacer is callable");

        if (!keyStr) {
            keyStr = KeyStringifier<KeyType>::toString(cx, key);
            if (!keyStr)
                return false;
        }

        RootedValue arg0(cx, StringValue(keyStr));
        RootedValue replacerVal(cx, ObjectValue(*scx->replacer));
        if (!js::Call(cx, replacerVal, holder, arg0, vp, vp))
            return false;
    }

    /* Step 4. */
    if (vp.get().isObject()) {
        RootedObject obj(cx, &vp.get().toObject());

        ESClass cls;
        if (!GetBuiltinClass(cx, obj, &cls))
            return false;

        if (cls == ESClass::Number) {
            double d;
            if (!ToNumber(cx, vp, &d))
                return false;
            vp.setNumber(d);
        } else if (cls == ESClass::String) {
            JSString* str = ToStringSlow<CanGC>(cx, vp);
            if (!str)
                return false;
            vp.setString(str);
        } else if (cls == ESClass::Boolean) {
            if (!Unbox(cx, obj, vp))
                return false;
        }
    }

    return true;
}

/*
 * Determines whether a value which has passed by ES5 150.2.3 Str steps 1-4's
 * gauntlet will result in Str returning |undefined|.  This function is used to
 * properly omit properties resulting in such values when stringifying objects,
 * while properly stringifying such properties as null when they're encountered
 * in arrays.
 */
static inline bool
IsFilteredValue(const Value& v)
{
    return v.isUndefined() || v.isSymbol() || IsCallable(v);
}

class CycleDetector
{
  public:
    CycleDetector(StringifyContext* scx, HandleObject obj)
      : stack_(&scx->stack), obj_(obj), appended_(false) {
    }

    MOZ_ALWAYS_INLINE bool foundCycle(JSContext* cx) {
        JSObject* obj = obj_;
        for (JSObject* obj2 : stack_) {
            if (MOZ_UNLIKELY(obj == obj2)) {
                JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_JSON_CYCLIC_VALUE);
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

/* ES5 15.12.3 JO. */
static bool
JO(JSContext* cx, HandleObject obj, StringifyContext* scx)
{
    /*
     * This method implements the JO algorithm in ES5 15.12.3, but:
     *
     *   * The algorithm is somewhat reformulated to allow the final string to
     *     be streamed into a single buffer, rather than be created and copied
     *     into place incrementally as the ES5 algorithm specifies it.  This
     *     requires moving portions of the Str call in 8a into this algorithm
     *     (and in JA as well).
     */

    MOZ_ASSERT_IF(scx->maybeSafely, obj->is<PlainObject>());

    /* Steps 1-2, 11. */
    CycleDetector detect(scx, obj);
    if (!detect.foundCycle(cx))
        return false;

    if (!scx->sb.append('{'))
        return false;

    /* Steps 5-7. */
    Maybe<AutoIdVector> ids;
    const AutoIdVector* props;
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
        if (!GetPropertyKeys(cx, obj, JSITER_OWNONLY, ids.ptr()))
            return false;
        props = ids.ptr();
    }

    /* My kingdom for not-quite-initialized-from-the-start references. */
    const AutoIdVector& propertyList = *props;

    /* Steps 8-10, 13. */
    bool wroteMember = false;
    RootedId id(cx);
    for (size_t i = 0, len = propertyList.length(); i < len; i++) {
        if (!CheckForInterrupt(cx))
            return false;

        /*
         * Steps 8a-8b.  Note that the call to Str is broken up into 1) getting
         * the property; 2) processing for toJSON, calling the replacer, and
         * handling boxed Number/String/Boolean objects; 3) filtering out
         * values which process to |undefined|, and 4) stringifying all values
         * which pass the filter.
         */
        id = propertyList[i];
        RootedValue outputValue(cx);
#ifdef DEBUG
        if (scx->maybeSafely) {
            RootedNativeObject nativeObj(cx, &obj->as<NativeObject>());
            Rooted<PropertyResult> prop(cx);
            NativeLookupOwnPropertyNoResolve(cx, nativeObj, id, &prop);
            MOZ_ASSERT(prop && prop.isNativeProperty() && prop.shape()->isDataDescriptor());
        }
#endif // DEBUG
        if (!GetProperty(cx, obj, obj, id, &outputValue))
            return false;
        if (!PreprocessValue(cx, obj, HandleId(id), &outputValue, scx))
            return false;
        if (IsFilteredValue(outputValue))
            continue;

        /* Output a comma unless this is the first member to write. */
        if (wroteMember && !scx->sb.append(','))
            return false;
        wroteMember = true;

        if (!WriteIndent(scx, scx->depth))
            return false;

        JSString* s = IdToString(cx, id);
        if (!s)
            return false;

        if (!Quote(cx, scx->sb, s) ||
            !scx->sb.append(':') ||
            !(scx->gap.empty() || scx->sb.append(' ')) ||
            !Str(cx, outputValue, scx))
        {
            return false;
        }
    }

    if (wroteMember && !WriteIndent(scx, scx->depth - 1))
        return false;

    return scx->sb.append('}');
}

/* ES5 15.12.3 JA. */
static bool
JA(JSContext* cx, HandleObject obj, StringifyContext* scx)
{
    /*
     * This method implements the JA algorithm in ES5 15.12.3, but:
     *
     *   * The algorithm is somewhat reformulated to allow the final string to
     *     be streamed into a single buffer, rather than be created and copied
     *     into place incrementally as the ES5 algorithm specifies it.  This
     *     requires moving portions of the Str call in 8a into this algorithm
     *     (and in JO as well).
     */

    /* Steps 1-2, 11. */
    CycleDetector detect(scx, obj);
    if (!detect.foundCycle(cx))
        return false;

    if (!scx->sb.append('['))
        return false;

    /* Step 6. */
    uint32_t length;
    if (!GetLengthProperty(cx, obj, &length))
        return false;

    /* Steps 7-10. */
    if (length != 0) {
        /* Steps 4, 10b(i). */
        if (!WriteIndent(scx, scx->depth))
            return false;

        /* Steps 7-10. */
        RootedValue outputValue(cx);
        for (uint32_t i = 0; i < length; i++) {
            if (!CheckForInterrupt(cx))
                return false;

            /*
             * Steps 8a-8c.  Again note how the call to the spec's Str method
             * is broken up into getting the property, running it past toJSON
             * and the replacer and maybe unboxing, and interpreting some
             * values as |null| in separate steps.
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
                RootedNativeObject nativeObj(cx, &obj->as<NativeObject>());
                if (i <= JSID_INT_MAX) {
                    MOZ_ASSERT(nativeObj->containsDenseElement(i) != nativeObj->isIndexed(),
                               "the array must either be small enough to remain "
                               "fully dense (and otherwise un-indexed), *or* "
                               "all its initially-dense elements were sparsified "
                               "and the object is indexed");
                } else {
                    MOZ_ASSERT(nativeObj->isIndexed());
                }
            }
#endif
            if (!GetElement(cx, obj, i, &outputValue))
                return false;
            if (!PreprocessValue(cx, obj, i, &outputValue, scx))
                return false;
            if (IsFilteredValue(outputValue)) {
                if (!scx->sb.append("null"))
                    return false;
            } else {
                if (!Str(cx, outputValue, scx))
                    return false;
            }

            /* Steps 3, 4, 10b(i). */
            if (i < length - 1) {
                if (!scx->sb.append(','))
                    return false;
                if (!WriteIndent(scx, scx->depth))
                    return false;
            }
        }

        /* Step 10(b)(iii). */
        if (!WriteIndent(scx, scx->depth - 1))
            return false;
    }

    return scx->sb.append(']');
}

static bool
Str(JSContext* cx, const Value& v, StringifyContext* scx)
{
    /* Step 11 must be handled by the caller. */
    MOZ_ASSERT(!IsFilteredValue(v));

    if (!CheckRecursionLimit(cx))
        return false;

    /*
     * This method implements the Str algorithm in ES5 15.12.3, but:
     *
     *   * We move property retrieval (step 1) into callers to stream the
     *     stringification process and avoid constantly copying strings.
     *   * We move the preprocessing in steps 2-4 into a helper function to
     *     allow both JO and JA to use this method.  While JA could use it
     *     without this move, JO must omit any |undefined|-valued property per
     *     so it can't stream out a value using the Str method exactly as
     *     defined by ES5.
     *   * We move step 11 into callers, again to ease streaming.
     */

    /* Step 8. */
    if (v.isString())
        return Quote(cx, scx->sb, v.toString());

    /* Step 5. */
    if (v.isNull())
        return scx->sb.append("null");

    /* Steps 6-7. */
    if (v.isBoolean())
        return v.toBoolean() ? scx->sb.append("true") : scx->sb.append("false");

    /* Step 9. */
    if (v.isNumber()) {
        if (v.isDouble()) {
            if (!IsFinite(v.toDouble())) {
                MOZ_ASSERT(!scx->maybeSafely,
                           "input JS::ToJSONMaybeSafely must not include "
                           "reachable non-finite numbers");
                return scx->sb.append("null");
            }
        }

        return NumberValueToStringBuffer(cx, v, scx->sb);
    }

    /* Step 10. */
    MOZ_ASSERT(v.isObject());
    RootedObject obj(cx, &v.toObject());

    MOZ_ASSERT(!scx->maybeSafely || obj->is<PlainObject>() || obj->is<ArrayObject>(),
               "input to JS::ToJSONMaybeSafely must not include reachable "
               "objects that are neither arrays nor plain objects");

    scx->depth++;
    auto dec = mozilla::MakeScopeExit([&] { scx->depth--; });

    bool isArray;
    if (!IsArray(cx, obj, &isArray))
        return false;

    return isArray ? JA(cx, obj, scx) : JO(cx, obj, scx);
}

/* ES6 24.3.2. */
bool
js::Stringify(JSContext* cx, MutableHandleValue vp, JSObject* replacer_, const Value& space_,
              StringBuffer& sb, StringifyBehavior stringifyBehavior)
{
    RootedObject replacer(cx, replacer_);
    RootedValue space(cx, space_);

    MOZ_ASSERT_IF(stringifyBehavior == StringifyBehavior::RestrictedSafe, space.isNull());
    MOZ_ASSERT_IF(stringifyBehavior == StringifyBehavior::RestrictedSafe, vp.isObject());
    /**
     * This uses MOZ_ASSERT, since it's actually asserting something jsapi
     * consumers could get wrong, so needs a better error message.
     */
    MOZ_ASSERT(stringifyBehavior == StringifyBehavior::Normal ||
               vp.toObject().is<PlainObject>() || vp.toObject().is<ArrayObject>(),
               "input to JS::ToJSONMaybeSafely must be a plain object or array");

    /* Step 4. */
    AutoIdVector propertyList(cx);
    if (replacer) {
        bool isArray;
        if (replacer->isCallable()) {
            /* Step 4a(i): use replacer to transform values.  */
        } else if (!IsArray(cx, replacer, &isArray)) {
            return false;
        } else if (isArray) {
            /* Step 4b(iii). */

            /* Step 4b(iii)(2-3). */
            uint32_t len;
            if (!GetLengthProperty(cx, replacer, &len))
                return false;

            // Cap the initial size to a moderately small value.  This avoids
            // ridiculous over-allocation if an array with bogusly-huge length
            // is passed in.  If we end up having to add elements past this
            // size, the set will naturally resize to accommodate them.
            const uint32_t MaxInitialSize = 32;
            Rooted<GCHashSet<jsid>> idSet(cx, GCHashSet<jsid>(cx));
            if (!idSet.init(Min(len, MaxInitialSize)))
                return false;

            /* Step 4b(iii)(4). */
            uint32_t k = 0;

            /* Step 4b(iii)(5). */
            RootedValue item(cx);
            for (; k < len; k++) {
                if (!CheckForInterrupt(cx))
                    return false;

                /* Step 4b(iii)(5)(a-b). */
                if (!GetElement(cx, replacer, k, &item))
                    return false;

                RootedId id(cx);

                /* Step 4b(iii)(5)(c-f). */
                if (item.isNumber()) {
                    /* Step 4b(iii)(5)(e). */
                    int32_t n;
                    if (ValueFitsInInt32(item, &n) && INT_FITS_IN_JSID(n)) {
                        id = INT_TO_JSID(n);
                    } else {
                        if (!ValueToId<CanGC>(cx, item, &id))
                            return false;
                    }
                } else {
                    bool shouldAdd = item.isString();
                    if (!shouldAdd) {
                        ESClass cls;
                        if (!GetClassOfValue(cx, item, &cls))
                            return false;

                        shouldAdd = cls == ESClass::String || cls == ESClass::Number;
                    }

                    if (shouldAdd) {
                        /* Step 4b(iii)(5)(f). */
                        if (!ValueToId<CanGC>(cx, item, &id))
                            return false;
                    } else {
                        /* Step 4b(iii)(5)(g). */
                        continue;
                    }
                }

                /* Step 4b(iii)(5)(g). */
                auto p = idSet.lookupForAdd(id);
                if (!p) {
                    /* Step 4b(iii)(5)(g)(i). */
                    if (!idSet.add(p, id) || !propertyList.append(id))
                        return false;
                }
            }
        } else {
            replacer = nullptr;
        }
    }

    /* Step 5. */
    if (space.isObject()) {
        RootedObject spaceObj(cx, &space.toObject());

        ESClass cls;
        if (!GetBuiltinClass(cx, spaceObj, &cls))
            return false;

        if (cls == ESClass::Number) {
            double d;
            if (!ToNumber(cx, space, &d))
                return false;
            space = NumberValue(d);
        } else if (cls == ESClass::String) {
            JSString* str = ToStringSlow<CanGC>(cx, space);
            if (!str)
                return false;
            space = StringValue(str);
        }
    }

    StringBuffer gap(cx);

    if (space.isNumber()) {
        /* Step 6. */
        double d;
        MOZ_ALWAYS_TRUE(ToInteger(cx, space, &d));
        d = Min(10.0, d);
        if (d >= 1 && !gap.appendN(' ', uint32_t(d)))
            return false;
    } else if (space.isString()) {
        /* Step 7. */
        JSLinearString* str = space.toString()->ensureLinear(cx);
        if (!str)
            return false;
        size_t len = Min(size_t(10), str->length());
        if (!gap.appendSubstring(str, 0, len))
            return false;
    } else {
        /* Step 8. */
        MOZ_ASSERT(gap.empty());
    }

    RootedPlainObject wrapper(cx);
    RootedId emptyId(cx, NameToId(cx->names().empty));
    if (replacer && replacer->isCallable()) {
        // We can skip creating the initial wrapper object if no replacer
        // function is present.

        /* Step 9. */
        wrapper = NewBuiltinClassInstance<PlainObject>(cx);
        if (!wrapper)
            return false;

        /* Steps 10-11. */
        if (!NativeDefineDataProperty(cx, wrapper, emptyId, vp, JSPROP_ENUMERATE))
            return false;
    }

    /* Step 12. */
    StringifyContext scx(cx, sb, gap, replacer, propertyList,
                         stringifyBehavior == StringifyBehavior::RestrictedSafe);
    if (!PreprocessValue(cx, wrapper, HandleId(emptyId), vp, &scx))
        return false;
    if (IsFilteredValue(vp))
        return true;

    return Str(cx, vp, &scx);
}

/* ES5 15.12.2 Walk. */
static bool
Walk(JSContext* cx, HandleObject holder, HandleId name, HandleValue reviver, MutableHandleValue vp)
{
    if (!CheckRecursionLimit(cx))
        return false;

    /* Step 1. */
    RootedValue val(cx);
    if (!GetProperty(cx, holder, holder, name, &val))
        return false;

    /* Step 2. */
    if (val.isObject()) {
        RootedObject obj(cx, &val.toObject());

        bool isArray;
        if (!IsArray(cx, obj, &isArray))
            return false;

        if (isArray) {
            /* Step 2a(ii). */
            uint32_t length;
            if (!GetLengthProperty(cx, obj, &length))
                return false;

            /* Step 2a(i), 2a(iii-iv). */
            RootedId id(cx);
            RootedValue newElement(cx);
            for (uint32_t i = 0; i < length; i++) {
                if (!CheckForInterrupt(cx))
                    return false;

                if (!IndexToId(cx, i, &id))
                    return false;

                /* Step 2a(iii)(1). */
                if (!Walk(cx, obj, id, reviver, &newElement))
                    return false;

                ObjectOpResult ignored;
                if (newElement.isUndefined()) {
                    /* Step 2a(iii)(2). The spec deliberately ignores strict failure. */
                    if (!DeleteProperty(cx, obj, id, ignored))
                        return false;
                } else {
                    /* Step 2a(iii)(3). The spec deliberately ignores strict failure. */
                    Rooted<PropertyDescriptor> desc(cx);
                    desc.setDataDescriptor(newElement, JSPROP_ENUMERATE);
                    if (!DefineProperty(cx, obj, id, desc, ignored))
                        return false;
                }
            }
        } else {
            /* Step 2b(i). */
            AutoIdVector keys(cx);
            if (!GetPropertyKeys(cx, obj, JSITER_OWNONLY, &keys))
                return false;

            /* Step 2b(ii). */
            RootedId id(cx);
            RootedValue newElement(cx);
            for (size_t i = 0, len = keys.length(); i < len; i++) {
                if (!CheckForInterrupt(cx))
                    return false;

                /* Step 2b(ii)(1). */
                id = keys[i];
                if (!Walk(cx, obj, id, reviver, &newElement))
                    return false;

                ObjectOpResult ignored;
                if (newElement.isUndefined()) {
                    /* Step 2b(ii)(2). The spec deliberately ignores strict failure. */
                    if (!DeleteProperty(cx, obj, id, ignored))
                        return false;
                } else {
                    /* Step 2b(ii)(3). The spec deliberately ignores strict failure. */
                    Rooted<PropertyDescriptor> desc(cx);
                    desc.setDataDescriptor(newElement, JSPROP_ENUMERATE);
                    if (!DefineProperty(cx, obj, id, desc, ignored))
                        return false;
                }
            }
        }
    }

    /* Step 3. */
    RootedString key(cx, IdToString(cx, name));
    if (!key)
        return false;

    RootedValue keyVal(cx, StringValue(key));
    return js::Call(cx, reviver, holder, keyVal, val, vp);
}

static bool
Revive(JSContext* cx, HandleValue reviver, MutableHandleValue vp)
{
    RootedPlainObject obj(cx, NewBuiltinClassInstance<PlainObject>(cx));
    if (!obj)
        return false;

    if (!DefineDataProperty(cx, obj, cx->names().empty, vp))
        return false;

    Rooted<jsid> id(cx, NameToId(cx->names().empty));
    return Walk(cx, obj, id, reviver, vp);
}

template <typename CharT>
bool
js::ParseJSONWithReviver(JSContext* cx, const mozilla::Range<const CharT> chars, HandleValue reviver,
                         MutableHandleValue vp)
{
    /* 15.12.2 steps 2-3. */
    Rooted<JSONParser<CharT>> parser(cx, JSONParser<CharT>(cx, chars));
    if (!parser.parse(vp))
        return false;

    /* 15.12.2 steps 4-5. */
    if (IsCallable(reviver))
        return Revive(cx, reviver, vp);
    return true;
}

template bool
js::ParseJSONWithReviver(JSContext* cx, const mozilla::Range<const Latin1Char> chars,
                         HandleValue reviver, MutableHandleValue vp);

template bool
js::ParseJSONWithReviver(JSContext* cx, const mozilla::Range<const char16_t> chars, HandleValue reviver,
                         MutableHandleValue vp);

static bool
json_toSource(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    args.rval().setString(cx->names().JSON);
    return true;
}

/* ES5 15.12.2. */
static bool
json_parse(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    /* Step 1. */
    JSString* str = (args.length() >= 1)
                    ? ToString<CanGC>(cx, args[0])
                    : cx->names().undefined;
    if (!str)
        return false;

    JSLinearString* linear = str->ensureLinear(cx);
    if (!linear)
        return false;

    AutoStableStringChars linearChars(cx);
    if (!linearChars.init(cx, linear))
        return false;

    HandleValue reviver = args.get(1);

    /* Steps 2-5. */
    return linearChars.isLatin1()
           ? ParseJSONWithReviver(cx, linearChars.latin1Range(), reviver, args.rval())
           : ParseJSONWithReviver(cx, linearChars.twoByteRange(), reviver, args.rval());
}

/* ES6 24.3.2. */
bool
json_stringify(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    RootedObject replacer(cx, args.get(1).isObject() ? &args[1].toObject() : nullptr);
    RootedValue value(cx, args.get(0));
    RootedValue space(cx, args.get(2));

    StringBuffer sb(cx);
    if (!Stringify(cx, &value, replacer, space, sb, StringifyBehavior::Normal))
        return false;

    // XXX This can never happen to nsJSON.cpp, but the JSON object
    // needs to support returning undefined. So this is a little awkward
    // for the API, because we want to support streaming writers.
    if (!sb.empty()) {
        JSString* str = sb.finishString();
        if (!str)
            return false;
        args.rval().setString(str);
    } else {
        args.rval().setUndefined();
    }

    return true;
}

static const JSFunctionSpec json_static_methods[] = {
    JS_FN(js_toSource_str,  json_toSource,      0, 0),
    JS_FN("parse",          json_parse,         2, 0),
    JS_FN("stringify",      json_stringify,     3, 0),
    JS_FS_END
};

JSObject*
js::InitJSONClass(JSContext* cx, HandleObject obj)
{
    Handle<GlobalObject*> global = obj.as<GlobalObject>();

    RootedObject proto(cx, GlobalObject::getOrCreateObjectPrototype(cx, global));
    if (!proto)
        return nullptr;
    RootedObject JSON(cx, NewObjectWithGivenProto(cx, &JSONClass, proto, SingletonObject));
    if (!JSON)
        return nullptr;

    if (!JS_DefineProperty(cx, global, js_JSON_str, JSON, JSPROP_RESOLVING))
        return nullptr;

    if (!JS_DefineFunctions(cx, JSON, json_static_methods))
        return nullptr;

    if (!DefineToStringTag(cx, JSON, cx->names().JSON))
        return nullptr;

    global->setConstructor(JSProto_JSON, ObjectValue(*JSON));

    return JSON;
}

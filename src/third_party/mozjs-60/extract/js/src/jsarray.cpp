/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jsarrayinlines.h"

#include "mozilla/ArrayUtils.h"
#include "mozilla/CheckedInt.h"
#include "mozilla/DebugOnly.h"
#include "mozilla/MathAlgorithms.h"

#include <algorithm>

#include "jsapi.h"
#include "jsfriendapi.h"
#include "jsnum.h"
#include "jstypes.h"
#include "jsutil.h"

#include "ds/Sort.h"
#include "gc/Heap.h"
#include "jit/InlinableNatives.h"
#include "js/Class.h"
#include "js/Conversions.h"
#include "util/StringBuffer.h"
#include "util/Text.h"
#include "vm/ArgumentsObject.h"
#include "vm/Interpreter.h"
#include "vm/Iteration.h"
#include "vm/JSAtom.h"
#include "vm/JSContext.h"
#include "vm/JSFunction.h"
#include "vm/JSObject.h"
#include "vm/SelfHosting.h"
#include "vm/Shape.h"
#include "vm/TypedArrayObject.h"
#include "vm/WrapperObject.h"

#include "vm/ArgumentsObject-inl.h"
#include "vm/ArrayObject-inl.h"
#include "vm/Caches-inl.h"
#include "vm/GeckoProfiler-inl.h"
#include "vm/Interpreter-inl.h"
#include "vm/JSAtom-inl.h"
#include "vm/NativeObject-inl.h"
#include "vm/UnboxedObject-inl.h"

using namespace js;
using namespace js::gc;

using mozilla::Abs;
using mozilla::ArrayLength;
using mozilla::CeilingLog2;
using mozilla::CheckedInt;
using mozilla::DebugOnly;

using JS::AutoCheckCannotGC;
using JS::IsArrayAnswer;
using JS::ToUint32;

bool
JS::IsArray(JSContext* cx, HandleObject obj, IsArrayAnswer* answer)
{
    if (obj->is<ArrayObject>()) {
        *answer = IsArrayAnswer::Array;
        return true;
    }

    if (obj->is<ProxyObject>())
        return Proxy::isArray(cx, obj, answer);

    *answer = IsArrayAnswer::NotArray;
    return true;
}

bool
JS::IsArray(JSContext* cx, HandleObject obj, bool* isArray)
{
    IsArrayAnswer answer;
    if (!IsArray(cx, obj, &answer))
        return false;

    if (answer == IsArrayAnswer::RevokedProxy) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_PROXY_REVOKED);
        return false;
    }

    *isArray = answer == IsArrayAnswer::Array;
    return true;
}

// ES2017 7.1.15 ToLength, but clamped to the [0,2^32-2] range.
static bool
ToLengthClamped(JSContext* cx, HandleValue v, uint32_t* out)
{
    if (v.isInt32()) {
        int32_t i = v.toInt32();
        *out = i < 0 ? 0 : i;
        return true;
    }
    double d;
    if (v.isDouble()) {
        d = v.toDouble();
    } else {
        if (!ToNumber(cx, v, &d))
            return false;
    }
    d = JS::ToInteger(d);
    if (d <= 0.0)
        *out = 0;
    else if (d < double(UINT32_MAX - 1))
        *out = uint32_t(d);
    else
        *out = UINT32_MAX;
    return true;
}

bool
js::GetLengthProperty(JSContext* cx, HandleObject obj, uint32_t* lengthp)
{
    if (obj->is<ArrayObject>()) {
        *lengthp = obj->as<ArrayObject>().length();
        return true;
    }

    if (obj->is<ArgumentsObject>()) {
        ArgumentsObject& argsobj = obj->as<ArgumentsObject>();
        if (!argsobj.hasOverriddenLength()) {
            *lengthp = argsobj.initialLength();
            return true;
        }
    }

    RootedValue value(cx);
    if (!GetProperty(cx, obj, obj, cx->names().length, &value))
        return false;

    if (!ToLengthClamped(cx, value, lengthp))
        return false;

    return true;
}

// ES2017 7.1.15 ToLength.
static bool
ToLength(JSContext* cx, HandleValue v, uint64_t* out)
{
    if (v.isInt32()) {
        int32_t i = v.toInt32();
        *out = i < 0 ? 0 : i;
        return true;
    }

    double d;
    if (v.isDouble()) {
        d = v.toDouble();
    } else {
        if (!ToNumber(cx, v, &d))
            return false;
    }

    d = JS::ToInteger(d);
    if (d <= 0.0)
        *out = 0;
    else
        *out = uint64_t(Min(d, DOUBLE_INTEGRAL_PRECISION_LIMIT - 1));
    return true;
}

static MOZ_ALWAYS_INLINE bool
GetLengthProperty(JSContext* cx, HandleObject obj, uint64_t* lengthp)
{
    if (obj->is<ArrayObject>()) {
        *lengthp = obj->as<ArrayObject>().length();
        return true;
    }

    if (obj->is<ArgumentsObject>()) {
        ArgumentsObject& argsobj = obj->as<ArgumentsObject>();
        if (!argsobj.hasOverriddenLength()) {
            *lengthp = argsobj.initialLength();
            return true;
        }
    }

    RootedValue value(cx);
    if (!GetProperty(cx, obj, obj, cx->names().length, &value))
        return false;

    return ToLength(cx, value, lengthp);
}

/*
 * Determine if the id represents an array index.
 *
 * An id is an array index according to ECMA by (15.4):
 *
 * "Array objects give special treatment to a certain class of property names.
 * A property name P (in the form of a string value) is an array index if and
 * only if ToString(ToUint32(P)) is equal to P and ToUint32(P) is not equal
 * to 2^32-1."
 *
 * This means the largest allowed index is actually 2^32-2 (4294967294).
 *
 * In our implementation, it would be sufficient to check for id.isInt32()
 * except that by using signed 31-bit integers we miss the top half of the
 * valid range. This function checks the string representation itself; note
 * that calling a standard conversion routine might allow strings such as
 * "08" or "4.0" as array indices, which they are not.
 *
 */
template <typename CharT>
static bool
StringIsArrayIndex(const CharT* s, uint32_t length, uint32_t* indexp)
{
    const CharT* end = s + length;

    if (length == 0 || length > (sizeof("4294967294") - 1) || !JS7_ISDEC(*s))
        return false;

    uint32_t c = 0, previous = 0;
    uint32_t index = JS7_UNDEC(*s++);

    /* Don't allow leading zeros. */
    if (index == 0 && s != end)
        return false;

    for (; s < end; s++) {
        if (!JS7_ISDEC(*s))
            return false;

        previous = index;
        c = JS7_UNDEC(*s);
        index = 10 * index + c;
    }

    /* Make sure we didn't overflow. */
    if (previous < (MAX_ARRAY_INDEX / 10) || (previous == (MAX_ARRAY_INDEX / 10) &&
        c <= (MAX_ARRAY_INDEX % 10))) {
        MOZ_ASSERT(index <= MAX_ARRAY_INDEX);
        *indexp = index;
        return true;
    }

    return false;
}

JS_FRIEND_API(bool)
js::StringIsArrayIndex(JSLinearString* str, uint32_t* indexp)
{
    AutoCheckCannotGC nogc;
    return str->hasLatin1Chars()
           ? ::StringIsArrayIndex(str->latin1Chars(nogc), str->length(), indexp)
           : ::StringIsArrayIndex(str->twoByteChars(nogc), str->length(), indexp);
}

template <typename T>
static bool
ToId(JSContext* cx, T index, MutableHandleId id);

template <>
bool
ToId(JSContext* cx, uint32_t index, MutableHandleId id)
{
    return IndexToId(cx, index, id);
}

template <>
bool
ToId(JSContext* cx, uint64_t index, MutableHandleId id)
{
    MOZ_ASSERT(index < uint64_t(DOUBLE_INTEGRAL_PRECISION_LIMIT));

    if (index == uint32_t(index))
        return IndexToId(cx, uint32_t(index), id);

    Value tmp = DoubleValue(index);
    return ValueToId<CanGC>(cx, HandleValue::fromMarkedLocation(&tmp), id);
}

/*
 * If the property at the given index exists, get its value into |vp| and set
 * |*hole| to false. Otherwise set |*hole| to true and |vp| to Undefined.
 */
template <typename T>
static bool
HasAndGetElement(JSContext* cx, HandleObject obj, HandleObject receiver, T index, bool* hole,
                 MutableHandleValue vp)
{
    if (obj->isNative()) {
        NativeObject* nobj = &obj->as<NativeObject>();
        if (index < nobj->getDenseInitializedLength()) {
            vp.set(nobj->getDenseElement(size_t(index)));
            if (!vp.isMagic(JS_ELEMENTS_HOLE)) {
                *hole = false;
                return true;
            }
        }
        if (nobj->is<ArgumentsObject>() && index <= UINT32_MAX) {
            if (nobj->as<ArgumentsObject>().maybeGetElement(uint32_t(index), vp)) {
                *hole = false;
                return true;
            }
        }
    }

    RootedId id(cx);
    if (!ToId(cx, index, &id))
        return false;

    bool found;
    if (!HasProperty(cx, obj, id, &found))
        return false;

    if (found) {
        if (!GetProperty(cx, obj, receiver, id, vp))
            return false;
    } else {
        vp.setUndefined();
    }
    *hole = !found;
    return true;
}

template <typename T>
static inline bool
HasAndGetElement(JSContext* cx, HandleObject obj, T index, bool* hole, MutableHandleValue vp)
{
    return HasAndGetElement(cx, obj, obj, index, hole, vp);
}

bool
ElementAdder::append(JSContext* cx, HandleValue v)
{
    MOZ_ASSERT(index_ < length_);
    if (resObj_) {
        NativeObject* resObj = &resObj_->as<NativeObject>();
        DenseElementResult result = resObj->setOrExtendDenseElements(cx, index_, v.address(), 1);
        if (result == DenseElementResult::Failure)
            return false;
        if (result == DenseElementResult::Incomplete) {
            if (!DefineDataElement(cx, resObj_, index_, v))
                return false;
        }
    } else {
        vp_[index_] = v;
    }
    index_++;
    return true;
}

void
ElementAdder::appendHole()
{
    MOZ_ASSERT(getBehavior_ == ElementAdder::CheckHasElemPreserveHoles);
    MOZ_ASSERT(index_ < length_);
    if (!resObj_)
        vp_[index_].setMagic(JS_ELEMENTS_HOLE);
    index_++;
}

bool
js::GetElementsWithAdder(JSContext* cx, HandleObject obj, HandleObject receiver,
                         uint32_t begin, uint32_t end, ElementAdder* adder)
{
    MOZ_ASSERT(begin <= end);

    RootedValue val(cx);
    for (uint32_t i = begin; i < end; i++) {
        if (adder->getBehavior() == ElementAdder::CheckHasElemPreserveHoles) {
            bool hole;
            if (!HasAndGetElement(cx, obj, receiver, i, &hole, &val))
                return false;
            if (hole) {
                adder->appendHole();
                continue;
            }
        } else {
            MOZ_ASSERT(adder->getBehavior() == ElementAdder::GetElement);
            if (!GetElement(cx, obj, receiver, i, &val))
                return false;
        }
        if (!adder->append(cx, val))
            return false;
    }

    return true;
}

static bool
ObjectMayHaveExtraIndexedProperties(JSObject* obj);

static inline bool
IsPackedArrayOrNoExtraIndexedProperties(JSObject* obj, uint64_t length)
{
    return (IsPackedArray(obj) && obj->as<ArrayObject>().length() == length) ||
           !ObjectMayHaveExtraIndexedProperties(obj);
}

static bool
GetDenseElements(NativeObject* aobj, uint32_t length, Value* vp)
{
    MOZ_ASSERT(IsPackedArrayOrNoExtraIndexedProperties(aobj, length));

    if (length > aobj->getDenseInitializedLength())
        return false;

    for (size_t i = 0; i < length; i++) {
        vp[i] = aobj->getDenseElement(i);

        // No other indexed properties so hole => undefined.
        if (vp[i].isMagic(JS_ELEMENTS_HOLE))
            vp[i] = UndefinedValue();
    }

    return true;
}

bool
js::GetElements(JSContext* cx, HandleObject aobj, uint32_t length, Value* vp)
{
    if (IsPackedArrayOrNoExtraIndexedProperties(aobj, length)) {
        if (GetDenseElements(&aobj->as<NativeObject>(), length, vp))
            return true;
    }

    if (aobj->is<ArgumentsObject>()) {
        ArgumentsObject& argsobj = aobj->as<ArgumentsObject>();
        if (!argsobj.hasOverriddenLength()) {
            if (argsobj.maybeGetElements(0, length, vp))
                return true;
        }
    }

    if (aobj->is<TypedArrayObject>()) {
        TypedArrayObject* typedArray = &aobj->as<TypedArrayObject>();
        if (typedArray->length() == length) {
            typedArray->getElements(vp);
            return true;
        }
    }

    if (js::GetElementsOp op = aobj->getOpsGetElements()) {
        ElementAdder adder(cx, vp, length, ElementAdder::GetElement);
        return op(cx, aobj, 0, length, &adder);
    }

    for (uint32_t i = 0; i < length; i++) {
        if (!GetElement(cx, aobj, aobj, i, MutableHandleValue::fromMarkedLocation(&vp[i])))
            return false;
    }

    return true;
}

static inline bool
GetArrayElement(JSContext* cx, HandleObject obj, uint64_t index, MutableHandleValue vp)
{
    if (obj->isNative()) {
        NativeObject* nobj = &obj->as<NativeObject>();
        if (index < nobj->getDenseInitializedLength()) {
            vp.set(nobj->getDenseElement(size_t(index)));
            if (!vp.isMagic(JS_ELEMENTS_HOLE))
                return true;
        }

        if (nobj->is<ArgumentsObject>() && index <= UINT32_MAX) {
            if (nobj->as<ArgumentsObject>().maybeGetElement(uint32_t(index), vp))
                return true;
        }
    }

    RootedId id(cx);
    if (!ToId(cx, index, &id))
        return false;
    return GetProperty(cx, obj, obj, id, vp);
}

static inline bool
DefineArrayElement(JSContext* cx, HandleObject obj, uint64_t index, HandleValue value)
{
    RootedId id(cx);
    if (!ToId(cx, index, &id))
        return false;
    return DefineDataProperty(cx, obj, id, value);
}

// Set the value of the property at the given index to v.
static inline bool
SetArrayElement(JSContext* cx, HandleObject obj, uint64_t index, HandleValue v)
{
    RootedId id(cx);
    if (!ToId(cx, index, &id))
        return false;

    return SetProperty(cx, obj, id, v);
}

/*
 * Attempt to delete the element |index| from |obj| as if by
 * |obj.[[Delete]](index)|.
 *
 * If an error occurs while attempting to delete the element (that is, the call
 * to [[Delete]] threw), return false.
 *
 * Otherwise call result.succeed() or result.fail() to indicate whether the
 * deletion attempt succeeded (that is, whether the call to [[Delete]] returned
 * true or false).  (Deletes generally fail only when the property is
 * non-configurable, but proxies may implement different semantics.)
 */
static bool
DeleteArrayElement(JSContext* cx, HandleObject obj, uint64_t index, ObjectOpResult& result)
{
    if (obj->is<ArrayObject>() &&
        !obj->as<NativeObject>().isIndexed() &&
        !obj->as<NativeObject>().denseElementsAreFrozen())
    {
        ArrayObject* aobj = &obj->as<ArrayObject>();
        if (index <= UINT32_MAX) {
            uint32_t idx = uint32_t(index);
            if (idx < aobj->getDenseInitializedLength()) {
                if (!aobj->maybeCopyElementsForWrite(cx))
                    return false;
                if (idx+1 == aobj->getDenseInitializedLength()) {
                    aobj->setDenseInitializedLength(idx);
                } else {
                    aobj->markDenseElementsNotPacked(cx);
                    aobj->setDenseElement(idx, MagicValue(JS_ELEMENTS_HOLE));
                }
                if (!SuppressDeletedElement(cx, obj, idx))
                    return false;
            }
        }

        return result.succeed();
    }

    RootedId id(cx);
    if (!ToId(cx, index, &id))
        return false;
    return DeleteProperty(cx, obj, id, result);
}

/* ES6 draft rev 32 (2 Febr 2015) 7.3.7 */
static bool
DeletePropertyOrThrow(JSContext* cx, HandleObject obj, uint64_t index)
{
    ObjectOpResult success;
    if (!DeleteArrayElement(cx, obj, index, success))
        return false;
    if (!success) {
        RootedId id(cx);
        if (!ToId(cx, index, &id))
            return false;
        return success.reportError(cx, obj, id);
    }
    return true;
}

static bool
DeletePropertiesOrThrow(JSContext* cx, HandleObject obj, uint64_t len, uint64_t finalLength)
{
    if (obj->is<ArrayObject>() &&
        !obj->as<NativeObject>().isIndexed() &&
        !obj->as<NativeObject>().denseElementsAreFrozen())
    {
        if (len <= UINT32_MAX) {
            // Skip forward to the initialized elements of this array.
            len = Min(uint32_t(len), obj->as<ArrayObject>().getDenseInitializedLength());
        }
    }

    for (uint64_t k = len; k > finalLength; k--) {
        if (!CheckForInterrupt(cx))
            return false;

        if (!DeletePropertyOrThrow(cx, obj, k - 1))
            return false;
    }
    return true;
}

static bool
SetArrayLengthProperty(JSContext* cx, HandleArrayObject obj, HandleValue value)
{
    RootedId id(cx, NameToId(cx->names().length));
    ObjectOpResult result;
    if (obj->lengthIsWritable()) {
        if (!ArraySetLength(cx, obj, id, JSPROP_PERMANENT, value, result))
            return false;
    } else {
        MOZ_ALWAYS_TRUE(result.fail(JSMSG_READ_ONLY));
    }
    return result.checkStrict(cx, obj, id);
}

static bool
SetLengthProperty(JSContext* cx, HandleObject obj, uint64_t length)
{
    MOZ_ASSERT(length < uint64_t(DOUBLE_INTEGRAL_PRECISION_LIMIT));

    RootedValue v(cx, NumberValue(length));
    if (obj->is<ArrayObject>())
        return SetArrayLengthProperty(cx, obj.as<ArrayObject>(), v);
    return SetProperty(cx, obj, cx->names().length, v);
}

bool
js::SetLengthProperty(JSContext* cx, HandleObject obj, uint32_t length)
{
    RootedValue v(cx, NumberValue(length));
    if (obj->is<ArrayObject>())
        return SetArrayLengthProperty(cx, obj.as<ArrayObject>(), v);
    return SetProperty(cx, obj, cx->names().length, v);
}

static bool
array_length_getter(JSContext* cx, HandleObject obj, HandleId id, MutableHandleValue vp)
{
    vp.setNumber(obj->as<ArrayObject>().length());
    return true;
}

static bool
array_length_setter(JSContext* cx, HandleObject obj, HandleId id, HandleValue v,
                    ObjectOpResult& result)
{
    MOZ_ASSERT(id == NameToId(cx->names().length));

    if (!obj->is<ArrayObject>()) {
        // This array .length property was found on the prototype
        // chain. Ideally the setter should not have been called, but since
        // we're here, do an impression of SetPropertyByDefining.
        return DefineDataProperty(cx, obj, id, v, JSPROP_ENUMERATE, result);
    }

    HandleArrayObject arr = obj.as<ArrayObject>();
    MOZ_ASSERT(arr->lengthIsWritable(),
               "setter shouldn't be called if property is non-writable");

    return ArraySetLength(cx, arr, id, JSPROP_PERMANENT, v, result);
}

struct ReverseIndexComparator
{
    bool operator()(const uint32_t& a, const uint32_t& b, bool* lessOrEqualp) {
        MOZ_ASSERT(a != b, "how'd we get duplicate indexes?");
        *lessOrEqualp = b <= a;
        return true;
    }
};

static bool
MaybeInIteration(HandleObject obj, JSContext* cx)
{
    /*
     * Don't optimize if the array might be in the midst of iteration.  We
     * rely on this to be able to safely move dense array elements around with
     * just a memmove (see NativeObject::moveDenseArrayElements), without worrying
     * about updating any in-progress enumerators for properties implicitly
     * deleted if a hole is moved from one location to another location not yet
     * visited.  See bug 690622.
     *
     * Note that it's fine to optimize if |obj| is on the prototype of another
     * object: SuppressDeletedProperty only suppresses properties deleted from
     * the iterated object itself.
     */

    if (MOZ_LIKELY(!cx->compartment()->objectMaybeInIteration(obj)))
        return false;

    ObjectGroup* group = JSObject::getGroup(cx, obj);
    if (MOZ_UNLIKELY(!group)) {
        cx->recoverFromOutOfMemory();
        return true;
    }

    if (MOZ_UNLIKELY(group->hasAllFlags(OBJECT_FLAG_ITERATED)))
        return true;

    return false;
}

/* ES6 draft rev 34 (2015 Feb 20) 9.4.2.4 ArraySetLength */
bool
js::ArraySetLength(JSContext* cx, Handle<ArrayObject*> arr, HandleId id,
                   unsigned attrs, HandleValue value, ObjectOpResult& result)
{
    MOZ_ASSERT(id == NameToId(cx->names().length));

    if (!arr->maybeCopyElementsForWrite(cx))
        return false;

    // Step 1.
    uint32_t newLen;
    if (attrs & JSPROP_IGNORE_VALUE) {
        MOZ_ASSERT(value.isUndefined());

        // The spec has us calling OrdinaryDefineOwnProperty if
        // Desc.[[Value]] is absent, but our implementation is so different that
        // this is impossible. Instead, set newLen to the current length and
        // proceed to step 9.
        newLen = arr->length();
    } else {
        // Step 2 is irrelevant in our implementation.

        // Step 3.
        if (!ToUint32(cx, value, &newLen))
            return false;

        // Step 4.
        double d;
        if (!ToNumber(cx, value, &d))
            return false;

        // Step 5.
        if (d != newLen) {
            JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_BAD_ARRAY_LENGTH);
            return false;
        }

        // Steps 6-8 are irrelevant in our implementation.
    }

    // Steps 9-11.
    bool lengthIsWritable = arr->lengthIsWritable();
#ifdef DEBUG
    {
        RootedShape lengthShape(cx, arr->lookupPure(id));
        MOZ_ASSERT(lengthShape);
        MOZ_ASSERT(lengthShape->writable() == lengthIsWritable);
    }
#endif
    uint32_t oldLen = arr->length();

    // Part of steps 1.a, 12.a, and 16: Fail if we're being asked to change
    // enumerability or configurability, or otherwise break the object
    // invariants. (ES6 checks these by calling OrdinaryDefineOwnProperty, but
    // in SM, the array length property is hardly ordinary.)
    if ((attrs & (JSPROP_PERMANENT | JSPROP_IGNORE_PERMANENT)) == 0 ||
        (attrs & (JSPROP_ENUMERATE | JSPROP_IGNORE_ENUMERATE)) == JSPROP_ENUMERATE ||
        (attrs & (JSPROP_GETTER | JSPROP_SETTER)) != 0 ||
        (!lengthIsWritable && (attrs & (JSPROP_READONLY | JSPROP_IGNORE_READONLY)) == 0))
    {
        return result.fail(JSMSG_CANT_REDEFINE_PROP);
    }

    // Steps 12-13 for arrays with non-writable length.
    if (!lengthIsWritable) {
        if (newLen == oldLen)
            return result.succeed();

        return result.fail(JSMSG_CANT_REDEFINE_ARRAY_LENGTH);
    }

    // Step 19.
    bool succeeded = true;
    do {
        // The initialized length and capacity of an array only need updating
        // when non-hole elements are added or removed, which doesn't happen
        // when array length stays the same or increases.
        if (newLen >= oldLen)
            break;

        // Attempt to propagate dense-element optimization tricks, if possible,
        // and avoid the generic (and accordingly slow) deletion code below.
        // We can only do this if there are only densely-indexed elements.
        // Once there's a sparse indexed element, there's no good way to know,
        // save by enumerating all the properties to find it.  But we *have* to
        // know in case that sparse indexed element is non-configurable, as
        // that element must prevent any deletions below it.  Bug 586842 should
        // fix this inefficiency by moving indexed storage to be entirely
        // separate from non-indexed storage.
        // A second reason for this optimization to be invalid is an active
        // for..in iteration over the array. Keys deleted before being reached
        // during the iteration must not be visited, and suppressing them here
        // would be too costly.
        if (!arr->isIndexed() && !MaybeInIteration(arr, cx)) {
            if (!arr->maybeCopyElementsForWrite(cx))
                return false;

            uint32_t oldCapacity = arr->getDenseCapacity();
            uint32_t oldInitializedLength = arr->getDenseInitializedLength();
            MOZ_ASSERT(oldCapacity >= oldInitializedLength);
            if (oldInitializedLength > newLen)
                arr->setDenseInitializedLength(newLen);
            if (oldCapacity > newLen)
                arr->shrinkElements(cx, newLen);

            // We've done the work of deleting any dense elements needing
            // deletion, and there are no sparse elements.  Thus we can skip
            // straight to defining the length.
            break;
        }

        // Step 15.
        //
        // Attempt to delete all elements above the new length, from greatest
        // to least.  If any of these deletions fails, we're supposed to define
        // the length to one greater than the index that couldn't be deleted,
        // *with the property attributes specified*.  This might convert the
        // length to be not the value specified, yet non-writable.  (You may be
        // forgiven for thinking these are interesting semantics.)  Example:
        //
        //   var arr =
        //     Object.defineProperty([0, 1, 2, 3], 1, { writable: false });
        //   Object.defineProperty(arr, "length",
        //                         { value: 0, writable: false });
        //
        // will convert |arr| to an array of non-writable length two, then
        // throw a TypeError.
        //
        // We implement this behavior, in the relevant lops below, by setting
        // |succeeded| to false.  Then we exit the loop, define the length
        // appropriately, and only then throw a TypeError, if necessary.
        uint32_t gap = oldLen - newLen;
        const uint32_t RemoveElementsFastLimit = 1 << 24;
        if (gap < RemoveElementsFastLimit) {
            // If we're removing a relatively small number of elements, just do
            // it exactly by the spec.
            while (newLen < oldLen) {
                // Step 15a.
                oldLen--;

                // Steps 15b-d.
                ObjectOpResult deleteSucceeded;
                if (!DeleteElement(cx, arr, oldLen, deleteSucceeded))
                    return false;
                if (!deleteSucceeded) {
                    newLen = oldLen + 1;
                    succeeded = false;
                    break;
                }
            }
        } else {
            // If we're removing a large number of elements from an array
            // that's probably sparse, try a different tack.  Get all the own
            // property names, sift out the indexes in the deletion range into
            // a vector, sort the vector greatest to least, then delete the
            // indexes greatest to least using that vector.  See bug 322135.
            //
            // This heuristic's kind of a huge guess -- "large number of
            // elements" and "probably sparse" are completely unprincipled
            // predictions.  In the long run, bug 586842 will support the right
            // fix: store sparse elements in a sorted data structure that
            // permits fast in-reverse-order traversal and concurrent removals.

            Vector<uint32_t> indexes(cx);
            {
                AutoIdVector props(cx);
                if (!GetPropertyKeys(cx, arr, JSITER_OWNONLY | JSITER_HIDDEN, &props))
                    return false;

                for (size_t i = 0; i < props.length(); i++) {
                    if (!CheckForInterrupt(cx))
                        return false;

                    uint32_t index;
                    if (!IdIsIndex(props[i], &index))
                        continue;

                    if (index >= newLen && index < oldLen) {
                        if (!indexes.append(index))
                            return false;
                    }
                }
            }

            uint32_t count = indexes.length();
            {
                // We should use radix sort to be O(n), but this is uncommon
                // enough that we'll punt til someone complains.
                Vector<uint32_t> scratch(cx);
                if (!scratch.resize(count))
                    return false;
                MOZ_ALWAYS_TRUE(MergeSort(indexes.begin(), count, scratch.begin(),
                                          ReverseIndexComparator()));
            }

            uint32_t index = UINT32_MAX;
            for (uint32_t i = 0; i < count; i++) {
                MOZ_ASSERT(indexes[i] < index, "indexes should never repeat");
                index = indexes[i];

                // Steps 15b-d.
                ObjectOpResult deleteSucceeded;
                if (!DeleteElement(cx, arr, index, deleteSucceeded))
                    return false;
                if (!deleteSucceeded) {
                    newLen = index + 1;
                    succeeded = false;
                    break;
                }
            }
        }
    } while (false);

    // Update array length. Technically we should have been doing this
    // throughout the loop, in step 19.d.iii.
    arr->setLength(cx, newLen);

    // Step 20.
    if (attrs & JSPROP_READONLY) {
        // Yes, we totally drop a non-stub getter/setter from a defineProperty
        // API call on the floor here.  Given that getter/setter will go away in
        // the long run, with accessors replacing them both internally and at the
        // API level, just run with this.
        RootedShape lengthShape(cx, arr->lookup(cx, id));
        if (!NativeObject::changeProperty(cx, arr, lengthShape,
                                          lengthShape->attributes() | JSPROP_READONLY,
                                          array_length_getter, array_length_setter))
        {
            return false;
        }
    }

    // All operations past here until the |!succeeded| code must be infallible,
    // so that all element fields remain properly synchronized.

    // Trim the initialized length, if needed, to preserve the <= length
    // invariant.  (Capacity was already reduced during element deletion, if
    // necessary.)
    ObjectElements* header = arr->getElementsHeader();
    header->initializedLength = Min(header->initializedLength, newLen);

    if (attrs & JSPROP_READONLY)
        arr->setNonWritableLength(cx);

    if (!succeeded)
        return result.fail(JSMSG_CANT_TRUNCATE_ARRAY);

    return result.succeed();
}

bool
js::WouldDefinePastNonwritableLength(HandleNativeObject obj, uint32_t index)
{
    if (!obj->is<ArrayObject>())
        return false;

    ArrayObject* arr = &obj->as<ArrayObject>();
    return !arr->lengthIsWritable() && index >= arr->length();
}

static bool
array_addProperty(JSContext* cx, HandleObject obj, HandleId id, HandleValue v)
{
    ArrayObject* arr = &obj->as<ArrayObject>();

    uint32_t index;
    if (!IdIsIndex(id, &index))
        return true;

    uint32_t length = arr->length();
    if (index >= length) {
        MOZ_ASSERT(arr->lengthIsWritable(),
                   "how'd this element get added if length is non-writable?");
        arr->setLength(cx, index + 1);
    }
    return true;
}

static inline bool
ObjectMayHaveExtraIndexedOwnProperties(JSObject* obj)
{
    if (!obj->isNative())
        return true;

    if (obj->as<NativeObject>().isIndexed())
        return true;

    if (obj->is<TypedArrayObject>())
        return true;

    return ClassMayResolveId(*obj->runtimeFromAnyThread()->commonNames,
                             obj->getClass(), INT_TO_JSID(0), obj);
}

/*
 * Whether obj may have indexed properties anywhere besides its dense
 * elements. This includes other indexed properties in its shape hierarchy, and
 * indexed properties or elements along its prototype chain.
 */
static bool
ObjectMayHaveExtraIndexedProperties(JSObject* obj)
{
    MOZ_ASSERT_IF(obj->hasDynamicPrototype(), !obj->isNative());

    if (ObjectMayHaveExtraIndexedOwnProperties(obj))
        return true;

    do {
        MOZ_ASSERT(obj->hasStaticPrototype(),
                   "dynamic-prototype objects must be non-native, ergo must "
                   "have failed ObjectMayHaveExtraIndexedOwnProperties");

        obj = obj->staticPrototype();
        if (!obj)
            return false; // no extra indexed properties found

        if (ObjectMayHaveExtraIndexedOwnProperties(obj))
            return true;
        if (obj->as<NativeObject>().getDenseInitializedLength() != 0)
            return true;
    } while (true);
}

static bool
AddLengthProperty(JSContext* cx, HandleArrayObject obj)
{
    /*
     * Add the 'length' property for a newly created array,
     * and update the elements to be an empty array owned by the object.
     * The shared emptyObjectElements singleton cannot be used for slow arrays,
     * as accesses to 'length' will use the elements header.
     */

    RootedId lengthId(cx, NameToId(cx->names().length));
    MOZ_ASSERT(!obj->lookup(cx, lengthId));

    return NativeObject::addAccessorProperty(cx, obj, lengthId,
                                             array_length_getter, array_length_setter,
                                             JSPROP_PERMANENT | JSPROP_SHADOWABLE);
}

static bool
IsArrayConstructor(const JSObject* obj)
{
    // This must only return true if v is *the* Array constructor for the
    // current compartment; we rely on the fact that any other Array
    // constructor would be represented as a wrapper.
    return obj->is<JSFunction>() &&
           obj->as<JSFunction>().isNative() &&
           obj->as<JSFunction>().native() == ArrayConstructor;
}

static bool
IsArrayConstructor(const Value& v)
{
    return v.isObject() && IsArrayConstructor(&v.toObject());
}

bool
js::IsWrappedArrayConstructor(JSContext* cx, const Value& v, bool* result)
{
    if (!v.isObject()) {
        *result = false;
        return true;
    }
    if (v.toObject().is<WrapperObject>()) {
        JSObject* obj = CheckedUnwrap(&v.toObject());
        if (!obj) {
            ReportAccessDenied(cx);
            return false;
        }

        *result = IsArrayConstructor(obj);
    } else {
        *result = false;
    }
    return true;
}

static MOZ_ALWAYS_INLINE bool
IsArraySpecies(JSContext* cx, HandleObject origArray)
{
    if (MOZ_UNLIKELY(origArray->is<ProxyObject>())) {
        if (origArray->getClass()->isDOMClass()) {
#ifdef DEBUG
            // We assume DOM proxies never return true for IsArray.
            IsArrayAnswer answer;
            MOZ_ASSERT(Proxy::isArray(cx, origArray, &answer));
            MOZ_ASSERT(answer == IsArrayAnswer::NotArray);
#endif
            return true;
        }
        return false;
    }

    // 9.4.2.3 Step 4. Non-array objects always use the default constructor.
    if (!origArray->is<ArrayObject>())
        return true;

    if (cx->compartment()->arraySpeciesLookup.tryOptimizeArray(cx, &origArray->as<ArrayObject>()))
        return true;

    Value ctor;
    if (!GetPropertyPure(cx, origArray, NameToId(cx->names().constructor), &ctor))
        return false;

    if (!IsArrayConstructor(ctor))
        return ctor.isUndefined();

    jsid speciesId = SYMBOL_TO_JSID(cx->wellKnownSymbols().species);
    JSFunction* getter;
    if (!GetGetterPure(cx, &ctor.toObject(), speciesId, &getter))
        return false;

    if (!getter)
        return false;

    return IsSelfHostedFunctionWithName(getter, cx->names().ArraySpecies);
}

static bool
ArraySpeciesCreate(JSContext* cx, HandleObject origArray, uint64_t length, MutableHandleObject arr)
{
    MOZ_ASSERT(length < DOUBLE_INTEGRAL_PRECISION_LIMIT);

    FixedInvokeArgs<2> args(cx);

    args[0].setObject(*origArray);
    args[1].set(NumberValue(length));

    RootedValue rval(cx);
    if (!CallSelfHostedFunction(cx, cx->names().ArraySpeciesCreate, UndefinedHandleValue, args,
                                &rval))
    {
        return false;
    }

    MOZ_ASSERT(rval.isObject());
    arr.set(&rval.toObject());
    return true;
}

static bool
array_toSource(JSContext* cx, unsigned argc, Value* vp)
{
    if (!CheckRecursionLimit(cx))
        return false;

    CallArgs args = CallArgsFromVp(argc, vp);

    if (!args.thisv().isObject()) {
        ReportIncompatible(cx, args);
        return false;
    }

    Rooted<JSObject*> obj(cx, &args.thisv().toObject());
    RootedValue elt(cx);

    AutoCycleDetector detector(cx, obj);
    if (!detector.init())
        return false;

    StringBuffer sb(cx);

    if (detector.foundCycle()) {
        if (!sb.append("[]"))
            return false;
        goto make_string;
    }

    if (!sb.append('['))
        return false;

    uint64_t length;
    if (!GetLengthProperty(cx, obj, &length))
        return false;

    for (uint64_t index = 0; index < length; index++) {
        bool hole;
        if (!CheckForInterrupt(cx) ||
            !HasAndGetElement(cx, obj, index, &hole, &elt)) {
            return false;
        }

        /* Get element's character string. */
        JSString* str;
        if (hole) {
            str = cx->runtime()->emptyString;
        } else {
            str = ValueToSource(cx, elt);
            if (!str)
                return false;
        }

        /* Append element to buffer. */
        if (!sb.append(str))
            return false;
        if (index + 1 != length) {
            if (!sb.append(", "))
                return false;
        } else if (hole) {
            if (!sb.append(','))
                return false;
        }
    }

    /* Finalize the buffer. */
    if (!sb.append(']'))
        return false;

  make_string:
    JSString* str = sb.finishString();
    if (!str)
        return false;

    args.rval().setString(str);
    return true;
}

struct EmptySeparatorOp
{
    bool operator()(JSContext*, StringBuffer& sb) { return true; }
};

template <typename CharT>
struct CharSeparatorOp
{
    const CharT sep;
    explicit CharSeparatorOp(CharT sep) : sep(sep) {}
    bool operator()(JSContext*, StringBuffer& sb) { return sb.append(sep); }
};

struct StringSeparatorOp
{
    HandleLinearString sep;

    explicit StringSeparatorOp(HandleLinearString sep) : sep(sep) {}

    bool operator()(JSContext* cx, StringBuffer& sb) {
        return sb.append(sep);
    }
};

template <typename SeparatorOp>
static bool
ArrayJoinDenseKernel(JSContext* cx, SeparatorOp sepOp, HandleNativeObject obj, uint64_t length,
                     StringBuffer& sb, uint32_t* numProcessed)
{
    // This loop handles all elements up to initializedLength. If
    // length > initLength we rely on the second loop to add the
    // other elements.
    MOZ_ASSERT(*numProcessed == 0);
    uint64_t initLength = Min<uint64_t>(obj->getDenseInitializedLength(),
                                        length);
    MOZ_ASSERT(initLength <= UINT32_MAX, "initialized length shouldn't exceed UINT32_MAX");
    uint32_t initLengthClamped = uint32_t(initLength);
    while (*numProcessed < initLengthClamped) {
        if (!CheckForInterrupt(cx))
            return false;

        // Step 7.b.
        Value elem = obj->getDenseElement(*numProcessed);

        // Steps 7.c-d.
        if (elem.isString()) {
            if (!sb.append(elem.toString()))
                return false;
        } else if (elem.isNumber()) {
            if (!NumberValueToStringBuffer(cx, elem, sb))
                return false;
        } else if (elem.isBoolean()) {
            if (!BooleanToStringBuffer(elem.toBoolean(), sb))
                return false;
        } else if (elem.isObject() || elem.isSymbol()) {
            /*
             * Object stringifying could modify the initialized length or make
             * the array sparse. Delegate it to a separate loop to keep this
             * one tight.
             *
             * Symbol stringifying is a TypeError, so into the slow path
             * with those as well.
             */
            break;
        } else {
            MOZ_ASSERT(elem.isMagic(JS_ELEMENTS_HOLE) || elem.isNullOrUndefined());
        }

        // Steps 7.a, 7.e.
        if (++(*numProcessed) != length && !sepOp(cx, sb))
            return false;
    }

    return true;
}

template <typename SeparatorOp>
static bool
ArrayJoinKernel(JSContext* cx, SeparatorOp sepOp, HandleObject obj, uint64_t length,
               StringBuffer& sb)
{
    // Step 6.
    uint32_t numProcessed = 0;

    if (IsPackedArrayOrNoExtraIndexedProperties(obj, length)) {
        if (!ArrayJoinDenseKernel<SeparatorOp>(cx, sepOp, obj.as<NativeObject>(), length, sb,
                                               &numProcessed))
        {
            return false;
        }
    }

    // Step 7.
    if (numProcessed != length) {
        RootedValue v(cx);
        for (uint64_t i = numProcessed; i < length; ) {
            if (!CheckForInterrupt(cx))
                return false;

            // Step 7.b.
            if (!GetArrayElement(cx, obj, i, &v))
                return false;

            // Steps 7.c-d.
            if (!v.isNullOrUndefined()) {
                if (!ValueToStringBuffer(cx, v, sb))
                    return false;
            }

            // Steps 7.a, 7.e.
            if (++i != length && !sepOp(cx, sb))
                return false;
        }
    }

    return true;
}

// ES2017 draft rev 1b0184bc17fc09a8ddcf4aeec9b6d9fcac4eafce
// 22.1.3.13 Array.prototype.join ( separator )
bool
js::array_join(JSContext* cx, unsigned argc, Value* vp)
{
    if (!CheckRecursionLimit(cx))
        return false;

    AutoGeckoProfilerEntry pseudoFrame(cx, "Array.prototype.join");
    CallArgs args = CallArgsFromVp(argc, vp);

    // Step 1.
    RootedObject obj(cx, ToObject(cx, args.thisv()));
    if (!obj)
        return false;

    AutoCycleDetector detector(cx, obj);
    if (!detector.init())
        return false;

    if (detector.foundCycle()) {
        args.rval().setString(cx->names().empty);
        return true;
    }

    // Step 2.
    uint64_t length;
    if (!GetLengthProperty(cx, obj, &length))
        return false;

    // Steps 3-4.
    RootedLinearString sepstr(cx);
    if (args.hasDefined(0)) {
        JSString *s = ToString<CanGC>(cx, args[0]);
        if (!s)
            return false;
        sepstr = s->ensureLinear(cx);
        if (!sepstr)
            return false;
    } else {
        sepstr = cx->names().comma;
    }

    // Steps 5-8 (When the length is zero, directly return the empty string).
    if (length == 0) {
        args.rval().setString(cx->emptyString());
        return true;
    }

    // An optimized version of a special case of steps 5-8: when length==1 and
    // the 0th element is a string, ToString() of that element is a no-op and
    // so it can be immediately returned as the result.
    if (length == 1 && obj->isNative()) {
        NativeObject* nobj = &obj->as<NativeObject>();
        if (nobj->getDenseInitializedLength() == 1) {
            Value elem0 = nobj->getDenseElement(0);
            if (elem0.isString()) {
                args.rval().set(elem0);
                return true;
            }
        }
    }

    // Step 5.
    StringBuffer sb(cx);
    if (sepstr->hasTwoByteChars() && !sb.ensureTwoByteChars())
        return false;

    // The separator will be added |length - 1| times, reserve space for that
    // so that we don't have to unnecessarily grow the buffer.
    size_t seplen = sepstr->length();
    if (seplen > 0) {
        if (length > UINT32_MAX) {
            ReportAllocationOverflow(cx);
            return false;
        }
        CheckedInt<uint32_t> res = CheckedInt<uint32_t>(seplen) * (uint32_t(length) - 1);
        if (!res.isValid()) {
            ReportAllocationOverflow(cx);
            return false;
        }

        if (!sb.reserve(res.value()))
            return false;
    }

    // Various optimized versions of steps 6-7.
    if (seplen == 0) {
        EmptySeparatorOp op;
        if (!ArrayJoinKernel(cx, op, obj, length, sb))
            return false;
    } else if (seplen == 1) {
        char16_t c = sepstr->latin1OrTwoByteChar(0);
        if (c <= JSString::MAX_LATIN1_CHAR) {
            CharSeparatorOp<Latin1Char> op(c);
            if (!ArrayJoinKernel(cx, op, obj, length, sb))
                return false;
        } else {
            CharSeparatorOp<char16_t> op(c);
            if (!ArrayJoinKernel(cx, op, obj, length, sb))
                return false;
        }
    } else {
        StringSeparatorOp op(sepstr);
        if (!ArrayJoinKernel(cx, op, obj, length, sb))
            return false;
    }

    // Step 8.
    JSString* str = sb.finishString();
    if (!str)
        return false;

    args.rval().setString(str);
    return true;
}

// ES2017 draft rev f8a9be8ea4bd97237d176907a1e3080dce20c68f
// 22.1.3.27 Array.prototype.toLocaleString ([ reserved1 [ , reserved2 ] ])
// ES2017 Intl draft rev 78bbe7d1095f5ff3760ac4017ed366026e4cb276
// 13.4.1 Array.prototype.toLocaleString ([ locales [ , options ]])
static bool
array_toLocaleString(JSContext* cx, unsigned argc, Value* vp)
{
    if (!CheckRecursionLimit(cx))
        return false;

    CallArgs args = CallArgsFromVp(argc, vp);

    // Step 1
    RootedObject obj(cx, ToObject(cx, args.thisv()));
    if (!obj)
        return false;

    // Avoid calling into self-hosted code if the array is empty.
    if (obj->is<ArrayObject>() && obj->as<ArrayObject>().length() == 0) {
        args.rval().setString(cx->names().empty);
        return true;
    }

    AutoCycleDetector detector(cx, obj);
    if (!detector.init())
        return false;

    if (detector.foundCycle()) {
        args.rval().setString(cx->names().empty);
        return true;
    }

    FixedInvokeArgs<2> args2(cx);

    args2[0].set(args.get(0));
    args2[1].set(args.get(1));

    // Steps 2-10.
    RootedValue thisv(cx, ObjectValue(*obj));
    return CallSelfHostedFunction(cx, cx->names().ArrayToLocaleString, thisv, args2, args.rval());
}

/* vector must point to rooted memory. */
static bool
SetArrayElements(JSContext* cx, HandleObject obj, uint64_t start,
                 uint32_t count, const Value* vector,
                 ShouldUpdateTypes updateTypes = ShouldUpdateTypes::Update)
{
    MOZ_ASSERT(count <= MAX_ARRAY_INDEX);
    MOZ_ASSERT(start + count < uint64_t(DOUBLE_INTEGRAL_PRECISION_LIMIT));

    if (count == 0)
        return true;

    if (!ObjectMayHaveExtraIndexedProperties(obj) && start <= UINT32_MAX) {
        NativeObject* nobj = &obj->as<NativeObject>();
        DenseElementResult result = nobj->setOrExtendDenseElements(cx, uint32_t(start), vector,
                                                                   count, updateTypes);
        if (result != DenseElementResult::Incomplete)
            return result == DenseElementResult::Success;
    }

    RootedId id(cx);
    const Value* end = vector + count;
    while (vector < end) {
        if (!CheckForInterrupt(cx))
            return false;

        if (!ToId(cx, start++, &id))
            return false;

        if (!SetProperty(cx, obj, id, HandleValue::fromMarkedLocation(vector++)))
            return false;
    }

    return true;
}

static DenseElementResult
ArrayReverseDenseKernel(JSContext* cx, HandleNativeObject obj, uint32_t length)
{
    MOZ_ASSERT(length > 1);

    // If there are no elements, we're done.
    if (obj->getDenseInitializedLength() == 0)
        return DenseElementResult::Success;

    if (obj->denseElementsAreFrozen())
        return DenseElementResult::Incomplete;

    if (!IsPackedArray(obj)) {
        /*
         * It's actually surprisingly complicated to reverse an array due
         * to the orthogonality of array length and array capacity while
         * handling leading and trailing holes correctly.  Reversing seems
         * less likely to be a common operation than other array
         * mass-mutation methods, so for now just take a probably-small
         * memory hit (in the absence of too many holes in the array at
         * its start) and ensure that the capacity is sufficient to hold
         * all the elements in the array if it were full.
         */
        DenseElementResult result = obj->ensureDenseElements(cx, length, 0);
        if (result != DenseElementResult::Success)
            return result;

        /* Fill out the array's initialized length to its proper length. */
        obj->ensureDenseInitializedLength(cx, length, 0);
    } else {
        if (!obj->maybeCopyElementsForWrite(cx))
            return DenseElementResult::Failure;
    }

    if (!MaybeInIteration(obj, cx) && !cx->zone()->needsIncrementalBarrier()) {
        obj->reverseDenseElementsNoPreBarrier(length);
        return DenseElementResult::Success;
    }

    RootedValue origlo(cx), orighi(cx);

    uint32_t lo = 0, hi = length - 1;
    for (; lo < hi; lo++, hi--) {
        origlo = obj->getDenseElement(lo);
        orighi = obj->getDenseElement(hi);
        obj->setDenseElement(lo, orighi);
        if (orighi.isMagic(JS_ELEMENTS_HOLE) &&
            !SuppressDeletedProperty(cx, obj, INT_TO_JSID(lo)))
        {
            return DenseElementResult::Failure;
        }
        obj->setDenseElement(hi, origlo);
        if (origlo.isMagic(JS_ELEMENTS_HOLE) &&
            !SuppressDeletedProperty(cx, obj, INT_TO_JSID(hi)))
        {
            return DenseElementResult::Failure;
        }
    }

    return DenseElementResult::Success;
}

// ES2017 draft rev 1b0184bc17fc09a8ddcf4aeec9b6d9fcac4eafce
// 22.1.3.21 Array.prototype.reverse ( )
bool
js::array_reverse(JSContext* cx, unsigned argc, Value* vp)
{
    AutoGeckoProfilerEntry pseudoFrame(cx, "Array.prototype.reverse");
    CallArgs args = CallArgsFromVp(argc, vp);

    // Step 1.
    RootedObject obj(cx, ToObject(cx, args.thisv()));
    if (!obj)
        return false;

    // Step 2.
    uint64_t len;
    if (!GetLengthProperty(cx, obj, &len))
        return false;

    // An empty array or an array with length 1 is already reversed.
    if (len <= 1) {
        args.rval().setObject(*obj);
        return true;
    }

    if (IsPackedArrayOrNoExtraIndexedProperties(obj, len) && len <= UINT32_MAX) {
        DenseElementResult result =
            ArrayReverseDenseKernel(cx, obj.as<NativeObject>(), uint32_t(len));
        if (result != DenseElementResult::Incomplete) {
            /*
             * Per ECMA-262, don't update the length of the array, even if the new
             * array has trailing holes (and thus the original array began with
             * holes).
             */
            args.rval().setObject(*obj);
            return result == DenseElementResult::Success;
        }
    }

    // Steps 3-5.
    RootedValue lowval(cx), hival(cx);
    for (uint64_t i = 0, half = len / 2; i < half; i++) {
        bool hole, hole2;
        if (!CheckForInterrupt(cx) ||
            !HasAndGetElement(cx, obj, i, &hole, &lowval) ||
            !HasAndGetElement(cx, obj, len - i - 1, &hole2, &hival))
        {
            return false;
        }

        if (!hole && !hole2) {
            if (!SetArrayElement(cx, obj, i, hival))
                return false;
            if (!SetArrayElement(cx, obj, len - i - 1, lowval))
                return false;
        } else if (hole && !hole2) {
            if (!SetArrayElement(cx, obj, i, hival))
                return false;
            if (!DeletePropertyOrThrow(cx, obj, len - i - 1))
                return false;
        } else if (!hole && hole2) {
            if (!DeletePropertyOrThrow(cx, obj, i))
                return false;
            if (!SetArrayElement(cx, obj, len - i - 1, lowval))
                return false;
        } else {
            // No action required.
        }
    }

    // Step 6.
    args.rval().setObject(*obj);
    return true;
}

static inline bool
CompareStringValues(JSContext* cx, const Value& a, const Value& b, bool* lessOrEqualp)
{
    if (!CheckForInterrupt(cx))
        return false;

    JSString* astr = a.toString();
    JSString* bstr = b.toString();
    int32_t result;
    if (!CompareStrings(cx, astr, bstr, &result))
        return false;

    *lessOrEqualp = (result <= 0);
    return true;
}

static const uint64_t powersOf10[] = {
    1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000, 1000000000, 1000000000000ULL
};

static inline unsigned
NumDigitsBase10(uint32_t n)
{
    /*
     * This is just floor_log10(n) + 1
     * Algorithm taken from
     * http://graphics.stanford.edu/~seander/bithacks.html#IntegerLog10
     */
    uint32_t log2 = CeilingLog2(n);
    uint32_t t = log2 * 1233 >> 12;
    return t - (n < powersOf10[t]) + 1;
}

static inline bool
CompareLexicographicInt32(const Value& a, const Value& b, bool* lessOrEqualp)
{
    int32_t aint = a.toInt32();
    int32_t bint = b.toInt32();

    /*
     * If both numbers are equal ... trivial
     * If only one of both is negative --> arithmetic comparison as char code
     * of '-' is always less than any other digit
     * If both numbers are negative convert them to positive and continue
     * handling ...
     */
    if (aint == bint) {
        *lessOrEqualp = true;
    } else if ((aint < 0) && (bint >= 0)) {
        *lessOrEqualp = true;
    } else if ((aint >= 0) && (bint < 0)) {
        *lessOrEqualp = false;
    } else {
        uint32_t auint = Abs(aint);
        uint32_t buint = Abs(bint);

        /*
         *  ... get number of digits of both integers.
         * If they have the same number of digits --> arithmetic comparison.
         * If digits_a > digits_b: a < b*10e(digits_a - digits_b).
         * If digits_b > digits_a: a*10e(digits_b - digits_a) <= b.
         */
        unsigned digitsa = NumDigitsBase10(auint);
        unsigned digitsb = NumDigitsBase10(buint);
        if (digitsa == digitsb) {
            *lessOrEqualp = (auint <= buint);
        } else if (digitsa > digitsb) {
            MOZ_ASSERT((digitsa - digitsb) < ArrayLength(powersOf10));
            *lessOrEqualp = (uint64_t(auint) < uint64_t(buint) * powersOf10[digitsa - digitsb]);
        } else { /* if (digitsb > digitsa) */
            MOZ_ASSERT((digitsb - digitsa) < ArrayLength(powersOf10));
            *lessOrEqualp = (uint64_t(auint) * powersOf10[digitsb - digitsa] <= uint64_t(buint));
        }
    }

    return true;
}

template <typename Char1, typename Char2>
static inline bool
CompareSubStringValues(JSContext* cx, const Char1* s1, size_t len1, const Char2* s2, size_t len2,
                       bool* lessOrEqualp)
{
    if (!CheckForInterrupt(cx))
        return false;

    if (!s1 || !s2)
        return false;

    int32_t result = CompareChars(s1, len1, s2, len2);
    *lessOrEqualp = (result <= 0);
    return true;
}

namespace {

struct SortComparatorStrings
{
    JSContext*  const cx;

    explicit SortComparatorStrings(JSContext* cx)
      : cx(cx) {}

    bool operator()(const Value& a, const Value& b, bool* lessOrEqualp) {
        return CompareStringValues(cx, a, b, lessOrEqualp);
    }
};

struct SortComparatorLexicographicInt32
{
    bool operator()(const Value& a, const Value& b, bool* lessOrEqualp) {
        return CompareLexicographicInt32(a, b, lessOrEqualp);
    }
};

struct StringifiedElement
{
    size_t charsBegin;
    size_t charsEnd;
    size_t elementIndex;
};

struct SortComparatorStringifiedElements
{
    JSContext*          const cx;
    const StringBuffer& sb;

    SortComparatorStringifiedElements(JSContext* cx, const StringBuffer& sb)
      : cx(cx), sb(sb) {}

    bool operator()(const StringifiedElement& a, const StringifiedElement& b, bool* lessOrEqualp) {
        size_t lenA = a.charsEnd - a.charsBegin;
        size_t lenB = b.charsEnd - b.charsBegin;

        if (sb.isUnderlyingBufferLatin1()) {
            return CompareSubStringValues(cx, sb.rawLatin1Begin() + a.charsBegin, lenA,
                                          sb.rawLatin1Begin() + b.charsBegin, lenB,
                                          lessOrEqualp);
        }

        return CompareSubStringValues(cx, sb.rawTwoByteBegin() + a.charsBegin, lenA,
                                      sb.rawTwoByteBegin() + b.charsBegin, lenB,
                                      lessOrEqualp);
    }
};

struct NumericElement
{
    double dv;
    size_t elementIndex;
};

static bool
ComparatorNumericLeftMinusRight(const NumericElement& a, const NumericElement& b,
                                bool* lessOrEqualp)
{
    *lessOrEqualp = (a.dv <= b.dv);
    return true;
}

static bool
ComparatorNumericRightMinusLeft(const NumericElement& a, const NumericElement& b,
                                bool* lessOrEqualp)
{
    *lessOrEqualp = (b.dv <= a.dv);
    return true;
}

typedef bool (*ComparatorNumeric)(const NumericElement& a, const NumericElement& b,
                                  bool* lessOrEqualp);

static const ComparatorNumeric SortComparatorNumerics[] = {
    nullptr,
    nullptr,
    ComparatorNumericLeftMinusRight,
    ComparatorNumericRightMinusLeft
};

static bool
ComparatorInt32LeftMinusRight(const Value& a, const Value& b, bool* lessOrEqualp)
{
    *lessOrEqualp = (a.toInt32() <= b.toInt32());
    return true;
}

static bool
ComparatorInt32RightMinusLeft(const Value& a, const Value& b, bool* lessOrEqualp)
{
    *lessOrEqualp = (b.toInt32() <= a.toInt32());
    return true;
}

typedef bool (*ComparatorInt32)(const Value& a, const Value& b, bool* lessOrEqualp);

static const ComparatorInt32 SortComparatorInt32s[] = {
    nullptr,
    nullptr,
    ComparatorInt32LeftMinusRight,
    ComparatorInt32RightMinusLeft
};

// Note: Values for this enum must match up with SortComparatorNumerics
// and SortComparatorInt32s.
enum ComparatorMatchResult {
    Match_Failure = 0,
    Match_None,
    Match_LeftMinusRight,
    Match_RightMinusLeft
};

} // namespace


/*
 * Specialize behavior for comparator functions with particular common bytecode
 * patterns: namely, |return x - y| and |return y - x|.
 */
static ComparatorMatchResult
MatchNumericComparator(JSContext* cx, JSObject* obj)
{
    if (!obj->is<JSFunction>())
        return Match_None;

    RootedFunction fun(cx, &obj->as<JSFunction>());
    if (!fun->isInterpreted() || fun->isClassConstructor())
        return Match_None;

    JSScript* script = JSFunction::getOrCreateScript(cx, fun);
    if (!script)
        return Match_Failure;

    jsbytecode* pc = script->code();

    uint16_t arg0, arg1;
    if (JSOp(*pc) != JSOP_GETARG)
        return Match_None;
    arg0 = GET_ARGNO(pc);
    pc += JSOP_GETARG_LENGTH;

    if (JSOp(*pc) != JSOP_GETARG)
        return Match_None;
    arg1 = GET_ARGNO(pc);
    pc += JSOP_GETARG_LENGTH;

    if (JSOp(*pc) != JSOP_SUB)
        return Match_None;
    pc += JSOP_SUB_LENGTH;

    if (JSOp(*pc) != JSOP_RETURN)
        return Match_None;

    if (arg0 == 0 && arg1 == 1)
        return Match_LeftMinusRight;

    if (arg0 == 1 && arg1 == 0)
        return Match_RightMinusLeft;

    return Match_None;
}

template <typename K, typename C>
static inline bool
MergeSortByKey(K keys, size_t len, K scratch, C comparator, MutableHandle<GCVector<Value>> vec)
{
    MOZ_ASSERT(vec.length() >= len);

    /* Sort keys. */
    if (!MergeSort(keys, len, scratch, comparator))
        return false;

    /*
     * Reorder vec by keys in-place, going element by element.  When an out-of-
     * place element is encountered, move that element to its proper position,
     * displacing whatever element was at *that* point to its proper position,
     * and so on until an element must be moved to the current position.
     *
     * At each outer iteration all elements up to |i| are sorted.  If
     * necessary each inner iteration moves some number of unsorted elements
     * (including |i|) directly to sorted position.  Thus on completion |*vec|
     * is sorted, and out-of-position elements have moved once.  Complexity is
     * Θ(len) + O(len) == O(2*len), with each element visited at most twice.
     */
    for (size_t i = 0; i < len; i++) {
        size_t j = keys[i].elementIndex;
        if (i == j)
            continue; // fixed point

        MOZ_ASSERT(j > i, "Everything less than |i| should be in the right place!");
        Value tv = vec[j];
        do {
            size_t k = keys[j].elementIndex;
            keys[j].elementIndex = j;
            vec[j].set(vec[k]);
            j = k;
        } while (j != i);

        // We could assert the loop invariant that |i == keys[i].elementIndex|
        // here if we synced |keys[i].elementIndex|.  But doing so would render
        // the assertion vacuous, so don't bother, even in debug builds.
        vec[i].set(tv);
    }

    return true;
}

/*
 * Sort Values as strings.
 *
 * To minimize #conversions, SortLexicographically() first converts all Values
 * to strings at once, then sorts the elements by these cached strings.
 */
static bool
SortLexicographically(JSContext* cx, MutableHandle<GCVector<Value>> vec, size_t len)
{
    MOZ_ASSERT(vec.length() >= len);

    StringBuffer sb(cx);
    Vector<StringifiedElement, 0, TempAllocPolicy> strElements(cx);

    /* MergeSort uses the upper half as scratch space. */
    if (!strElements.resize(2 * len))
        return false;

    /* Convert Values to strings. */
    size_t cursor = 0;
    for (size_t i = 0; i < len; i++) {
        if (!CheckForInterrupt(cx))
            return false;

        if (!ValueToStringBuffer(cx, vec[i], sb))
            return false;

        strElements[i] = { cursor, sb.length(), i };
        cursor = sb.length();
    }

    /* Sort Values in vec alphabetically. */
    return MergeSortByKey(strElements.begin(), len, strElements.begin() + len,
                          SortComparatorStringifiedElements(cx, sb), vec);
}

/*
 * Sort Values as numbers.
 *
 * To minimize #conversions, SortNumerically first converts all Values to
 * numerics at once, then sorts the elements by these cached numerics.
 */
static bool
SortNumerically(JSContext* cx, MutableHandle<GCVector<Value>> vec, size_t len,
                ComparatorMatchResult comp)
{
    MOZ_ASSERT(vec.length() >= len);

    Vector<NumericElement, 0, TempAllocPolicy> numElements(cx);

    /* MergeSort uses the upper half as scratch space. */
    if (!numElements.resize(2 * len))
        return false;

    /* Convert Values to numerics. */
    for (size_t i = 0; i < len; i++) {
        if (!CheckForInterrupt(cx))
            return false;

        double dv;
        if (!ToNumber(cx, vec[i], &dv))
            return false;

        numElements[i] = { dv, i };
    }

    /* Sort Values in vec numerically. */
    return MergeSortByKey(numElements.begin(), len, numElements.begin() + len,
                          SortComparatorNumerics[comp], vec);
}

static bool
FillWithUndefined(JSContext* cx, HandleObject obj, uint32_t start, uint32_t count)
{
    MOZ_ASSERT(start < start + count, "count > 0 and start + count doesn't overflow");

    do {
        if (ObjectMayHaveExtraIndexedProperties(obj))
            break;

        NativeObject* nobj = &obj->as<NativeObject>();
        if (nobj->denseElementsAreFrozen())
            break;

        if (obj->is<ArrayObject>() &&
            !obj->as<ArrayObject>().lengthIsWritable() &&
            start + count >= obj->as<ArrayObject>().length())
        {
            break;
        }

        DenseElementResult result = nobj->ensureDenseElements(cx, start, count);
        if (result != DenseElementResult::Success) {
            if (result == DenseElementResult::Failure)
                return false;
            MOZ_ASSERT(result == DenseElementResult::Incomplete);
            break;
        }

        if (obj->is<ArrayObject>() && start + count >= obj->as<ArrayObject>().length())
            obj->as<ArrayObject>().setLengthInt32(start + count);

        for (uint32_t i = 0; i < count; i++)
            nobj->setDenseElementWithType(cx, start + i, UndefinedHandleValue);

        return true;
    } while (false);

    for (uint32_t i = 0; i < count; i++) {
        if (!CheckForInterrupt(cx) || !SetArrayElement(cx, obj, start + i, UndefinedHandleValue))
            return false;
    }

    return true;
}

bool
js::intrinsic_ArrayNativeSort(JSContext* cx, unsigned argc, Value* vp)
{
    // This function is called from the self-hosted Array.prototype.sort
    // implementation. It returns |true| if the array was sorted, otherwise it
    // returns |false| to notify the self-hosted code to perform the sorting.
    CallArgs args = CallArgsFromVp(argc, vp);
    MOZ_ASSERT(args.length() == 1);

    HandleValue fval = args[0];
    MOZ_ASSERT(fval.isUndefined() || IsCallable(fval));

    ComparatorMatchResult comp;
    if (fval.isObject()) {
        comp = MatchNumericComparator(cx, &fval.toObject());
        if (comp == Match_Failure)
            return false;

        if (comp == Match_None) {
            // Non-optimized user supplied comparators perform much better when
            // called from within a self-hosted sorting function.
            args.rval().setBoolean(false);
            return true;
        }
    } else {
        comp = Match_None;
    }

    RootedObject obj(cx, &args.thisv().toObject());

    uint64_t length;
    if (!GetLengthProperty(cx, obj, &length))
        return false;
    if (length < 2) {
        /* [] and [a] remain unchanged when sorted. */
        args.rval().setBoolean(true);
        return true;
    }

    if (length > UINT32_MAX) {
        ReportAllocationOverflow(cx);
        return false;
    }
    uint32_t len = uint32_t(length);

    /*
     * We need a temporary array of 2 * len Value to hold the array elements
     * and the scratch space for merge sort. Check that its size does not
     * overflow size_t, which would allow for indexing beyond the end of the
     * malloc'd vector.
     */
#if JS_BITS_PER_WORD == 32
    if (size_t(len) > size_t(-1) / (2 * sizeof(Value))) {
        ReportAllocationOverflow(cx);
        return false;
    }
#endif

    size_t n, undefs;
    {
        Rooted<GCVector<Value>> vec(cx, GCVector<Value>(cx));
        if (!vec.reserve(2 * size_t(len)))
            return false;

        /*
         * By ECMA 262, 15.4.4.11, a property that does not exist (which we
         * call a "hole") is always greater than an existing property with
         * value undefined and that is always greater than any other property.
         * Thus to sort holes and undefs we simply count them, sort the rest
         * of elements, append undefs after them and then make holes after
         * undefs.
         */
        undefs = 0;
        bool allStrings = true;
        bool allInts = true;
        bool extraIndexed;
        RootedValue v(cx);
        if (IsPackedArray(obj)) {
            HandleArrayObject array = obj.as<ArrayObject>();
            extraIndexed = false;

            for (uint32_t i = 0; i < len; i++) {
                if (!CheckForInterrupt(cx))
                    return false;

                v.set(array->getDenseElement(i));
                MOZ_ASSERT(!v.isMagic(JS_ELEMENTS_HOLE));
                if (v.isUndefined()) {
                    ++undefs;
                    continue;
                }
                vec.infallibleAppend(v);
                allStrings = allStrings && v.isString();
                allInts = allInts && v.isInt32();
            }
        } else {
            extraIndexed = ObjectMayHaveExtraIndexedProperties(obj);

            for (uint32_t i = 0; i < len; i++) {
                if (!CheckForInterrupt(cx))
                    return false;

                bool hole;
                if (!HasAndGetElement(cx, obj, i, &hole, &v))
                    return false;
                if (hole)
                    continue;
                if (v.isUndefined()) {
                    ++undefs;
                    continue;
                }
                vec.infallibleAppend(v);
                allStrings = allStrings && v.isString();
                allInts = allInts && v.isInt32();
            }
        }

        /*
         * If the array only contains holes, we're done.  But if it contains
         * undefs, those must be sorted to the front of the array.
         */
        n = vec.length();
        if (n == 0 && undefs == 0) {
            args.rval().setBoolean(true);
            return true;
        }

        /* Here len == n + undefs + number_of_holes. */
        if (comp == Match_None) {
            /*
             * Sort using the default comparator converting all elements to
             * strings.
             */
            if (allStrings) {
                JS_ALWAYS_TRUE(vec.resize(n * 2));
                if (!MergeSort(vec.begin(), n, vec.begin() + n, SortComparatorStrings(cx)))
                    return false;
            } else if (allInts) {
                JS_ALWAYS_TRUE(vec.resize(n * 2));
                if (!MergeSort(vec.begin(), n, vec.begin() + n,
                               SortComparatorLexicographicInt32())) {
                    return false;
                }
            } else {
                if (!SortLexicographically(cx, &vec, n))
                    return false;
            }
        } else {
            if (allInts) {
                JS_ALWAYS_TRUE(vec.resize(n * 2));
                if (!MergeSort(vec.begin(), n, vec.begin() + n, SortComparatorInt32s[comp]))
                    return false;
            } else {
                if (!SortNumerically(cx, &vec, n, comp))
                    return false;
            }
        }

        // We can omit the type update when neither collecting the elements
        // nor calling the default comparator can execute a (getter) function
        // that might run user code.
        ShouldUpdateTypes updateTypes = !extraIndexed && (allStrings || allInts)
                                        ? ShouldUpdateTypes::DontUpdate
                                        : ShouldUpdateTypes::Update;
        if (!SetArrayElements(cx, obj, 0, uint32_t(n), vec.begin(), updateTypes))
            return false;
    }

    /* Set undefs that sorted after the rest of elements. */
    if (undefs > 0) {
        if (!FillWithUndefined(cx, obj, n, undefs))
            return false;
        n += undefs;
    }

    /* Re-create any holes that sorted to the end of the array. */
    while (len > n) {
        if (!CheckForInterrupt(cx) || !DeletePropertyOrThrow(cx, obj, --len))
            return false;
    }
    args.rval().setBoolean(true);
    return true;
}

bool
js::NewbornArrayPush(JSContext* cx, HandleObject obj, const Value& v)
{
    HandleArrayObject arr = obj.as<ArrayObject>();

    MOZ_ASSERT(!v.isMagic());
    MOZ_ASSERT(arr->lengthIsWritable());

    uint32_t length = arr->length();
    MOZ_ASSERT(length <= arr->getDenseCapacity());

    if (!arr->ensureElements(cx, length + 1))
        return false;

    arr->setDenseInitializedLength(length + 1);
    arr->setLengthInt32(length + 1);
    arr->initDenseElementWithType(cx, length, v);
    return true;
}

// ES2017 draft rev 1b0184bc17fc09a8ddcf4aeec9b6d9fcac4eafce
// 22.1.3.18 Array.prototype.push ( ...items )
bool
js::array_push(JSContext* cx, unsigned argc, Value* vp)
{
    AutoGeckoProfilerEntry pseudoFrame(cx, "Array.prototype.push");
    CallArgs args = CallArgsFromVp(argc, vp);

    // Step 1.
    RootedObject obj(cx, ToObject(cx, args.thisv()));
    if (!obj)
        return false;

    // Step 2.
    uint64_t length;
    if (!GetLengthProperty(cx, obj, &length))
        return false;

    if (!ObjectMayHaveExtraIndexedProperties(obj) && length <= UINT32_MAX) {
        DenseElementResult result =
            obj->as<NativeObject>().setOrExtendDenseElements(cx, uint32_t(length),
                                                             args.array(), args.length());
        if (result != DenseElementResult::Incomplete) {
            if (result == DenseElementResult::Failure)
                return false;

            uint32_t newlength = uint32_t(length) + args.length();
            args.rval().setNumber(newlength);

            // setOrExtendDenseElements takes care of updating the length for
            // arrays. Handle updates to the length of non-arrays here.
            if (!obj->is<ArrayObject>()) {
                MOZ_ASSERT(obj->is<NativeObject>());
                return SetLengthProperty(cx, obj, newlength);
            }

            return true;
        }
    }

    // Step 5.
    uint64_t newlength = length + args.length();
    if (newlength >= uint64_t(DOUBLE_INTEGRAL_PRECISION_LIMIT)) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_TOO_LONG_ARRAY);
        return false;
    }

    // Steps 3-6.
    if (!SetArrayElements(cx, obj, length, args.length(), args.array()))
        return false;

    // Steps 7-8.
    args.rval().setNumber(double(newlength));
    return SetLengthProperty(cx, obj, newlength);
}

// ES2017 draft rev 1b0184bc17fc09a8ddcf4aeec9b6d9fcac4eafce
// 22.1.3.17 Array.prototype.pop ( )
bool
js::array_pop(JSContext* cx, unsigned argc, Value* vp)
{
    AutoGeckoProfilerEntry pseudoFrame(cx, "Array.prototype.pop");
    CallArgs args = CallArgsFromVp(argc, vp);

    // Step 1.
    RootedObject obj(cx, ToObject(cx, args.thisv()));
    if (!obj)
        return false;

    // Step 2.
    uint64_t index;
    if (!GetLengthProperty(cx, obj, &index))
        return false;

    // Steps 3-4.
    if (index == 0) {
        // Step 3.b.
        args.rval().setUndefined();
    } else {
        // Steps 4.a-b.
        index--;

        // Steps 4.c, 4.f.
        if (!GetArrayElement(cx, obj, index, args.rval()))
            return false;

        // Steps 4.d.
        if (!DeletePropertyOrThrow(cx, obj, index))
            return false;
    }

    // Steps 3.a, 4.e.
    return SetLengthProperty(cx, obj, index);
}

void
js::ArrayShiftMoveElements(NativeObject* obj)
{
    AutoUnsafeCallWithABI unsafe;
    MOZ_ASSERT_IF(obj->is<ArrayObject>(), obj->as<ArrayObject>().lengthIsWritable());

    size_t initlen = obj->getDenseInitializedLength();
    MOZ_ASSERT(initlen > 0);

    if (!obj->tryShiftDenseElements(1))
        obj->moveDenseElementsNoPreBarrier(0, 1, initlen - 1);
}

static inline void
SetInitializedLength(JSContext* cx, NativeObject* obj, size_t initlen)
{
    size_t oldInitlen = obj->getDenseInitializedLength();
    obj->setDenseInitializedLength(initlen);
    if (initlen < oldInitlen)
        obj->shrinkElements(cx, initlen);
}

static DenseElementResult
MoveDenseElements(JSContext* cx, NativeObject* obj, uint32_t dstStart, uint32_t srcStart,
                  uint32_t length)
{
    if (obj->denseElementsAreFrozen())
        return DenseElementResult::Incomplete;

    if (!obj->maybeCopyElementsForWrite(cx))
        return DenseElementResult::Failure;
    obj->moveDenseElements(dstStart, srcStart, length);

    return DenseElementResult::Success;
}

static DenseElementResult
ArrayShiftDenseKernel(JSContext* cx, HandleObject obj, MutableHandleValue rval)
{
    if (!IsPackedArray(obj) && ObjectMayHaveExtraIndexedProperties(obj))
        return DenseElementResult::Incomplete;

    if (MaybeInIteration(obj, cx))
        return DenseElementResult::Incomplete;

    size_t initlen = obj->as<NativeObject>().getDenseInitializedLength();
    if (initlen == 0)
        return DenseElementResult::Incomplete;

    rval.set(obj->as<NativeObject>().getDenseElement(0));
    if (rval.isMagic(JS_ELEMENTS_HOLE))
        rval.setUndefined();

    if (obj->as<NativeObject>().tryShiftDenseElements(1))
        return DenseElementResult::Success;

    DenseElementResult result = MoveDenseElements(cx, &obj->as<NativeObject>(), 0, 1, initlen - 1);
    if (result != DenseElementResult::Success)
        return result;

    SetInitializedLength(cx, obj.as<NativeObject>(), initlen - 1);
    return DenseElementResult::Success;
}

// ES2017 draft rev 1b0184bc17fc09a8ddcf4aeec9b6d9fcac4eafce
// 22.1.3.22 Array.prototype.shift ( )
bool
js::array_shift(JSContext* cx, unsigned argc, Value* vp)
{
    AutoGeckoProfilerEntry pseudoFrame(cx, "Array.prototype.shift");
    CallArgs args = CallArgsFromVp(argc, vp);

    // Step 1.
    RootedObject obj(cx, ToObject(cx, args.thisv()));
    if (!obj)
        return false;

    // Step 2.
    uint64_t len;
    if (!GetLengthProperty(cx, obj, &len))
        return false;

    // Step 3.
    if (len == 0) {
        // Step 3.a.
        if (!SetLengthProperty(cx, obj, uint32_t(0)))
            return false;

        // Step 3.b.
        args.rval().setUndefined();
        return true;
    }

    uint64_t newlen = len - 1;

    /* Fast paths. */
    uint64_t startIndex;
    DenseElementResult result = ArrayShiftDenseKernel(cx, obj, args.rval());
    if (result != DenseElementResult::Incomplete) {
        if (result == DenseElementResult::Failure)
            return false;

        if (len <= UINT32_MAX)
            return SetLengthProperty(cx, obj, newlen);

        startIndex = UINT32_MAX - 1;
    } else {
        // Steps 4, 9.
        if (!GetElement(cx, obj, 0, args.rval()))
            return false;

        startIndex = 0;
    }

    // Steps 5-6.
    RootedValue value(cx);
    for (uint64_t i = startIndex; i < newlen; i++) {
        if (!CheckForInterrupt(cx))
            return false;
        bool hole;
        if (!HasAndGetElement(cx, obj, i + 1, &hole, &value))
            return false;
        if (hole) {
            if (!DeletePropertyOrThrow(cx, obj, i))
                return false;
        } else {
            if (!SetArrayElement(cx, obj, i, value))
                return false;
        }
    }

    // Step 7.
    if (!DeletePropertyOrThrow(cx, obj, newlen))
        return false;

    // Step 8.
    return SetLengthProperty(cx, obj, newlen);
}

// ES2017 draft rev 1b0184bc17fc09a8ddcf4aeec9b6d9fcac4eafce
// 22.1.3.29 Array.prototype.unshift ( ...items )
bool
js::array_unshift(JSContext* cx, unsigned argc, Value* vp)
{
    AutoGeckoProfilerEntry pseudoFrame(cx, "Array.prototype.unshift");
    CallArgs args = CallArgsFromVp(argc, vp);

    // Step 1.
    RootedObject obj(cx, ToObject(cx, args.thisv()));
    if (!obj)
        return false;

    // Step 2.
    uint64_t length;
    if (!GetLengthProperty(cx, obj, &length))
        return false;

    // Steps 3-4.
    if (args.length() > 0) {
        bool optimized = false;
        do {
            if (length > UINT32_MAX)
                break;
            if (ObjectMayHaveExtraIndexedProperties(obj))
                break;
            if (MaybeInIteration(obj, cx))
                break;
            NativeObject* nobj = &obj->as<NativeObject>();
            if (nobj->denseElementsAreFrozen())
                break;
            if (nobj->is<ArrayObject>() && !nobj->as<ArrayObject>().lengthIsWritable())
                break;
            if (!nobj->tryUnshiftDenseElements(args.length())) {
                DenseElementResult result = nobj->ensureDenseElements(cx, uint32_t(length), args.length());
                if (result != DenseElementResult::Success) {
                    if (result == DenseElementResult::Failure)
                        return false;
                    MOZ_ASSERT(result == DenseElementResult::Incomplete);
                    break;
                }
                if (length > 0)
                    nobj->moveDenseElements(args.length(), 0, uint32_t(length));
            }
            for (uint32_t i = 0; i < args.length(); i++)
                nobj->setDenseElementWithType(cx, i, args[i]);
            optimized = true;
        } while (false);

        if (!optimized) {
            if (length > 0) {
                uint64_t last = length;
                uint64_t upperIndex = last + args.length();

                // Step 4.a.
                if (upperIndex >= uint64_t(DOUBLE_INTEGRAL_PRECISION_LIMIT)) {
                    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_TOO_LONG_ARRAY);
                    return false;
                }

                // Steps 4.b-c.
                RootedValue value(cx);
                do {
                    --last; --upperIndex;
                    if (!CheckForInterrupt(cx))
                        return false;
                    bool hole;
                    if (!HasAndGetElement(cx, obj, last, &hole, &value))
                        return false;
                    if (hole) {
                        if (!DeletePropertyOrThrow(cx, obj, upperIndex))
                            return false;
                    } else {
                        if (!SetArrayElement(cx, obj, upperIndex, value))
                            return false;
                    }
                } while (last != 0);
            }

            // Steps 4.d-f.
            /* Copy from args to the bottom of the array. */
            if (!SetArrayElements(cx, obj, 0, args.length(), args.array()))
                return false;
        }
    }

    // Step 5.
    uint64_t newlength = length + args.length();
    if (!SetLengthProperty(cx, obj, newlength))
        return false;

    // Step 6.
    /* Follow Perl by returning the new array length. */
    args.rval().setNumber(double(newlength));
    return true;
}

enum class ArrayAccess {
    Read, Write
};

/*
 * Returns true if this is a dense array whose properties ending at |endIndex|
 * (exclusive) may be accessed (get, set, delete) directly through its
 * contiguous vector of elements without fear of getters, setters, etc. along
 * the prototype chain, or of enumerators requiring notification of
 * modifications.
 */
template <ArrayAccess Access>
static bool
CanOptimizeForDenseStorage(HandleObject arr, uint64_t endIndex, JSContext* cx)
{
    /* If the desired properties overflow dense storage, we can't optimize. */
    if (endIndex > UINT32_MAX)
        return false;

    if (Access == ArrayAccess::Read) {
        /*
         * Dense storage read access is possible for any packed array as long
         * as we only access properties within the initialized length. In all
         * other cases we need to ensure there are no other indexed properties
         * on this object or on the prototype chain. Callers are required to
         * clamp the read length, so it doesn't exceed the initialized length.
         */
        if (IsPackedArray(arr) && endIndex <= arr->as<ArrayObject>().getDenseInitializedLength())
            return true;
        return !ObjectMayHaveExtraIndexedProperties(arr);
    }

    /* There's no optimizing possible if it's not an array. */
    if (!arr->is<ArrayObject>())
        return false;

    /* If the length is non-writable, always pick the slow path */
    if (!arr->as<ArrayObject>().lengthIsWritable())
        return false;

    MOZ_ASSERT(!arr->as<ArrayObject>().denseElementsAreFrozen(),
               "writable length implies elements are not frozen");

    /* Also pick the slow path if the object is being iterated over. */
    if (MaybeInIteration(arr, cx))
        return false;

    /* Or we attempt to write to indices outside the initialized length. */
    if (endIndex > arr->as<ArrayObject>().getDenseInitializedLength())
        return false;

    /*
     * Now watch out for getters and setters along the prototype chain or in
     * other indexed properties on the object. Packed arrays don't have any
     * other indexed properties by definition.
     */
    return IsPackedArray(arr) || !ObjectMayHaveExtraIndexedProperties(arr);
}

static ArrayObject*
CopyDenseArrayElements(JSContext* cx, HandleNativeObject obj, uint32_t begin, uint32_t count)
{
    size_t initlen = obj->getDenseInitializedLength();
    MOZ_ASSERT(initlen <= UINT32_MAX, "initialized length shouldn't exceed UINT32_MAX");
    uint32_t newlength = 0;
    if (initlen > begin)
        newlength = Min<uint32_t>(initlen - begin, count);

    ArrayObject* narr = NewFullyAllocatedArrayTryReuseGroup(cx, obj, newlength);
    if (!narr)
        return nullptr;

    MOZ_ASSERT(count >= narr->length());
    narr->setLength(cx, count);

    if (newlength > 0)
        narr->initDenseElements(obj, begin, newlength);

    return narr;
}

static bool
CopyArrayElements(JSContext* cx, HandleObject obj, uint64_t begin, uint64_t count,
                  HandleArrayObject result)
{
    MOZ_ASSERT(result->length() == count);

    uint64_t startIndex = 0;
    RootedValue value(cx);

    // Use dense storage for new indexed properties where possible.
    {
        uint32_t index = 0;
        uint32_t limit = Min<uint32_t>(count, JSID_INT_MAX);
        for (; index < limit; index++) {
            bool hole;
            if (!CheckForInterrupt(cx) ||
                !HasAndGetElement(cx, obj, begin + index, &hole, &value))
            {
                return false;
            }

            if (!hole) {
                DenseElementResult edResult = result->ensureDenseElements(cx, index, 1);
                if (edResult != DenseElementResult::Success) {
                    if (edResult == DenseElementResult::Failure)
                        return false;

                    MOZ_ASSERT(edResult == DenseElementResult::Incomplete);
                    if (!DefineDataElement(cx, result, index, value))
                        return false;

                    break;
                }
                result->setDenseElementWithType(cx, index, value);
            }
        }
        startIndex = index + 1;
    }

    // Copy any remaining elements.
    for (uint64_t i = startIndex; i < count; i++) {
        bool hole;
        if (!CheckForInterrupt(cx) ||
            !HasAndGetElement(cx, obj, begin + i, &hole, &value))
        {
            return false;
        }

        if (!hole && !DefineArrayElement(cx, result, i, value))
            return false;
    }
    return true;
}

static bool
array_splice_impl(JSContext* cx, unsigned argc, Value* vp, bool returnValueIsUsed)
{
    AutoGeckoProfilerEntry pseudoFrame(cx, "Array.prototype.splice");
    CallArgs args = CallArgsFromVp(argc, vp);

    /* Step 1. */
    RootedObject obj(cx, ToObject(cx, args.thisv()));
    if (!obj)
        return false;

    /* Step 2. */
    uint64_t len;
    if (!GetLengthProperty(cx, obj, &len))
        return false;

    /* Step 3. */
    double relativeStart;
    if (!ToInteger(cx, args.get(0), &relativeStart))
        return false;

    /* Step 4. */
    uint64_t actualStart;
    if (relativeStart < 0)
        actualStart = Max(len + relativeStart, 0.0);
    else
        actualStart = Min(relativeStart, double(len));

    /* Step 5. */
    uint64_t actualDeleteCount;
    if (args.length() == 0) {
        /* Step 5.b. */
        actualDeleteCount = 0;
    } else if (args.length() == 1) {
        /* Step 6.b. */
        actualDeleteCount = len - actualStart;
    } else {
        /* Steps 7.b. */
        double deleteCountDouble;
        if (!ToInteger(cx, args[1], &deleteCountDouble))
            return false;

        /* Step 7.c. */
        actualDeleteCount = Min(Max(deleteCountDouble, 0.0), double(len - actualStart));

        /* Step 8. */
        uint32_t insertCount = args.length() - 2;
        if (len + insertCount - actualDeleteCount >= DOUBLE_INTEGRAL_PRECISION_LIMIT) {
            JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_TOO_LONG_ARRAY);
            return false;
        }
    }

    MOZ_ASSERT(actualStart + actualDeleteCount <= len);

    RootedObject arr(cx);
    if (IsArraySpecies(cx, obj)) {
        if (actualDeleteCount > UINT32_MAX) {
            JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_BAD_ARRAY_LENGTH);
            return false;
        }
        uint32_t count = uint32_t(actualDeleteCount);

        if (CanOptimizeForDenseStorage<ArrayAccess::Read>(obj, actualStart + count, cx)) {
            MOZ_ASSERT(actualStart <= UINT32_MAX,
                       "if actualStart + count <= UINT32_MAX, then actualStart <= UINT32_MAX");
            if (returnValueIsUsed) {
                /* Steps 9-12. */
                arr = CopyDenseArrayElements(cx, obj.as<NativeObject>(), uint32_t(actualStart),
                                             count);
                if (!arr)
                    return false;
            }
        } else {
            /* Step 9. */
            arr = NewFullyAllocatedArrayTryReuseGroup(cx, obj, count);
            if (!arr)
                return false;

            /* Steps 10-11. */
            if (!CopyArrayElements(cx, obj, actualStart, count, arr.as<ArrayObject>()))
                return false;

            /* Step 12 (implicit). */
        }
    } else {
        /* Steps 9. */
        if (!ArraySpeciesCreate(cx, obj, actualDeleteCount, &arr))
            return false;

        /* Steps 10, 11, 11.d. */
        RootedValue fromValue(cx);
        for (uint64_t k = 0; k < actualDeleteCount; k++) {
            /* Step 11.a (implicit). */

            if (!CheckForInterrupt(cx))
                return false;

            /* Steps 11.b, 11.c.i. */
            bool hole;
            if (!HasAndGetElement(cx, obj, actualStart + k, &hole, &fromValue))
                return false;

            /* Step 11.c. */
            if (!hole) {
                /* Step 11.c.ii. */
                if (!DefineArrayElement(cx, arr, k, fromValue))
                    return false;
            }
        }

        /* Step 12. */
        if (!SetLengthProperty(cx, arr, actualDeleteCount))
            return false;
    }

    /* Step 14. */
    uint32_t itemCount = (args.length() >= 2) ? (args.length() - 2) : 0;
    uint64_t finalLength = len - actualDeleteCount + itemCount;

    if (itemCount < actualDeleteCount) {
        /* Step 15: the array is being shrunk. */
        uint64_t sourceIndex = actualStart + actualDeleteCount;
        uint64_t targetIndex = actualStart + itemCount;

        if (CanOptimizeForDenseStorage<ArrayAccess::Write>(obj, len, cx)) {
            MOZ_ASSERT(sourceIndex <= len && targetIndex <= len && len <= UINT32_MAX,
                       "sourceIndex and targetIndex are uint32 array indices");
            MOZ_ASSERT(finalLength < len, "finalLength is strictly less than len");
            MOZ_ASSERT(obj->isNative());

            /* Steps 15.a-b. */
            if (targetIndex != 0 ||
                !obj->as<NativeObject>().tryShiftDenseElements(sourceIndex))
            {
                DenseElementResult result = MoveDenseElements(cx, &obj->as<NativeObject>(),
                                                              uint32_t(targetIndex),
                                                              uint32_t(sourceIndex),
                                                              uint32_t(len - sourceIndex));
                MOZ_ASSERT(result != DenseElementResult::Incomplete);
                if (result == DenseElementResult::Failure)
                    return false;
            }

            /* Steps 15.c-d. */
            SetInitializedLength(cx, obj.as<NativeObject>(), finalLength);
        } else {
            /*
             * This is all very slow if the length is very large. We don't yet
             * have the ability to iterate in sorted order, so we just do the
             * pessimistic thing and let CheckForInterrupt handle the
             * fallout.
             */

            /* Steps 15.a-b. */
            RootedValue fromValue(cx);
            for (uint64_t from = sourceIndex, to = targetIndex; from < len; from++, to++) {
                /* Steps 15.b.i-ii (implicit). */

                if (!CheckForInterrupt(cx))
                    return false;

                /* Steps 15.b.iii, 15.b.iv.1. */
                bool hole;
                if (!HasAndGetElement(cx, obj, from, &hole, &fromValue))
                    return false;

                /* Steps 15.b.iv. */
                if (hole) {
                    /* Steps 15.b.v.1. */
                    if (!DeletePropertyOrThrow(cx, obj, to))
                        return false;
                } else {
                    /* Step 15.b.iv.2. */
                    if (!SetArrayElement(cx, obj, to, fromValue))
                        return false;
                }
            }

            /* Steps 15.c-d. */
            if (!DeletePropertiesOrThrow(cx, obj, len, finalLength))
                return false;
        }
    } else if (itemCount > actualDeleteCount) {
        MOZ_ASSERT(actualDeleteCount <= UINT32_MAX);
        uint32_t deleteCount = uint32_t(actualDeleteCount);

        /* Step 16. */

        /*
         * Optimize only if the array is already dense and we can extend it to
         * its new length.  It would be wrong to extend the elements here for a
         * number of reasons.
         *
         * First, this could cause us to fall into the fast-path below.  This
         * would cause elements to be moved into places past the non-writable
         * length.  And when the dense initialized length is updated, that'll
         * cause the |in| operator to think that those elements actually exist,
         * even though, properly, setting them must fail.
         *
         * Second, extending the elements here will trigger assertions inside
         * ensureDenseElements that the elements aren't being extended past the
         * length of a non-writable array.  This is because extending elements
         * will extend capacity -- which might extend them past a non-writable
         * length, violating the |capacity <= length| invariant for such
         * arrays.  And that would make the various JITted fast-path method
         * implementations of [].push, [].unshift, and so on wrong.
         *
         * If the array length is non-writable, this method *will* throw.  For
         * simplicity, have the slow-path code do it.  (Also note that the slow
         * path may validly *not* throw -- if all the elements being moved are
         * holes.)
         */
        if (obj->is<ArrayObject>() &&
            !ObjectMayHaveExtraIndexedProperties(obj) &&
            len <= UINT32_MAX)
        {
            HandleArrayObject arr = obj.as<ArrayObject>();
            if (arr->lengthIsWritable()) {
                DenseElementResult result =
                    arr->ensureDenseElements(cx, uint32_t(len), itemCount - deleteCount);
                if (result == DenseElementResult::Failure)
                    return false;
            }
        }

        if (CanOptimizeForDenseStorage<ArrayAccess::Write>(obj, finalLength, cx)) {
            MOZ_ASSERT((actualStart + actualDeleteCount) <= len && len <= UINT32_MAX,
                       "start and deleteCount are uint32 array indices");
            MOZ_ASSERT(actualStart + itemCount <= UINT32_MAX,
                       "can't overflow because |len - actualDeleteCount + itemCount <= UINT32_MAX| "
                       "and |actualStart <= len - actualDeleteCount| are both true");
            uint32_t start = uint32_t(actualStart);
            uint32_t length = uint32_t(len);

            DenseElementResult result = MoveDenseElements(cx, &obj->as<NativeObject>(),
                                                          start + itemCount,
                                                          start + deleteCount,
                                                          length - (start + deleteCount));
            MOZ_ASSERT(result != DenseElementResult::Incomplete);
            if (result == DenseElementResult::Failure)
                return false;

            /* Steps 16.a-b. */
            SetInitializedLength(cx, obj.as<NativeObject>(), finalLength);
        } else {
            RootedValue fromValue(cx);
            for (uint64_t k = len - actualDeleteCount; k > actualStart; k--) {
                if (!CheckForInterrupt(cx))
                    return false;

                /* Step 16.b.i. */
                uint64_t from = k + actualDeleteCount - 1;

                /* Step 16.b.ii. */
                uint64_t to = k + itemCount - 1;

                /* Steps 16.b.iii, 16.b.iv.1. */
                bool hole;
                if (!HasAndGetElement(cx, obj, from, &hole, &fromValue))
                    return false;

                /* Steps 16.b.iv. */
                if (hole) {
                    /* Step 16.b.v.1. */
                    if (!DeletePropertyOrThrow(cx, obj, to))
                        return false;
                } else {
                    /* Step 16.b.iv.2. */
                    if (!SetArrayElement(cx, obj, to, fromValue))
                        return false;
                }
            }
        }
    }

    /* Step 13 (reordered). */
    Value* items = args.array() + 2;

    /* Steps 17-18. */
    if (!SetArrayElements(cx, obj, actualStart, itemCount, items))
        return false;

    /* Step 19. */
    if (!SetLengthProperty(cx, obj, finalLength))
        return false;

    /* Step 20. */
    if (returnValueIsUsed)
        args.rval().setObject(*arr);

    return true;
}

/* ES 2016 draft Mar 25, 2016 22.1.3.26. */
bool
js::array_splice(JSContext* cx, unsigned argc, Value* vp)
{
    return array_splice_impl(cx, argc, vp, true);
}

static bool
array_splice_noRetVal(JSContext* cx, unsigned argc, Value* vp)
{
    return array_splice_impl(cx, argc, vp, false);
}

struct SortComparatorIndexes
{
    bool operator()(uint32_t a, uint32_t b, bool* lessOrEqualp) {
        *lessOrEqualp = (a <= b);
        return true;
    }
};

// Returns all indexed properties in the range [begin, end) found on |obj| or
// its proto chain. This function does not handle proxies, objects with
// resolve/lookupProperty hooks or indexed getters, as those can introduce
// new properties. In those cases, *success is set to |false|.
static bool
GetIndexedPropertiesInRange(JSContext* cx, HandleObject obj, uint64_t begin, uint64_t end,
                            Vector<uint32_t>& indexes, bool* success)
{
    *success = false;

    // TODO: Add IdIsIndex with support for large indices.
    if (end > UINT32_MAX)
        return true;
    MOZ_ASSERT(begin <= UINT32_MAX);

    // First, look for proxies or class hooks that can introduce extra
    // properties.
    JSObject* pobj = obj;
    do {
        if (!pobj->isNative() || pobj->getClass()->getResolve() || pobj->getOpsLookupProperty())
            return true;
    } while ((pobj = pobj->staticPrototype()));

    // Collect indexed property names.
    pobj = obj;
    do {
        // Append dense elements.
        NativeObject* nativeObj = &pobj->as<NativeObject>();
        uint32_t initLen = nativeObj->getDenseInitializedLength();
        for (uint32_t i = begin; i < initLen && i < end; i++) {
            if (nativeObj->getDenseElement(i).isMagic(JS_ELEMENTS_HOLE))
                continue;
            if (!indexes.append(i))
                return false;
        }

        // Append typed array elements.
        if (nativeObj->is<TypedArrayObject>()) {
            uint32_t len = nativeObj->as<TypedArrayObject>().length();
            for (uint32_t i = begin; i < len && i < end; i++) {
                if (!indexes.append(i))
                    return false;
            }
        }

        // Append sparse elements.
        if (nativeObj->isIndexed()) {
            Shape::Range<NoGC> r(nativeObj->lastProperty());
            for (; !r.empty(); r.popFront()) {
                Shape& shape = r.front();
                jsid id = shape.propid();
                uint32_t i;
                if (!IdIsIndex(id, &i))
                    continue;

                if (!(begin <= i && i < end))
                    continue;

                // Watch out for getters, they can add new properties.
                if (!shape.hasDefaultGetter())
                    return true;

                if (!indexes.append(i))
                    return false;
            }
        }
    } while ((pobj = pobj->staticPrototype()));

    // Sort the indexes.
    Vector<uint32_t> tmp(cx);
    size_t n = indexes.length();
    if (!tmp.resize(n))
        return false;
    if (!MergeSort(indexes.begin(), n, tmp.begin(), SortComparatorIndexes()))
        return false;

    // Remove duplicates.
    if (!indexes.empty()) {
        uint32_t last = 0;
        for (size_t i = 1, len = indexes.length(); i < len; i++) {
            uint32_t elem = indexes[i];
            if (indexes[last] != elem) {
                last++;
                indexes[last] = elem;
            }
        }
        if (!indexes.resize(last + 1))
            return false;
    }

    *success = true;
    return true;
}

static bool
SliceSparse(JSContext* cx, HandleObject obj, uint64_t begin, uint64_t end,
            HandleArrayObject result)
{
    MOZ_ASSERT(begin <= end);

    Vector<uint32_t> indexes(cx);
    bool success;
    if (!GetIndexedPropertiesInRange(cx, obj, begin, end, indexes, &success))
        return false;

    if (!success)
        return CopyArrayElements(cx, obj, begin, end - begin, result);

    MOZ_ASSERT(end <= UINT32_MAX,
               "indices larger than UINT32_MAX should be rejected by GetIndexedPropertiesInRange");

    RootedValue value(cx);
    for (uint32_t index : indexes) {
        MOZ_ASSERT(begin <= index && index < end);

        bool hole;
        if (!HasAndGetElement(cx, obj, index, &hole, &value))
            return false;

        if (!hole && !DefineDataElement(cx, result, index - uint32_t(begin), value))
            return false;
    }

    return true;
}

static JSObject*
SliceArguments(JSContext* cx, Handle<ArgumentsObject*> argsobj, uint32_t begin, uint32_t count)
{
    MOZ_ASSERT(!argsobj->hasOverriddenLength() && !argsobj->isAnyElementDeleted());
    MOZ_ASSERT(begin + count <= argsobj->initialLength());

    ArrayObject* result = NewDenseFullyAllocatedArray(cx, count);
    if (!result)
        return nullptr;
    result->setDenseInitializedLength(count);

    MOZ_ASSERT(result->group()->unknownProperties(),
               "The default array group has unknown properties, so we can directly initialize the"
               "dense elements without needing to update the indexed type set.");

    for (uint32_t index = 0; index < count; index++) {
        const Value& v = argsobj->element(begin + index);
        result->initDenseElement(index, v);
    }
    return result;
}

template <typename T, typename ArrayLength>
static inline ArrayLength
NormalizeSliceTerm(T value, ArrayLength length)
{
    if (value < 0) {
        value += length;
        if (value < 0)
            return 0;
    } else if (double(value) > double(length)) {
        return length;
    }
    return ArrayLength(value);
}

static bool
ArraySliceOrdinary(JSContext* cx, HandleObject obj, uint64_t begin, uint64_t end,
                   MutableHandleValue rval)
{
    if (begin > end)
        begin = end;

    if ((end - begin) > UINT32_MAX) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_BAD_ARRAY_LENGTH);
        return false;
    }
    uint32_t count = uint32_t(end - begin);

    if (CanOptimizeForDenseStorage<ArrayAccess::Read>(obj, end, cx)) {
        MOZ_ASSERT(begin <= UINT32_MAX, "if end <= UINT32_MAX, then begin <= UINT32_MAX");
        JSObject* narr = CopyDenseArrayElements(cx, obj.as<NativeObject>(), uint32_t(begin),
                                                count);
        if (!narr)
            return false;

        rval.setObject(*narr);
        return true;
    }

    if (obj->is<ArgumentsObject>()) {
        Handle<ArgumentsObject*> argsobj = obj.as<ArgumentsObject>();
        if (!argsobj->hasOverriddenLength() && !argsobj->isAnyElementDeleted()) {
            MOZ_ASSERT(begin <= UINT32_MAX, "begin is limited by |argsobj|'s length");
            JSObject* narr = SliceArguments(cx, argsobj, uint32_t(begin), count);
            if (!narr)
                return false;

            rval.setObject(*narr);
            return true;
        }
    }

    RootedArrayObject narr(cx, NewPartlyAllocatedArrayTryReuseGroup(cx, obj, count));
    if (!narr)
        return false;

    if (end <= UINT32_MAX) {
        if (js::GetElementsOp op = obj->getOpsGetElements()) {
            ElementAdder adder(cx, narr, count, ElementAdder::CheckHasElemPreserveHoles);
            if (!op(cx, obj, uint32_t(begin), uint32_t(end), &adder))
                return false;

            rval.setObject(*narr);
            return true;
        }
    }

    if (obj->isNative() && obj->as<NativeObject>().isIndexed() && count > 1000) {
        if (!SliceSparse(cx, obj, begin, end, narr))
            return false;
    } else {
        if (!CopyArrayElements(cx, obj, begin, count, narr))
            return false;
    }

    rval.setObject(*narr);
    return true;
}

/* ES 2016 draft Mar 25, 2016 22.1.3.23. */
bool
js::array_slice(JSContext* cx, unsigned argc, Value* vp)
{
    AutoGeckoProfilerEntry pseudoFrame(cx, "Array.prototype.slice");
    CallArgs args = CallArgsFromVp(argc, vp);

    /* Step 1. */
    RootedObject obj(cx, ToObject(cx, args.thisv()));
    if (!obj)
        return false;

    /* Step 2. */
    uint64_t length;
    if (!GetLengthProperty(cx, obj, &length))
        return false;

    uint64_t k = 0;
    uint64_t final = length;
    if (args.length() > 0) {
        double d;
        /* Step 3. */
        if (!ToInteger(cx, args[0], &d))
            return false;

        /* Step 4. */
        k = NormalizeSliceTerm(d, length);

        if (args.hasDefined(1)) {
            /* Step 5. */
            if (!ToInteger(cx, args[1], &d))
                return false;

            /* Step 6. */
            final = NormalizeSliceTerm(d, length);
        }
    }

    if (IsArraySpecies(cx, obj)) {
        /* Steps 7-12: Optimized for ordinary array. */
        return ArraySliceOrdinary(cx, obj, k, final, args.rval());
    }

    /* Step 7. */
    uint64_t count = final > k ? final - k : 0;

    /* Step 8. */
    RootedObject arr(cx);
    if (!ArraySpeciesCreate(cx, obj, count, &arr))
        return false;

    /* Step 9. */
    uint64_t n = 0;

    /* Step 10. */
    RootedValue kValue(cx);
    while (k < final) {
        if (!CheckForInterrupt(cx))
            return false;

        /* Steps 10.a-b, and 10.c.i. */
        bool kNotPresent;
        if (!HasAndGetElement(cx, obj, k, &kNotPresent, &kValue))
            return false;

        /* Step 10.c. */
        if (!kNotPresent) {
            /* Steps 10.c.ii. */
            if (!DefineArrayElement(cx, arr, n, kValue))
                return false;
        }
        /* Step 10.d. */
        k++;

        /* Step 10.e. */
        n++;
    }

    /* Step 11. */
    if (!SetLengthProperty(cx, arr, n))
        return false;

    /* Step 12. */
    args.rval().setObject(*arr);
    return true;
}

static bool
ArraySliceDenseKernel(JSContext* cx, ArrayObject* arr, int32_t beginArg, int32_t endArg,
                      ArrayObject* result)
{
    uint32_t length = arr->length();

    uint32_t begin = NormalizeSliceTerm(beginArg, length);
    uint32_t end = NormalizeSliceTerm(endArg, length);

    if (begin > end)
        begin = end;

    uint32_t count = end - begin;
    size_t initlen = arr->getDenseInitializedLength();
    if (initlen > begin) {
        uint32_t newlength = Min<uint32_t>(initlen - begin, count);
        if (newlength > 0) {
            if (!result->ensureElements(cx, newlength))
                return false;
            result->initDenseElements(arr, begin, newlength);
        }
    }

    MOZ_ASSERT(count >= result->length());
    result->setLength(cx, count);

    return true;
}

JSObject*
js::array_slice_dense(JSContext* cx, HandleObject obj, int32_t begin, int32_t end,
                      HandleObject result)
{
    if (result && IsArraySpecies(cx, obj)) {
        if (!ArraySliceDenseKernel(cx, &obj->as<ArrayObject>(), begin, end,
                                   &result->as<ArrayObject>()))
        {
            return nullptr;
        }
        return result;
    }

    // Slower path if the JIT wasn't able to allocate an object inline.
    JS::AutoValueArray<4> argv(cx);
    argv[0].setUndefined();
    argv[1].setObject(*obj);
    argv[2].setInt32(begin);
    argv[3].setInt32(end);
    if (!array_slice(cx, 2, argv.begin()))
        return nullptr;
    return &argv[0].toObject();
}

static bool
array_isArray(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    bool isArray = false;
    if (args.get(0).isObject()) {
        RootedObject obj(cx, &args[0].toObject());
        if (!IsArray(cx, obj, &isArray))
            return false;
    }
    args.rval().setBoolean(isArray);
    return true;
}

static bool
ArrayFromCallArgs(JSContext* cx, CallArgs& args, HandleObject proto = nullptr)
{
    ArrayObject* obj = NewCopiedArrayForCallingAllocationSite(cx, args.array(), args.length(),
                                                              proto);
    if (!obj)
        return false;

    args.rval().setObject(*obj);
    return true;
}

static bool
array_of(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    if (IsArrayConstructor(args.thisv()) || !IsConstructor(args.thisv())) {
        // IsArrayConstructor(this) will usually be true in practice. This is
        // the most common path.
        return ArrayFromCallArgs(cx, args);
    }

    // Step 4.
    RootedObject obj(cx);
    {
        FixedConstructArgs<1> cargs(cx);

        cargs[0].setNumber(args.length());

        if (!Construct(cx, args.thisv(), cargs, args.thisv(), &obj))
            return false;
    }

    // Step 8.
    for (unsigned k = 0; k < args.length(); k++) {
        if (!DefineDataElement(cx, obj, k, args[k]))
            return false;
    }

    // Steps 9-10.
    if (!SetLengthProperty(cx, obj, args.length()))
        return false;

    // Step 11.
    args.rval().setObject(*obj);
    return true;
}

const JSJitInfo js::array_splice_info = {
  { (JSJitGetterOp)array_splice_noRetVal },
  { 0 }, /* unused */
  { 0 }, /* unused */
  JSJitInfo::IgnoresReturnValueNative,
  JSJitInfo::AliasEverything,
  JSVAL_TYPE_UNDEFINED,
};

static const JSFunctionSpec array_methods[] = {
    JS_FN(js_toSource_str,      array_toSource,     0,0),
    JS_SELF_HOSTED_FN(js_toString_str, "ArrayToString",      0,0),
    JS_FN(js_toLocaleString_str,       array_toLocaleString, 0,0),

    /* Perl-ish methods. */
    JS_INLINABLE_FN("join",     array_join,         1,0, ArrayJoin),
    JS_FN("reverse",            array_reverse,      0,0),
    JS_SELF_HOSTED_FN("sort",   "ArraySort",        1,0),
    JS_INLINABLE_FN("push",     array_push,         1,0, ArrayPush),
    JS_INLINABLE_FN("pop",      array_pop,          0,0, ArrayPop),
    JS_INLINABLE_FN("shift",    array_shift,        0,0, ArrayShift),
    JS_FN("unshift",            array_unshift,      1,0),
    JS_FNINFO("splice",         array_splice,       &array_splice_info, 2,0),

    /* Pythonic sequence methods. */
    JS_SELF_HOSTED_FN("concat",      "ArrayConcat",      1,0),
    JS_INLINABLE_FN("slice",    array_slice,        2,0, ArraySlice),

    JS_SELF_HOSTED_FN("lastIndexOf", "ArrayLastIndexOf", 1,0),
    JS_SELF_HOSTED_FN("indexOf",     "ArrayIndexOf",     1,0),
    JS_SELF_HOSTED_FN("forEach",     "ArrayForEach",     1,0),
    JS_SELF_HOSTED_FN("map",         "ArrayMap",         1,0),
    JS_SELF_HOSTED_FN("filter",      "ArrayFilter",      1,0),
    JS_SELF_HOSTED_FN("reduce",      "ArrayReduce",      1,0),
    JS_SELF_HOSTED_FN("reduceRight", "ArrayReduceRight", 1,0),
    JS_SELF_HOSTED_FN("some",        "ArraySome",        1,0),
    JS_SELF_HOSTED_FN("every",       "ArrayEvery",       1,0),

    /* ES6 additions */
    JS_SELF_HOSTED_FN("find",        "ArrayFind",        1,0),
    JS_SELF_HOSTED_FN("findIndex",   "ArrayFindIndex",   1,0),
    JS_SELF_HOSTED_FN("copyWithin",  "ArrayCopyWithin",  3,0),

    JS_SELF_HOSTED_FN("fill",        "ArrayFill",        3,0),

    JS_SELF_HOSTED_SYM_FN(iterator,  "ArrayValues",      0,0),
    JS_SELF_HOSTED_FN("entries",     "ArrayEntries",     0,0),
    JS_SELF_HOSTED_FN("keys",        "ArrayKeys",        0,0),
    JS_SELF_HOSTED_FN("values",      "ArrayValues",      0,0),

    /* ES7 additions */
    JS_SELF_HOSTED_FN("includes",    "ArrayIncludes",    2,0),

#ifdef NIGHTLY_BUILD
    JS_SELF_HOSTED_FN("flatMap",     "ArrayFlatMap",     1,0),
    JS_SELF_HOSTED_FN("flatten",     "ArrayFlatten",     0,0),
#endif

    JS_FS_END
};

static const JSFunctionSpec array_static_methods[] = {
    JS_INLINABLE_FN("isArray",       array_isArray,        1,0, ArrayIsArray),
    JS_SELF_HOSTED_FN("concat",      "ArrayStaticConcat", 2,0),
    JS_SELF_HOSTED_FN("lastIndexOf", "ArrayStaticLastIndexOf", 2,0),
    JS_SELF_HOSTED_FN("indexOf",     "ArrayStaticIndexOf", 2,0),
    JS_SELF_HOSTED_FN("forEach",     "ArrayStaticForEach", 2,0),
    JS_SELF_HOSTED_FN("map",         "ArrayStaticMap",   2,0),
    JS_SELF_HOSTED_FN("filter",      "ArrayStaticFilter", 2,0),
    JS_SELF_HOSTED_FN("every",       "ArrayStaticEvery", 2,0),
    JS_SELF_HOSTED_FN("some",        "ArrayStaticSome",  2,0),
    JS_SELF_HOSTED_FN("reduce",      "ArrayStaticReduce", 2,0),
    JS_SELF_HOSTED_FN("reduceRight", "ArrayStaticReduceRight", 2,0),
    JS_SELF_HOSTED_FN("join",        "ArrayStaticJoin", 2,0),
    JS_SELF_HOSTED_FN("reverse",     "ArrayStaticReverse", 1,0),
    JS_SELF_HOSTED_FN("sort",        "ArrayStaticSort", 2,0),
    JS_SELF_HOSTED_FN("push",        "ArrayStaticPush", 2,0),
    JS_SELF_HOSTED_FN("pop",         "ArrayStaticPop", 1,0),
    JS_SELF_HOSTED_FN("shift",       "ArrayStaticShift", 1,0),
    JS_SELF_HOSTED_FN("unshift",     "ArrayStaticUnshift", 2,0),
    JS_SELF_HOSTED_FN("splice",      "ArrayStaticSplice", 3,0),
    JS_SELF_HOSTED_FN("slice",       "ArrayStaticSlice", 3,0),
    JS_SELF_HOSTED_FN("from",        "ArrayFrom", 3,0),
    JS_FN("of",                 array_of,           0,0),

    JS_FS_END
};

const JSPropertySpec array_static_props[] = {
    JS_SELF_HOSTED_SYM_GET(species, "ArraySpecies", 0),
    JS_PS_END
};

static inline bool
ArrayConstructorImpl(JSContext* cx, CallArgs& args, bool isConstructor)
{
    RootedObject proto(cx);
    if (isConstructor) {
        if (!GetPrototypeFromBuiltinConstructor(cx, args, &proto))
            return false;
    } else {
        // We're emulating |new Array(n)| with |std_Array(n)| in self-hosted JS,
        // and the proto should be %ArrayPrototype% regardless of the callee.
        proto = GlobalObject::getOrCreateArrayPrototype(cx, cx->global());
        if (!proto)
            return false;
    }

    if (args.length() != 1 || !args[0].isNumber())
        return ArrayFromCallArgs(cx, args, proto);

    uint32_t length;
    if (args[0].isInt32()) {
        int32_t i = args[0].toInt32();
        if (i < 0) {
            JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_BAD_ARRAY_LENGTH);
            return false;
        }
        length = uint32_t(i);
    } else {
        double d = args[0].toDouble();
        length = ToUint32(d);
        if (d != double(length)) {
            JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_BAD_ARRAY_LENGTH);
            return false;
        }
    }

    ArrayObject* obj = NewPartlyAllocatedArrayForCallingAllocationSite(cx, length, proto);
    if (!obj)
        return false;

    args.rval().setObject(*obj);
    return true;
}

/* ES5 15.4.2 */
bool
js::ArrayConstructor(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return ArrayConstructorImpl(cx, args, /* isConstructor = */ true);
}

bool
js::array_construct(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    MOZ_ASSERT(!args.isConstructing());
    MOZ_ASSERT(args.length() == 1);
    MOZ_ASSERT(args[0].isNumber());
    return ArrayConstructorImpl(cx, args, /* isConstructor = */ false);
}

ArrayObject*
js::ArrayConstructorOneArg(JSContext* cx, HandleObjectGroup group, int32_t lengthInt)
{
    if (lengthInt < 0) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_BAD_ARRAY_LENGTH);
        return nullptr;
    }

    uint32_t length = uint32_t(lengthInt);
    return NewPartlyAllocatedArrayTryUseGroup(cx, group, length);
}

static JSObject*
CreateArrayPrototype(JSContext* cx, JSProtoKey key)
{
    MOZ_ASSERT(key == JSProto_Array);
    RootedObject proto(cx, GlobalObject::getOrCreateObjectPrototype(cx, cx->global()));
    if (!proto)
        return nullptr;

    RootedObjectGroup group(cx, ObjectGroup::defaultNewGroup(cx, &ArrayObject::class_,
                                                             TaggedProto(proto)));
    if (!group)
        return nullptr;

    RootedShape shape(cx, EmptyShape::getInitialShape(cx, &ArrayObject::class_, TaggedProto(proto),
                                                      gc::AllocKind::OBJECT0));
    if (!shape)
        return nullptr;

    AutoSetNewObjectMetadata metadata(cx);
    RootedArrayObject arrayProto(cx, ArrayObject::createArray(cx, gc::AllocKind::OBJECT4,
                                                              gc::TenuredHeap, shape, group, 0,
                                                              metadata));
    if (!arrayProto ||
        !JSObject::setSingleton(cx, arrayProto) ||
        !JSObject::setDelegate(cx, arrayProto) ||
        !AddLengthProperty(cx, arrayProto))
    {
        return nullptr;
    }

    /*
     * The default 'new' group of Array.prototype is required by type inference
     * to have unknown properties, to simplify handling of e.g. heterogenous
     * arrays in JSON and script literals and allows setDenseArrayElement to
     * be used without updating the indexed type set for such default arrays.
     */
    if (!JSObject::setNewGroupUnknown(cx, &ArrayObject::class_, arrayProto))
        return nullptr;

    return arrayProto;
}

static bool
array_proto_finish(JSContext* cx, JS::HandleObject ctor, JS::HandleObject proto)
{
    // Add Array.prototype[@@unscopables]. ECMA-262 draft (2016 Mar 19) 22.1.3.32.
    RootedObject unscopables(cx, NewObjectWithGivenProto<PlainObject>(cx, nullptr,
                                                                      SingletonObject));
    if (!unscopables)
        return false;

    RootedValue value(cx, BooleanValue(true));
    if (!DefineDataProperty(cx, unscopables, cx->names().copyWithin, value) ||
        !DefineDataProperty(cx, unscopables, cx->names().entries, value) ||
        !DefineDataProperty(cx, unscopables, cx->names().fill, value) ||
        !DefineDataProperty(cx, unscopables, cx->names().find, value) ||
        !DefineDataProperty(cx, unscopables, cx->names().findIndex, value) ||
        !DefineDataProperty(cx, unscopables, cx->names().includes, value) ||
        !DefineDataProperty(cx, unscopables, cx->names().keys, value) ||
        !DefineDataProperty(cx, unscopables, cx->names().values, value))
    {
        return false;
    }

    RootedId id(cx, SYMBOL_TO_JSID(cx->wellKnownSymbols().get(JS::SymbolCode::unscopables)));
    value.setObject(*unscopables);
    return DefineDataProperty(cx, proto, id, value, JSPROP_READONLY);
}

static const ClassOps ArrayObjectClassOps = {
    array_addProperty,
    nullptr, /* delProperty */
    nullptr, /* enumerate */
    nullptr, /* resolve */
    nullptr, /* mayResolve */
    nullptr, /* finalize */
    nullptr, /* call */
    nullptr, /* hasInstance */
    nullptr, /* construct */
    nullptr, /* trace */
};

static const ClassSpec ArrayObjectClassSpec = {
    GenericCreateConstructor<ArrayConstructor, 1, AllocKind::FUNCTION, &jit::JitInfo_Array>,
    CreateArrayPrototype,
    array_static_methods,
    array_static_props,
    array_methods,
    nullptr,
    array_proto_finish
};

const Class ArrayObject::class_ = {
    "Array",
    JSCLASS_HAS_CACHED_PROTO(JSProto_Array) | JSCLASS_DELAY_METADATA_BUILDER,
    &ArrayObjectClassOps,
    &ArrayObjectClassSpec
};

/*
 * Array allocation functions.
 */

static inline bool
EnsureNewArrayElements(JSContext* cx, ArrayObject* obj, uint32_t length)
{
    /*
     * If ensureElements creates dynamically allocated slots, then having
     * fixedSlots is a waste.
     */
    DebugOnly<uint32_t> cap = obj->getDenseCapacity();

    if (!obj->ensureElements(cx, length))
        return false;

    MOZ_ASSERT_IF(cap, !obj->hasDynamicElements());

    return true;
}

template <uint32_t maxLength>
static MOZ_ALWAYS_INLINE ArrayObject*
NewArray(JSContext* cx, uint32_t length,
         HandleObject protoArg, NewObjectKind newKind = GenericObject)
{
    gc::AllocKind allocKind = GuessArrayGCKind(length);
    MOZ_ASSERT(CanBeFinalizedInBackground(allocKind, &ArrayObject::class_));
    allocKind = GetBackgroundAllocKind(allocKind);

    RootedObject proto(cx, protoArg);
    if (!proto) {
        proto = GlobalObject::getOrCreateArrayPrototype(cx, cx->global());
        if (!proto)
            return nullptr;
    }

    Rooted<TaggedProto> taggedProto(cx, TaggedProto(proto));
    bool isCachable = NewObjectWithTaggedProtoIsCachable(cx, taggedProto, newKind, &ArrayObject::class_);
    if (isCachable) {
        NewObjectCache& cache = cx->caches().newObjectCache;
        NewObjectCache::EntryIndex entry = -1;
        if (cache.lookupProto(&ArrayObject::class_, proto, allocKind, &entry)) {
            gc::InitialHeap heap = GetInitialHeap(newKind, &ArrayObject::class_);
            AutoSetNewObjectMetadata metadata(cx);
            JSObject* obj = cache.newObjectFromHit(cx, entry, heap);
            if (obj) {
                /* Fixup the elements pointer and length, which may be incorrect. */
                ArrayObject* arr = &obj->as<ArrayObject>();
                arr->setFixedElements();
                arr->setLength(cx, length);
                if (maxLength > 0 &&
                    !EnsureNewArrayElements(cx, arr, std::min(maxLength, length)))
                {
                    return nullptr;
                }
                return arr;
            }
        }
    }

    RootedObjectGroup group(cx, ObjectGroup::defaultNewGroup(cx, &ArrayObject::class_,
                                                             TaggedProto(proto)));
    if (!group)
        return nullptr;

    /*
     * Get a shape with zero fixed slots, regardless of the size class.
     * See JSObject::createArray.
     */
    RootedShape shape(cx, EmptyShape::getInitialShape(cx, &ArrayObject::class_,
                                                      TaggedProto(proto),
                                                      gc::AllocKind::OBJECT0));
    if (!shape)
        return nullptr;

    AutoSetNewObjectMetadata metadata(cx);
    RootedArrayObject arr(cx, ArrayObject::createArray(cx, allocKind,
                                                       GetInitialHeap(newKind, &ArrayObject::class_),
                                                       shape, group, length, metadata));
    if (!arr)
        return nullptr;

    if (shape->isEmptyShape()) {
        if (!AddLengthProperty(cx, arr))
            return nullptr;
        shape = arr->lastProperty();
        EmptyShape::insertInitialShape(cx, shape, proto);
    }

    if (newKind == SingletonObject && !JSObject::setSingleton(cx, arr))
        return nullptr;

    if (isCachable) {
        NewObjectCache& cache = cx->caches().newObjectCache;
        NewObjectCache::EntryIndex entry = -1;
        cache.lookupProto(&ArrayObject::class_, proto, allocKind, &entry);
        cache.fillProto(entry, &ArrayObject::class_, taggedProto, allocKind, arr);
    }

    if (maxLength > 0 && !EnsureNewArrayElements(cx, arr, std::min(maxLength, length)))
        return nullptr;

    probes::CreateObject(cx, arr);
    return arr;
}

ArrayObject * JS_FASTCALL
js::NewDenseEmptyArray(JSContext* cx, HandleObject proto /* = nullptr */,
                       NewObjectKind newKind /* = GenericObject */)
{
    return NewArray<0>(cx, 0, proto, newKind);
}

ArrayObject * JS_FASTCALL
js::NewDenseFullyAllocatedArray(JSContext* cx, uint32_t length,
                                HandleObject proto /* = nullptr */,
                                NewObjectKind newKind /* = GenericObject */)
{
    return NewArray<UINT32_MAX>(cx, length, proto, newKind);
}

ArrayObject * JS_FASTCALL
js::NewDensePartlyAllocatedArray(JSContext* cx, uint32_t length,
                                 HandleObject proto /* = nullptr */,
                                 NewObjectKind newKind /* = GenericObject */)
{
    return NewArray<ArrayObject::EagerAllocationMaxLength>(cx, length, proto, newKind);
}

ArrayObject * JS_FASTCALL
js::NewDenseUnallocatedArray(JSContext* cx, uint32_t length,
                             HandleObject proto /* = nullptr */,
                             NewObjectKind newKind /* = GenericObject */)
{
    return NewArray<0>(cx, length, proto, newKind);
}

// values must point at already-rooted Value objects
ArrayObject*
js::NewDenseCopiedArray(JSContext* cx, uint32_t length, const Value* values,
                        HandleObject proto /* = nullptr */,
                        NewObjectKind newKind /* = GenericObject */)
{
    ArrayObject* arr = NewArray<UINT32_MAX>(cx, length, proto, newKind);
    if (!arr)
        return nullptr;

    MOZ_ASSERT(arr->getDenseCapacity() >= length);
    MOZ_ASSERT(arr->getDenseInitializedLength() == 0);

    if (values)
        arr->initDenseElements(values, length);

    return arr;
}

ArrayObject*
js::NewDenseFullyAllocatedArrayWithTemplate(JSContext* cx, uint32_t length, JSObject* templateObject)
{
    AutoSetNewObjectMetadata metadata(cx);
    gc::AllocKind allocKind = GuessArrayGCKind(length);
    MOZ_ASSERT(CanBeFinalizedInBackground(allocKind, &ArrayObject::class_));
    allocKind = GetBackgroundAllocKind(allocKind);

    RootedObjectGroup group(cx, templateObject->group());
    RootedShape shape(cx, templateObject->as<ArrayObject>().lastProperty());

    gc::InitialHeap heap = GetInitialHeap(GenericObject, &ArrayObject::class_);
    Rooted<ArrayObject*> arr(cx, ArrayObject::createArray(cx, allocKind,
                                                          heap, shape, group, length, metadata));
    if (!arr)
        return nullptr;

    if (!EnsureNewArrayElements(cx, arr, length))
        return nullptr;

    probes::CreateObject(cx, arr);

    return arr;
}

ArrayObject*
js::NewDenseCopyOnWriteArray(JSContext* cx, HandleArrayObject templateObject, gc::InitialHeap heap)
{
    MOZ_ASSERT(!gc::IsInsideNursery(templateObject));

    ArrayObject* arr = ArrayObject::createCopyOnWriteArray(cx, heap, templateObject);
    if (!arr)
        return nullptr;

    probes::CreateObject(cx, arr);
    return arr;
}

// Return a new array with the specified length and allocated capacity (up to
// maxLength), using the specified group if possible. If the specified group
// cannot be used, ensure that the created array at least has the given
// [[Prototype]].
template <uint32_t maxLength>
static inline ArrayObject*
NewArrayTryUseGroup(JSContext* cx, HandleObjectGroup group, size_t length,
                    NewObjectKind newKind = GenericObject)
{
    MOZ_ASSERT(newKind != SingletonObject);

    if (group->shouldPreTenure())
        newKind = TenuredObject;

    RootedObject proto(cx, group->proto().toObject());
    ArrayObject* res = NewArray<maxLength>(cx, length, proto, newKind);
    if (!res)
        return nullptr;

    res->setGroup(group);

    // If the length calculation overflowed, make sure that is marked for the
    // new group.
    if (res->length() > INT32_MAX)
        res->setLength(cx, res->length());

    return res;
}

ArrayObject*
js::NewFullyAllocatedArrayTryUseGroup(JSContext* cx, HandleObjectGroup group, size_t length,
                                      NewObjectKind newKind)
{
    return NewArrayTryUseGroup<UINT32_MAX>(cx, group, length, newKind);
}

ArrayObject*
js::NewPartlyAllocatedArrayTryUseGroup(JSContext* cx, HandleObjectGroup group, size_t length)
{
    return NewArrayTryUseGroup<ArrayObject::EagerAllocationMaxLength>(cx, group, length);
}

// Return a new array with the default prototype and specified allocated
// capacity and length. If possible, try to reuse the group of the input
// object. The resulting array will either reuse the input object's group or
// will have unknown property types.
template <uint32_t maxLength>
static inline ArrayObject*
NewArrayTryReuseGroup(JSContext* cx, HandleObject obj, size_t length,
                      NewObjectKind newKind = GenericObject)
{
    if (!obj->is<ArrayObject>())
        return NewArray<maxLength>(cx, length, nullptr, newKind);

    if (obj->staticPrototype() != cx->global()->maybeGetArrayPrototype())
        return NewArray<maxLength>(cx, length, nullptr, newKind);

    RootedObjectGroup group(cx, JSObject::getGroup(cx, obj));
    if (!group)
        return nullptr;

    return NewArrayTryUseGroup<maxLength>(cx, group, length, newKind);
}

ArrayObject*
js::NewFullyAllocatedArrayTryReuseGroup(JSContext* cx, HandleObject obj, size_t length,
                                        NewObjectKind newKind)
{
    return NewArrayTryReuseGroup<UINT32_MAX>(cx, obj, length, newKind);
}

ArrayObject*
js::NewPartlyAllocatedArrayTryReuseGroup(JSContext* cx, HandleObject obj, size_t length)
{
    return NewArrayTryReuseGroup<ArrayObject::EagerAllocationMaxLength>(cx, obj, length);
}

ArrayObject*
js::NewFullyAllocatedArrayForCallingAllocationSite(JSContext* cx, size_t length,
                                                   NewObjectKind newKind)
{
    RootedObjectGroup group(cx, ObjectGroup::callingAllocationSiteGroup(cx, JSProto_Array));
    if (!group)
        return nullptr;
    return NewArrayTryUseGroup<UINT32_MAX>(cx, group, length, newKind);
}

ArrayObject*
js::NewPartlyAllocatedArrayForCallingAllocationSite(JSContext* cx, size_t length, HandleObject proto)
{
    RootedObjectGroup group(cx, ObjectGroup::callingAllocationSiteGroup(cx, JSProto_Array, proto));
    if (!group)
        return nullptr;
    return NewArrayTryUseGroup<ArrayObject::EagerAllocationMaxLength>(cx, group, length);
}

ArrayObject*
js::NewCopiedArrayTryUseGroup(JSContext* cx, HandleObjectGroup group,
                              const Value* vp, size_t length, NewObjectKind newKind,
                              ShouldUpdateTypes updateTypes)
{
    ArrayObject* obj = NewFullyAllocatedArrayTryUseGroup(cx, group, length, newKind);
    if (!obj)
        return nullptr;

    DenseElementResult result = obj->setOrExtendDenseElements(cx, 0, vp, length, updateTypes);
    if (result == DenseElementResult::Failure)
        return nullptr;

    MOZ_ASSERT(result == DenseElementResult::Success);
    return obj;
}

ArrayObject*
js::NewCopiedArrayForCallingAllocationSite(JSContext* cx, const Value* vp, size_t length,
                                           HandleObject proto /* = nullptr */)
{
    RootedObjectGroup group(cx, ObjectGroup::callingAllocationSiteGroup(cx, JSProto_Array, proto));
    if (!group)
        return nullptr;
    return NewCopiedArrayTryUseGroup(cx, group, vp, length);
}

#ifdef DEBUG
bool
js::ArrayInfo(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    RootedObject obj(cx);

    for (unsigned i = 0; i < args.length(); i++) {
        HandleValue arg = args[i];

        UniqueChars bytes = DecompileValueGenerator(cx, JSDVG_SEARCH_STACK, arg, nullptr);
        if (!bytes)
            return false;
        if (arg.isPrimitive() ||
            !(obj = arg.toObjectOrNull())->is<ArrayObject>()) {
            fprintf(stderr, "%s: not array\n", bytes.get());
            continue;
        }
        fprintf(stderr, "%s: (len %u", bytes.get(), obj->as<ArrayObject>().length());
        fprintf(stderr, ", capacity %u", obj->as<ArrayObject>().getDenseCapacity());
        fputs(")\n", stderr);
    }

    args.rval().setUndefined();
    return true;
}
#endif

void
js::ArraySpeciesLookup::initialize(JSContext* cx)
{
    MOZ_ASSERT(state_ == State::Uninitialized);

    // Get the canonical Array.prototype.
    NativeObject* arrayProto = cx->global()->maybeGetArrayPrototype();

    // Leave the cache uninitialized if the Array class itself is not yet
    // initialized.
    if (!arrayProto)
        return;

    // Get the canonical Array constructor.
    const Value& arrayCtorValue = cx->global()->getConstructor(JSProto_Array);
    MOZ_ASSERT(arrayCtorValue.isObject(),
               "The Array constructor is initialized iff Array.prototype is initialized");
    JSFunction* arrayCtor = &arrayCtorValue.toObject().as<JSFunction>();

    // Shortcut returns below means Array[@@species] will never be
    // optimizable, set to disabled now, and clear it later when we succeed.
    state_ = State::Disabled;

    // Look up Array.prototype[@@iterator] and ensure it's a data property.
    Shape* ctorShape = arrayProto->lookup(cx, NameToId(cx->names().constructor));
    if (!ctorShape || !ctorShape->isDataProperty())
        return;

    // Get the referred value, and ensure it holds the canonical Array
    // constructor.
    JSFunction* ctorFun;
    if (!IsFunctionObject(arrayProto->getSlot(ctorShape->slot()), &ctorFun))
        return;
    if (ctorFun != arrayCtor)
        return;

    // Look up the '@@species' value on Array
    Shape* speciesShape = arrayCtor->lookup(cx, SYMBOL_TO_JSID(cx->wellKnownSymbols().species));
    if (!speciesShape || !speciesShape->hasGetterValue())
        return;

    // Get the referred value, ensure it holds the canonical Array[@@species]
    // function.
    JSFunction* speciesFun;
    if (!IsFunctionObject(speciesShape->getterValue(), &speciesFun))
        return;
    if (!IsSelfHostedFunctionWithName(speciesFun, cx->names().ArraySpecies))
        return;

    // Store raw pointers below. This is okay to do here, because all objects
    // are in the tenured heap.
    MOZ_ASSERT(!IsInsideNursery(arrayProto));
    MOZ_ASSERT(!IsInsideNursery(arrayCtor));
    MOZ_ASSERT(!IsInsideNursery(arrayCtor->lastProperty()));
    MOZ_ASSERT(!IsInsideNursery(speciesShape));
    MOZ_ASSERT(!IsInsideNursery(speciesFun));
    MOZ_ASSERT(!IsInsideNursery(arrayProto->lastProperty()));

    state_ = State::Initialized;
    arrayProto_ = arrayProto;
    arrayConstructor_ = arrayCtor;
    arrayConstructorShape_ = arrayCtor->lastProperty();
#ifdef DEBUG
    arraySpeciesShape_ = speciesShape;
    canonicalSpeciesFunc_ = speciesFun;
#endif
    arrayProtoShape_ = arrayProto->lastProperty();
    arrayProtoConstructorSlot_ = ctorShape->slot();
}

void
js::ArraySpeciesLookup::reset()
{
    state_ = State::Uninitialized;
    arrayProto_ = nullptr;
    arrayConstructor_ = nullptr;
    arrayConstructorShape_ = nullptr;
#ifdef DEBUG
    arraySpeciesShape_ = nullptr;
    canonicalSpeciesFunc_ = nullptr;
#endif
    arrayProtoShape_ = nullptr;
    arrayProtoConstructorSlot_ = -1;
}

bool
js::ArraySpeciesLookup::isArrayStateStillSane()
{
    MOZ_ASSERT(state_ == State::Initialized);

    // Ensure that Array.prototype still has the expected shape.
    if (arrayProto_->lastProperty() != arrayProtoShape_)
        return false;

    // Ensure that Array.prototype.constructor contains the canonical Array
    // constructor function.
    if (arrayProto_->getSlot(arrayProtoConstructorSlot_) != ObjectValue(*arrayConstructor_))
        return false;

    // Ensure that Array still has the expected shape.
    if (arrayConstructor_->lastProperty() != arrayConstructorShape_)
        return false;

    // Ensure the species getter contains the canonical @@species function.
    // Note: This is currently guaranteed to be always true, because modifying
    // the getter property implies a new shape is generated. If this ever
    // changes, convert this assertion into an if-statement.
    MOZ_ASSERT(arraySpeciesShape_->getterObject() == canonicalSpeciesFunc_);

    return true;
}

bool
js::ArraySpeciesLookup::tryOptimizeArray(JSContext* cx, ArrayObject* array)
{
    if (state_ == State::Uninitialized) {
        // If the cache is not initialized, initialize it.
        initialize(cx);
    } else if (state_ == State::Initialized && !isArrayStateStillSane()) {
        // Otherwise, if the array state is no longer sane, reinitialize.
        reset();
        initialize(cx);
    }

    // If the cache is disabled or still uninitialized, don't bother trying to
    // optimize.
    if (state_ != State::Initialized)
        return false;

    // By the time we get here, we should have a sane array state.
    MOZ_ASSERT(isArrayStateStillSane());

    // Ensure |array|'s prototype is the actual Array.prototype.
    if (array->staticPrototype() != arrayProto_)
        return false;

    // Ensure |array| doesn't define any own properties besides its
    // non-deletable "length" property. This serves as a quick check to make
    // sure |array| doesn't define an own "constructor" property which may
    // shadow Array.prototype.constructor.
    Shape* shape = array->shape();
    if (shape->previous() && !shape->previous()->isEmptyShape())
        return false;

    MOZ_ASSERT(JSID_IS_ATOM(shape->propidRaw(), cx->names().length));
    return true;
}

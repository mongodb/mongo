/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jsarray.h"

#include "mozilla/ArrayUtils.h"
#include "mozilla/CheckedInt.h"
#include "mozilla/DebugOnly.h"
#include "mozilla/FloatingPoint.h"
#include "mozilla/MathAlgorithms.h"

#include <algorithm>

#include "jsapi.h"
#include "jsatom.h"
#include "jscntxt.h"
#include "jsfriendapi.h"
#include "jsfun.h"
#include "jsiter.h"
#include "jsnum.h"
#include "jsobj.h"
#include "jstypes.h"
#include "jsutil.h"

#include "ds/Sort.h"
#include "gc/Heap.h"
#include "jit/InlinableNatives.h"
#include "js/Class.h"
#include "js/Conversions.h"
#include "vm/ArgumentsObject.h"
#include "vm/Interpreter.h"
#include "vm/Shape.h"
#include "vm/StringBuffer.h"
#include "vm/TypedArrayCommon.h"

#include "jsatominlines.h"

#include "vm/ArgumentsObject-inl.h"
#include "vm/ArrayObject-inl.h"
#include "vm/Interpreter-inl.h"
#include "vm/NativeObject-inl.h"
#include "vm/Runtime-inl.h"
#include "vm/UnboxedObject-inl.h"

using namespace js;
using namespace js::gc;

using mozilla::Abs;
using mozilla::ArrayLength;
using mozilla::CeilingLog2;
using mozilla::CheckedInt;
using mozilla::DebugOnly;
using mozilla::IsNaN;
using mozilla::UniquePtr;

using JS::AutoCheckCannotGC;
using JS::IsArrayAnswer;
using JS::ToUint32;

bool
JS::IsArray(JSContext* cx, HandleObject obj, IsArrayAnswer* answer)
{
    if (obj->is<ArrayObject>() || obj->is<UnboxedArrayObject>()) {
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
        JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_PROXY_REVOKED);
        return false;
    }

    *isArray = answer == IsArrayAnswer::Array;
    return true;
}

bool
js::GetLengthProperty(JSContext* cx, HandleObject obj, uint32_t* lengthp)
{
    if (obj->is<ArrayObject>()) {
        *lengthp = obj->as<ArrayObject>().length();
        return true;
    }

    if (obj->is<UnboxedArrayObject>()) {
        *lengthp = obj->as<UnboxedArrayObject>().length();
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

    bool overflow;
    if (!ToLengthClamped(cx, value, lengthp, &overflow)) {
        if (!overflow)
            return false;
        *lengthp = UINT32_MAX;
    }
    return true;
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

static bool
ToId(JSContext* cx, double index, MutableHandleId id)
{
    if (index == uint32_t(index))
        return IndexToId(cx, uint32_t(index), id);

    Value tmp = DoubleValue(index);
    return ValueToId<CanGC>(cx, HandleValue::fromMarkedLocation(&tmp), id);
}

static bool
ToId(JSContext* cx, uint32_t index, MutableHandleId id)
{
    return IndexToId(cx, index, id);
}

/*
 * If the property at the given index exists, get its value into location
 * pointed by vp and set *hole to false. Otherwise set *hole to true and *vp
 * to JSVAL_VOID. This function assumes that the location pointed by vp is
 * properly rooted and can be used as GC-protected storage for temporaries.
 */
template <typename IndexType>
static inline bool
DoGetElement(JSContext* cx, HandleObject obj, HandleObject receiver,
             IndexType index, bool* hole, MutableHandleValue vp)
{
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

template <typename IndexType>
static void
AssertGreaterThanZero(IndexType index)
{
    MOZ_ASSERT(index >= 0);
    MOZ_ASSERT(index == floor(index));
}

template<>
void
AssertGreaterThanZero(uint32_t index)
{
}

static bool
GetElement(JSContext* cx, HandleObject obj, HandleObject receiver,
           uint32_t index, bool* hole, MutableHandleValue vp)
{
    AssertGreaterThanZero(index);
    if (index < GetAnyBoxedOrUnboxedInitializedLength(obj)) {
        vp.set(GetAnyBoxedOrUnboxedDenseElement(obj, uint32_t(index)));
        if (!vp.isMagic(JS_ELEMENTS_HOLE)) {
            *hole = false;
            return true;
        }
    }
    if (obj->is<ArgumentsObject>()) {
        if (obj->as<ArgumentsObject>().maybeGetElement(uint32_t(index), vp)) {
            *hole = false;
            return true;
        }
    }

    return DoGetElement(cx, obj, receiver, index, hole, vp);
}

template <typename IndexType>
static inline bool
GetElement(JSContext* cx, HandleObject obj, IndexType index, bool* hole, MutableHandleValue vp)
{
    return GetElement(cx, obj, obj, index, hole, vp);
}

bool
ElementAdder::append(JSContext* cx, HandleValue v)
{
    MOZ_ASSERT(index_ < length_);
    if (resObj_) {
        DenseElementResult result =
            SetOrExtendAnyBoxedOrUnboxedDenseElements(cx, resObj_, index_, v.address(), 1);
        if (result == DenseElementResult::Failure)
            return false;
        if (result == DenseElementResult::Incomplete) {
            if (!DefineElement(cx, resObj_, index_, v))
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
            if (!GetElement(cx, obj, receiver, i, &hole, &val))
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

template <JSValueType Type>
DenseElementResult
GetBoxedOrUnboxedDenseElements(JSObject* aobj, uint32_t length, Value* vp)
{
    MOZ_ASSERT(!ObjectMayHaveExtraIndexedProperties(aobj));

    if (length > GetBoxedOrUnboxedInitializedLength<Type>(aobj))
        return DenseElementResult::Incomplete;

    for (size_t i = 0; i < length; i++) {
        vp[i] = GetBoxedOrUnboxedDenseElement<Type>(aobj, i);

        // No other indexed properties so hole => undefined.
        if (vp[i].isMagic(JS_ELEMENTS_HOLE))
            vp[i] = UndefinedValue();
    }

    return DenseElementResult::Success;
}

DefineBoxedOrUnboxedFunctor3(GetBoxedOrUnboxedDenseElements,
                             JSObject*, uint32_t, Value*);

bool
js::GetElements(JSContext* cx, HandleObject aobj, uint32_t length, Value* vp)
{
    if (!ObjectMayHaveExtraIndexedProperties(aobj)) {
        GetBoxedOrUnboxedDenseElementsFunctor functor(aobj, length, vp);
        DenseElementResult result = CallBoxedOrUnboxedSpecialization(functor, aobj);
        if (result != DenseElementResult::Incomplete)
            return result == DenseElementResult::Success;
    }

    if (aobj->is<ArgumentsObject>()) {
        ArgumentsObject& argsobj = aobj->as<ArgumentsObject>();
        if (!argsobj.hasOverriddenLength()) {
            if (argsobj.maybeGetElements(0, length, vp))
                return true;
        }
    }

    if (js::GetElementsOp op = aobj->getOps()->getElements) {
        ElementAdder adder(cx, vp, length, ElementAdder::GetElement);
        return op(cx, aobj, 0, length, &adder);
    }

    for (uint32_t i = 0; i < length; i++) {
        if (!GetElement(cx, aobj, aobj, i, MutableHandleValue::fromMarkedLocation(&vp[i])))
            return false;
    }

    return true;
}

// Set the value of the property at the given index to v. The behavior of this
// function is currently incoherent and it sometimes defines the property
// instead. See bug 1163091.
static bool
SetArrayElement(JSContext* cx, HandleObject obj, double index, HandleValue v)
{
    MOZ_ASSERT(index >= 0);

    if ((obj->is<ArrayObject>() || obj->is<UnboxedArrayObject>()) && !obj->isIndexed() && index <= UINT32_MAX) {
        DenseElementResult result =
            SetOrExtendAnyBoxedOrUnboxedDenseElements(cx, obj, uint32_t(index), v.address(), 1);
        if (result != DenseElementResult::Incomplete)
            return result == DenseElementResult::Success;
    }

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
DeleteArrayElement(JSContext* cx, HandleObject obj, double index, ObjectOpResult& result)
{
    MOZ_ASSERT(index >= 0);
    MOZ_ASSERT(floor(index) == index);

    if (obj->is<ArrayObject>() && !obj->isIndexed()) {
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
DeletePropertyOrThrow(JSContext* cx, HandleObject obj, double index)
{
    ObjectOpResult success;
    if (!DeleteArrayElement(cx, obj, index, success))
        return false;
    if (!success) {
        RootedId id(cx);
        RootedValue indexv(cx, NumberValue(index));
        if (!ValueToId<CanGC>(cx, indexv, &id))
            return false;
        return success.reportError(cx, obj, id);
    }
    return true;
}

bool
js::SetLengthProperty(JSContext* cx, HandleObject obj, double length)
{
    RootedValue v(cx, NumberValue(length));
    return SetProperty(cx, obj, cx->names().length, v);
}

static bool
array_length_getter(JSContext* cx, HandleObject obj, HandleId id, MutableHandleValue vp)
{
    vp.setNumber(obj->as<ArrayObject>().length());
    return true;
}

static bool
array_length_setter(JSContext* cx, HandleObject obj, HandleId id, MutableHandleValue vp,
                    ObjectOpResult& result)
{
    if (!obj->is<ArrayObject>()) {
        // This array .length property was found on the prototype
        // chain. Ideally the setter should not have been called, but since
        // we're here, do an impression of SetPropertyByDefining.
        const Class* clasp = obj->getClass();
        return DefineProperty(cx, obj, cx->names().length, vp,
                              clasp->getProperty, clasp->setProperty, JSPROP_ENUMERATE, result);
    }

    Rooted<ArrayObject*> arr(cx, &obj->as<ArrayObject>());
    MOZ_ASSERT(arr->lengthIsWritable(),
               "setter shouldn't be called if property is non-writable");

    return ArraySetLength(cx, arr, id, JSPROP_PERMANENT, vp, result);
}

struct ReverseIndexComparator
{
    bool operator()(const uint32_t& a, const uint32_t& b, bool* lessOrEqualp) {
        MOZ_ASSERT(a != b, "how'd we get duplicate indexes?");
        *lessOrEqualp = b <= a;
        return true;
    }
};

bool
js::CanonicalizeArrayLengthValue(JSContext* cx, HandleValue v, uint32_t* newLen)
{
    double d;

    if (!ToUint32(cx, v, newLen))
        return false;

    if (!ToNumber(cx, v, &d))
        return false;

    if (d == *newLen)
        return true;

    JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_BAD_ARRAY_LENGTH);
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
        // The spec has us calling OrdinaryDefineOwnProperty if
        // Desc.[[Value]] is absent, but our implementation is so different that
        // this is impossible. Instead, set newLen to the current length and
        // proceed to step 9.
        newLen = arr->length();
    } else {
        // Step 2 is irrelevant in our implementation.

        // Steps 3-7.
        MOZ_ASSERT_IF(attrs & JSPROP_IGNORE_VALUE, value.isUndefined());
        if (!CanonicalizeArrayLengthValue(cx, value, &newLen))
            return false;

        // Step 8 is irrelevant in our implementation.
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
        ObjectGroup* arrGroup = arr->getGroup(cx);
        if (!arr->isIndexed() &&
            !MOZ_UNLIKELY(!arrGroup || arrGroup->hasAllFlags(OBJECT_FLAG_ITERATED)))
        {
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

    if (attrs & JSPROP_READONLY) {
        header->setNonwritableArrayLength();

        // When an array's length becomes non-writable, writes to indexes
        // greater than or equal to the length don't change the array.  We
        // handle this with a check for non-writable length in most places.
        // But in JIT code every check counts -- so we piggyback the check on
        // the already-required range check for |index < capacity| by making
        // capacity of arrays with non-writable length never exceed the length.
        if (arr->getDenseCapacity() > newLen) {
            arr->shrinkElements(cx, newLen);
            arr->getElementsHeader()->capacity = newLen;
        }
    }

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
    Rooted<ArrayObject*> arr(cx, &obj->as<ArrayObject>());

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
    return (!obj->isNative() && !obj->is<UnboxedArrayObject>()) ||
           obj->isIndexed() ||
           IsAnyTypedArray(obj) ||
           ClassMayResolveId(*obj->runtimeFromAnyThread()->commonNames,
                             obj->getClass(), INT_TO_JSID(0), obj);
}

bool
js::ObjectMayHaveExtraIndexedProperties(JSObject* obj)
{
    /*
     * Whether obj may have indexed properties anywhere besides its dense
     * elements. This includes other indexed properties in its shape hierarchy,
     * and indexed properties or elements along its prototype chain.
     */

    if (ObjectMayHaveExtraIndexedOwnProperties(obj))
        return true;

    while ((obj = obj->getProto()) != nullptr) {
        if (ObjectMayHaveExtraIndexedOwnProperties(obj))
            return true;
        if (GetAnyBoxedOrUnboxedInitializedLength(obj) != 0)
            return true;
    }

    return false;
}

static bool
AddLengthProperty(ExclusiveContext* cx, HandleArrayObject obj)
{
    /*
     * Add the 'length' property for a newly created array,
     * and update the elements to be an empty array owned by the object.
     * The shared emptyObjectElements singleton cannot be used for slow arrays,
     * as accesses to 'length' will use the elements header.
     */

    RootedId lengthId(cx, NameToId(cx->names().length));
    MOZ_ASSERT(!obj->lookup(cx, lengthId));

    return NativeObject::addProperty(cx, obj, lengthId, array_length_getter, array_length_setter,
                                     SHAPE_INVALID_SLOT,
                                     JSPROP_PERMANENT | JSPROP_SHARED | JSPROP_SHADOWABLE,
                                     0, /* allowDictionary = */ false);
}

#if JS_HAS_TOSOURCE

static bool
array_toSource(JSContext* cx, unsigned argc, Value* vp)
{
    JS_CHECK_RECURSION(cx, return false);
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

    uint32_t length;
    if (!GetLengthProperty(cx, obj, &length))
        return false;

    for (uint32_t index = 0; index < length; index++) {
        bool hole;
        if (!CheckForInterrupt(cx) ||
            !GetElement(cx, obj, index, &hole, &elt)) {
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

#endif

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

template <typename SeparatorOp, JSValueType Type>
static DenseElementResult
ArrayJoinDenseKernel(JSContext* cx, SeparatorOp sepOp, HandleObject obj, uint32_t length,
                     StringBuffer& sb, uint32_t* numProcessed)
{
    // This loop handles all elements up to initializedLength. If
    // length > initLength we rely on the second loop to add the
    // other elements.
    MOZ_ASSERT(*numProcessed == 0);
    uint32_t initLength = Min<uint32_t>(GetBoxedOrUnboxedInitializedLength<Type>(obj), length);
    while (*numProcessed < initLength) {
        if (!CheckForInterrupt(cx))
            return DenseElementResult::Failure;

        const Value& elem = GetBoxedOrUnboxedDenseElement<Type>(obj, *numProcessed);

        if (elem.isString()) {
            if (!sb.append(elem.toString()))
                return DenseElementResult::Failure;
        } else if (elem.isNumber()) {
            if (!NumberValueToStringBuffer(cx, elem, sb))
                return DenseElementResult::Failure;
        } else if (elem.isBoolean()) {
            if (!BooleanToStringBuffer(elem.toBoolean(), sb))
                return DenseElementResult::Failure;
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

        if (++(*numProcessed) != length && !sepOp(cx, sb))
            return DenseElementResult::Failure;
    }

    return DenseElementResult::Incomplete;
}

template <typename SeparatorOp>
struct ArrayJoinDenseKernelFunctor {
    JSContext* cx;
    SeparatorOp sepOp;
    HandleObject obj;
    uint32_t length;
    StringBuffer& sb;
    uint32_t* numProcessed;

    ArrayJoinDenseKernelFunctor(JSContext* cx, SeparatorOp sepOp, HandleObject obj,
                                uint32_t length, StringBuffer& sb, uint32_t* numProcessed)
      : cx(cx), sepOp(sepOp), obj(obj), length(length), sb(sb), numProcessed(numProcessed)
    {}

    template <JSValueType Type>
    DenseElementResult operator()() {
        return ArrayJoinDenseKernel<SeparatorOp, Type>(cx, sepOp, obj, length, sb, numProcessed);
    }
};

template <bool Locale, typename SeparatorOp>
static bool
ArrayJoinKernel(JSContext* cx, SeparatorOp sepOp, HandleObject obj, uint32_t length,
               StringBuffer& sb)
{
    uint32_t i = 0;

    if (!Locale && !ObjectMayHaveExtraIndexedProperties(obj)) {
        ArrayJoinDenseKernelFunctor<SeparatorOp> functor(cx, sepOp, obj, length, sb, &i);
        DenseElementResult result = CallBoxedOrUnboxedSpecialization(functor, obj);
        if (result == DenseElementResult::Failure)
            return false;
    }

    if (i != length) {
        RootedValue v(cx);
        while (i < length) {
            if (!CheckForInterrupt(cx))
                return false;

            bool hole;
            if (!GetElement(cx, obj, i, &hole, &v))
                return false;
            if (!hole && !v.isNullOrUndefined()) {
                if (Locale) {
                    RootedValue fun(cx);
                    if (!GetProperty(cx, v, cx->names().toLocaleString, &fun))
                        return false;
                    if (!Invoke(cx, v, fun, 0, nullptr, &v))
                        return false;
                }
                if (!ValueToStringBuffer(cx, v, sb))
                    return false;
            }

            if (++i != length && !sepOp(cx, sb))
                return false;
        }
    }

    return true;
}

template <bool Locale>
JSString*
js::ArrayJoin(JSContext* cx, HandleObject obj, HandleLinearString sepstr, uint32_t length)
{
    // This method is shared by Array.prototype.join and
    // Array.prototype.toLocaleString. The steps in ES5 are nearly the same, so
    // the annotations in this function apply to both toLocaleString and join.

    // Steps 1 to 6, should be done by the caller.

    // Step 6 is implicit in the loops below.

    // An optimized version of a special case of steps 7-11: when length==1 and
    // the 0th element is a string, ToString() of that element is a no-op and
    // so it can be immediately returned as the result.
    if (length == 1 && !Locale && GetAnyBoxedOrUnboxedInitializedLength(obj) == 1) {
        Value elem0 = GetAnyBoxedOrUnboxedDenseElement(obj, 0);
        if (elem0.isString())
            return elem0.toString();
    }

    StringBuffer sb(cx);
    if (sepstr->hasTwoByteChars() && !sb.ensureTwoByteChars())
        return nullptr;

    // The separator will be added |length - 1| times, reserve space for that
    // so that we don't have to unnecessarily grow the buffer.
    size_t seplen = sepstr->length();
    CheckedInt<uint32_t> res = CheckedInt<uint32_t>(seplen) * (length - 1);
    if (length > 0 && !res.isValid()) {
        ReportAllocationOverflow(cx);
        return nullptr;
    }

    if (length > 0 && !sb.reserve(res.value()))
        return nullptr;

    // Various optimized versions of steps 7-10.
    if (seplen == 0) {
        EmptySeparatorOp op;
        if (!ArrayJoinKernel<Locale>(cx, op, obj, length, sb))
            return nullptr;
    } else if (seplen == 1) {
        char16_t c = sepstr->latin1OrTwoByteChar(0);
        if (c <= JSString::MAX_LATIN1_CHAR) {
            CharSeparatorOp<Latin1Char> op(c);
            if (!ArrayJoinKernel<Locale>(cx, op, obj, length, sb))
                return nullptr;
        } else {
            CharSeparatorOp<char16_t> op(c);
            if (!ArrayJoinKernel<Locale>(cx, op, obj, length, sb))
                return nullptr;
        }
    } else {
        StringSeparatorOp op(sepstr);
        if (!ArrayJoinKernel<Locale>(cx, op, obj, length, sb))
            return nullptr;
    }

    // Step 11
    return sb.finishString();
}

template <bool Locale>
bool
ArrayJoin(JSContext* cx, CallArgs& args)
{
    // Step 1
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

    // Steps 2 and 3
    uint32_t length;
    if (!GetLengthProperty(cx, obj, &length))
        return false;

    // Steps 4 and 5
    RootedLinearString sepstr(cx);
    if (!Locale && args.hasDefined(0)) {
        JSString* s = ToString<CanGC>(cx, args[0]);
        if (!s)
            return false;
        sepstr = s->ensureLinear(cx);
        if (!sepstr)
            return false;
    } else {
        sepstr = cx->names().comma;
    }

    // Step 6 to 11
    JSString* res = js::ArrayJoin<Locale>(cx, obj, sepstr, length);
    if (!res)
        return false;

    args.rval().setString(res);
    return true;
}

/* ES5 15.4.4.3 */
static bool
array_toLocaleString(JSContext* cx, unsigned argc, Value* vp)
{
    JS_CHECK_RECURSION(cx, return false);

    CallArgs args = CallArgsFromVp(argc, vp);
    return ArrayJoin<true>(cx, args);
}

/* ES5 15.4.4.5 */
bool
js::array_join(JSContext* cx, unsigned argc, Value* vp)
{
    JS_CHECK_RECURSION(cx, return false);

    CallArgs args = CallArgsFromVp(argc, vp);
    return ArrayJoin<false>(cx, args);
}

/* vector must point to rooted memory. */
static bool
InitArrayElements(JSContext* cx, HandleObject obj, uint32_t start,
                  uint32_t count, const Value* vector,
                  ShouldUpdateTypes updateTypes = ShouldUpdateTypes::Update)
{
    MOZ_ASSERT(count <= MAX_ARRAY_INDEX);

    if (count == 0)
        return true;

    ObjectGroup* group = obj->getGroup(cx);
    if (!group)
        return false;

    if (!ObjectMayHaveExtraIndexedProperties(obj)) {
        DenseElementResult result =
            SetOrExtendAnyBoxedOrUnboxedDenseElements(cx, obj, start, vector, count, updateTypes);
        if (result != DenseElementResult::Incomplete)
            return result == DenseElementResult::Success;
    }

    const Value* end = vector + count;
    while (vector < end && start <= MAX_ARRAY_INDEX) {
        if (!CheckForInterrupt(cx) ||
            !SetArrayElement(cx, obj, start++, HandleValue::fromMarkedLocation(vector++))) {
            return false;
        }
    }

    if (vector == end)
        return true;

    MOZ_ASSERT(start == MAX_ARRAY_INDEX + 1);
    RootedValue value(cx);
    RootedId id(cx);
    RootedValue indexv(cx);
    double index = MAX_ARRAY_INDEX + 1;
    do {
        value = *vector++;
        indexv = DoubleValue(index);
        if (!ValueToId<CanGC>(cx, indexv, &id))
            return false;
        if (!SetProperty(cx, obj, id, value))
            return false;
        index += 1;
    } while (vector != end);

    return true;
}

template <JSValueType Type>
DenseElementResult
ArrayReverseDenseKernel(JSContext* cx, HandleObject obj, uint32_t length)
{
    /* An empty array or an array with no elements is already reversed. */
    if (length == 0 || GetBoxedOrUnboxedInitializedLength<Type>(obj) == 0)
        return DenseElementResult::Success;

    if (Type == JSVAL_TYPE_MAGIC) {
        /*
         * It's actually surprisingly complicated to reverse an array due to the
         * orthogonality of array length and array capacity while handling
         * leading and trailing holes correctly.  Reversing seems less likely to
         * be a common operation than other array mass-mutation methods, so for
         * now just take a probably-small memory hit (in the absence of too many
         * holes in the array at its start) and ensure that the capacity is
         * sufficient to hold all the elements in the array if it were full.
         */
        DenseElementResult result = obj->as<NativeObject>().ensureDenseElements(cx, length, 0);
        if (result != DenseElementResult::Success)
            return result;

        /* Fill out the array's initialized length to its proper length. */
        obj->as<NativeObject>().ensureDenseInitializedLength(cx, length, 0);
    } else {
        // Unboxed arrays can only be reversed here if their initialized length
        // matches their actual length. Otherwise the reversal will place holes
        // at the beginning of the array, which we don't support.
        if (length != obj->as<UnboxedArrayObject>().initializedLength())
            return DenseElementResult::Incomplete;
    }

    RootedValue origlo(cx), orighi(cx);

    uint32_t lo = 0, hi = length - 1;
    for (; lo < hi; lo++, hi--) {
        origlo = GetBoxedOrUnboxedDenseElement<Type>(obj, lo);
        orighi = GetBoxedOrUnboxedDenseElement<Type>(obj, hi);
        SetBoxedOrUnboxedDenseElementNoTypeChange<Type>(obj, lo, orighi);
        if (orighi.isMagic(JS_ELEMENTS_HOLE) &&
            !SuppressDeletedProperty(cx, obj, INT_TO_JSID(lo)))
        {
            return DenseElementResult::Failure;
        }
        SetBoxedOrUnboxedDenseElementNoTypeChange<Type>(obj, hi, origlo);
        if (origlo.isMagic(JS_ELEMENTS_HOLE) &&
            !SuppressDeletedProperty(cx, obj, INT_TO_JSID(hi)))
        {
            return DenseElementResult::Failure;
        }
    }

    return DenseElementResult::Success;
}

DefineBoxedOrUnboxedFunctor3(ArrayReverseDenseKernel,
                             JSContext*, HandleObject, uint32_t);

static bool
array_reverse(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    RootedObject obj(cx, ToObject(cx, args.thisv()));
    if (!obj)
        return false;

    uint32_t len;
    if (!GetLengthProperty(cx, obj, &len))
        return false;

    if (!ObjectMayHaveExtraIndexedProperties(obj)) {
        ArrayReverseDenseKernelFunctor functor(cx, obj, len);
        DenseElementResult result = CallBoxedOrUnboxedSpecialization(functor, obj);
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

    RootedValue lowval(cx), hival(cx);
    for (uint32_t i = 0, half = len / 2; i < half; i++) {
        bool hole, hole2;
        if (!CheckForInterrupt(cx) ||
            !GetElement(cx, obj, i, &hole, &lowval) ||
            !GetElement(cx, obj, len - i - 1, &hole2, &hival))
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

struct SortComparatorFunction
{
    JSContext*         const cx;
    const Value&       fval;
    FastInvokeGuard&   fig;

    SortComparatorFunction(JSContext* cx, const Value& fval, FastInvokeGuard& fig)
      : cx(cx), fval(fval), fig(fig) { }

    bool operator()(const Value& a, const Value& b, bool* lessOrEqualp);
};

bool
SortComparatorFunction::operator()(const Value& a, const Value& b, bool* lessOrEqualp)
{
    /*
     * array_sort deals with holes and undefs on its own and they should not
     * come here.
     */
    MOZ_ASSERT(!a.isMagic() && !a.isUndefined());
    MOZ_ASSERT(!a.isMagic() && !b.isUndefined());

    if (!CheckForInterrupt(cx))
        return false;

    InvokeArgs& args = fig.args();
    if (!args.init(2))
        return false;

    args.setCallee(fval);
    args.setThis(UndefinedValue());
    args[0].set(a);
    args[1].set(b);

    if (!fig.invoke(cx))
        return false;

    double cmp;
    if (!ToNumber(cx, args.rval(), &cmp))
        return false;

    /*
     * XXX eport some kind of error here if cmp is NaN? ECMA talks about
     * 'consistent compare functions' that don't return NaN, but is silent
     * about what the result should be. So we currently ignore it.
     */
    *lessOrEqualp = (IsNaN(cmp) || cmp <= 0);
    return true;
}

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
MatchNumericComparator(JSContext* cx, const Value& v)
{
    if (!v.isObject())
        return Match_None;

    JSObject& obj = v.toObject();
    if (!obj.is<JSFunction>())
        return Match_None;

    JSFunction* fun = &obj.as<JSFunction>();
    if (!fun->isInterpreted() || fun->isClassConstructor())
        return Match_None;

    JSScript* script = fun->getOrCreateScript(cx);
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
MergeSortByKey(K keys, size_t len, K scratch, C comparator, AutoValueVector* vec)
{
    MOZ_ASSERT(vec->length() >= len);

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
        Value tv = (*vec)[j];
        do {
            size_t k = keys[j].elementIndex;
            keys[j].elementIndex = j;
            (*vec)[j].set((*vec)[k]);
            j = k;
        } while (j != i);

        // We could assert the loop invariant that |i == keys[i].elementIndex|
        // here if we synced |keys[i].elementIndex|.  But doing so would render
        // the assertion vacuous, so don't bother, even in debug builds.
        (*vec)[i].set(tv);
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
SortLexicographically(JSContext* cx, AutoValueVector* vec, size_t len)
{
    MOZ_ASSERT(vec->length() >= len);

    StringBuffer sb(cx);
    Vector<StringifiedElement, 0, TempAllocPolicy> strElements(cx);

    /* MergeSort uses the upper half as scratch space. */
    if (!strElements.reserve(2 * len))
        return false;

    /* Convert Values to strings. */
    size_t cursor = 0;
    for (size_t i = 0; i < len; i++) {
        if (!CheckForInterrupt(cx))
            return false;

        if (!ValueToStringBuffer(cx, (*vec)[i], sb))
            return false;

        StringifiedElement el = { cursor, sb.length(), i };
        strElements.infallibleAppend(el);
        cursor = sb.length();
    }

    /* Resize strElements so we can perform MergeSort. */
    JS_ALWAYS_TRUE(strElements.resize(2 * len));

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
SortNumerically(JSContext* cx, AutoValueVector* vec, size_t len, ComparatorMatchResult comp)
{
    MOZ_ASSERT(vec->length() >= len);

    Vector<NumericElement, 0, TempAllocPolicy> numElements(cx);

    /* MergeSort uses the upper half as scratch space. */
    if (!numElements.reserve(2 * len))
        return false;

    /* Convert Values to numerics. */
    for (size_t i = 0; i < len; i++) {
        if (!CheckForInterrupt(cx))
            return false;

        double dv;
        if (!ToNumber(cx, (*vec)[i], &dv))
            return false;

        NumericElement el = { dv, i };
        numElements.infallibleAppend(el);
    }

    /* Resize strElements so we can perform MergeSort. */
    JS_ALWAYS_TRUE(numElements.resize(2 * len));

    /* Sort Values in vec numerically. */
    return MergeSortByKey(numElements.begin(), len, numElements.begin() + len,
                          SortComparatorNumerics[comp], vec);
}

bool
js::array_sort(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    RootedValue fvalRoot(cx);
    Value& fval = fvalRoot.get();

    if (args.hasDefined(0)) {
        if (args[0].isPrimitive()) {
            JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_BAD_SORT_ARG);
            return false;
        }
        fval = args[0];     /* non-default compare function */
    } else {
        fval.setNull();
    }

    RootedObject obj(cx, ToObject(cx, args.thisv()));
    if (!obj)
        return false;

    uint32_t len;
    if (!GetLengthProperty(cx, obj, &len))
        return false;
    if (len < 2) {
        /* [] and [a] remain unchanged when sorted. */
        args.rval().setObject(*obj);
        return true;
    }

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

    /*
     * Initialize vec as a root. We will clear elements of vec one by
     * one while increasing the rooted amount of vec when we know that the
     * property at the corresponding index exists and its value must be rooted.
     *
     * In this way when sorting a huge mostly sparse array we will not
     * access the tail of vec corresponding to properties that do not
     * exist, allowing OS to avoiding committing RAM. See bug 330812.
     */
    size_t n, undefs;
    {
        AutoValueVector vec(cx);
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
        RootedValue v(cx);
        for (uint32_t i = 0; i < len; i++) {
            if (!CheckForInterrupt(cx))
                return false;

            /* Clear vec[newlen] before including it in the rooted set. */
            bool hole;
            if (!GetElement(cx, obj, i, &hole, &v))
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


        /*
         * If the array only contains holes, we're done.  But if it contains
         * undefs, those must be sorted to the front of the array.
         */
        n = vec.length();
        if (n == 0 && undefs == 0) {
            args.rval().setObject(*obj);
            return true;
        }

        /* Here len == n + undefs + number_of_holes. */
        if (fval.isNull()) {
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
            ComparatorMatchResult comp = MatchNumericComparator(cx, fval);
            if (comp == Match_Failure)
                return false;

            if (comp != Match_None) {
                if (allInts) {
                    JS_ALWAYS_TRUE(vec.resize(n * 2));
                    if (!MergeSort(vec.begin(), n, vec.begin() + n, SortComparatorInt32s[comp]))
                        return false;
                } else {
                    if (!SortNumerically(cx, &vec, n, comp))
                        return false;
                }
            } else {
                FastInvokeGuard fig(cx, fval);
                JS_ALWAYS_TRUE(vec.resize(n * 2));
                if (!MergeSort(vec.begin(), n, vec.begin() + n,
                               SortComparatorFunction(cx, fval, fig)))
                {
                    return false;
                }
            }
        }

        if (!InitArrayElements(cx, obj, 0, uint32_t(n), vec.begin(), ShouldUpdateTypes::DontUpdate))
            return false;
    }

    /* Set undefs that sorted after the rest of elements. */
    while (undefs != 0) {
        --undefs;
        if (!CheckForInterrupt(cx) || !SetArrayElement(cx, obj, n++, UndefinedHandleValue))
            return false;
    }

    /* Re-create any holes that sorted to the end of the array. */
    while (len > n) {
        if (!CheckForInterrupt(cx) || !DeletePropertyOrThrow(cx, obj, --len))
            return false;
    }
    args.rval().setObject(*obj);
    return true;
}

bool
js::NewbornArrayPush(JSContext* cx, HandleObject obj, const Value& v)
{
    Rooted<ArrayObject*> arr(cx, &obj->as<ArrayObject>());

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

/* ES5 15.4.4.7 */
bool
js::array_push(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    /* Step 1. */
    RootedObject obj(cx, ToObject(cx, args.thisv()));
    if (!obj)
        return false;

    /* Steps 2-3. */
    uint32_t length;
    if (!GetLengthProperty(cx, obj, &length))
        return false;

    if (!ObjectMayHaveExtraIndexedProperties(obj)) {
        DenseElementResult result =
            SetOrExtendAnyBoxedOrUnboxedDenseElements(cx, obj, length,
                                                      args.array(), args.length());
        if (result != DenseElementResult::Incomplete) {
            if (result == DenseElementResult::Failure)
                return false;

            uint32_t newlength = length + args.length();
            args.rval().setNumber(newlength);

            // SetOrExtendAnyBoxedOrUnboxedDenseElements takes care of updating the
            // length for boxed and unboxed arrays. Handle updates to the length of
            // non-arrays here.
            bool isArray;
            if (!IsArray(cx, obj, &isArray))
                return false;

            if (!isArray)
                return SetLengthProperty(cx, obj, newlength);

            return true;
        }
    }

    /* Steps 4-5. */
    if (!InitArrayElements(cx, obj, length, args.length(), args.array()))
        return false;

    /* Steps 6-7. */
    double newlength = length + double(args.length());
    args.rval().setNumber(newlength);
    return SetLengthProperty(cx, obj, newlength);
}

/* ES6 20130308 draft 15.4.4.6. */
bool
js::array_pop(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    /* Step 1. */
    RootedObject obj(cx, ToObject(cx, args.thisv()));
    if (!obj)
        return false;

    /* Steps 2-3. */
    uint32_t index;
    if (!GetLengthProperty(cx, obj, &index))
        return false;

    /* Steps 4-5. */
    if (index == 0) {
        /* Step 4b. */
        args.rval().setUndefined();
    } else {
        /* Step 5a. */
        index--;

        /* Step 5b, 5e. */
        bool hole;
        if (!GetElement(cx, obj, index, &hole, args.rval()))
            return false;

        /* Step 5c. */
        if (!hole && !DeletePropertyOrThrow(cx, obj, index))
            return false;
    }

    /* Steps 4a, 5d. */
    return SetLengthProperty(cx, obj, index);
}

template <JSValueType Type>
static inline DenseElementResult
ShiftMoveBoxedOrUnboxedDenseElements(JSObject* obj)
{
    MOZ_ASSERT(HasBoxedOrUnboxedDenseElements<Type>(obj));

    /*
     * At this point the length and initialized length have already been
     * decremented and the result fetched, so just shift the array elements
     * themselves.
     */
    size_t initlen = GetBoxedOrUnboxedInitializedLength<Type>(obj);
    if (Type == JSVAL_TYPE_MAGIC) {
        obj->as<NativeObject>().moveDenseElementsNoPreBarrier(0, 1, initlen);
    } else {
        uint8_t* data = obj->as<UnboxedArrayObject>().elements();
        size_t elementSize = UnboxedTypeSize(Type);
        memmove(data, data + elementSize, initlen * elementSize);
    }

    return DenseElementResult::Success;
}

DefineBoxedOrUnboxedFunctor1(ShiftMoveBoxedOrUnboxedDenseElements, JSObject*);

void
js::ArrayShiftMoveElements(JSObject* obj)
{
    MOZ_ASSERT_IF(obj->is<ArrayObject>(), obj->as<ArrayObject>().lengthIsWritable());

    ShiftMoveBoxedOrUnboxedDenseElementsFunctor functor(obj);
    JS_ALWAYS_TRUE(CallBoxedOrUnboxedSpecialization(functor, obj) == DenseElementResult::Success);
}

template <JSValueType Type>
DenseElementResult
ArrayShiftDenseKernel(JSContext* cx, JSObject* obj, Value* rval)
{
    if (ObjectMayHaveExtraIndexedProperties(obj))
        return DenseElementResult::Incomplete;

    size_t initlen = GetBoxedOrUnboxedInitializedLength<Type>(obj);
    if (initlen == 0)
        return DenseElementResult::Incomplete;

    *rval = GetBoxedOrUnboxedDenseElement<Type>(obj, 0);
    if (rval->isMagic(JS_ELEMENTS_HOLE))
        rval->setUndefined();

    DenseElementResult result = MoveBoxedOrUnboxedDenseElements<Type>(cx, obj, 0, 1, initlen - 1);
    MOZ_ASSERT(result != DenseElementResult::Incomplete);
    if (result == DenseElementResult::Failure)
        return DenseElementResult::Failure;

    SetBoxedOrUnboxedInitializedLength<Type>(cx, obj, initlen - 1);
    return DenseElementResult::Success;
}

DefineBoxedOrUnboxedFunctor3(ArrayShiftDenseKernel,
                             JSContext*, JSObject*, Value*);

/* ES5 15.4.4.9 */
bool
js::array_shift(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    /* Step 1. */
    RootedObject obj(cx, ToObject(cx, args.thisv()));
    if (!obj)
        return false;

    /* Steps 2-3. */
    uint32_t len;
    if (!GetLengthProperty(cx, obj, &len))
        return false;

    /* Step 4. */
    if (len == 0) {
        /* Step 4a. */
        if (!SetLengthProperty(cx, obj, 0))
            return false;

        /* Step 4b. */
        args.rval().setUndefined();
        return true;
    }

    uint32_t newlen = len - 1;

    /* Fast paths. */
    ArrayShiftDenseKernelFunctor functor(cx, obj, args.rval().address());
    DenseElementResult result = CallBoxedOrUnboxedSpecialization(functor, obj);
    if (result != DenseElementResult::Incomplete) {
        if (result == DenseElementResult::Failure)
            return false;

        if (!SetLengthProperty(cx, obj, newlen))
            return false;

        RootedId id(cx);
        if (!IndexToId(cx, newlen, &id))
            return false;
        return SuppressDeletedProperty(cx, obj, id);
    }

    /* Steps 5, 10. */
    bool hole;
    if (!GetElement(cx, obj, uint32_t(0), &hole, args.rval()))
        return false;

    /* Steps 6-7. */
    RootedValue value(cx);
    for (uint32_t i = 0; i < newlen; i++) {
        if (!CheckForInterrupt(cx))
            return false;
        if (!GetElement(cx, obj, i + 1, &hole, &value))
            return false;
        if (hole) {
            if (!DeletePropertyOrThrow(cx, obj, i))
                return false;
        } else {
            if (!SetArrayElement(cx, obj, i, value))
                return false;
        }
    }

    /* Step 8. */
    if (!DeletePropertyOrThrow(cx, obj, newlen))
        return false;

    /* Step 9. */
    return SetLengthProperty(cx, obj, newlen);
}

bool
js::array_unshift(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    RootedObject obj(cx, ToObject(cx, args.thisv()));
    if (!obj)
        return false;

    uint32_t length;
    if (!GetLengthProperty(cx, obj, &length))
        return false;

    double newlen = length;
    if (args.length() > 0) {
        /* Slide up the array to make room for all args at the bottom. */
        if (length > 0) {
            // Only include a fast path for boxed arrays. Unboxed arrays can'nt
            // be optimized here because unshifting temporarily places holes at
            // the start of the array.
            bool optimized = false;
            do {
                if (!obj->is<ArrayObject>())
                    break;
                if (ObjectMayHaveExtraIndexedProperties(obj))
                    break;
                ArrayObject* aobj = &obj->as<ArrayObject>();
                if (!aobj->lengthIsWritable())
                    break;
                DenseElementResult result = aobj->ensureDenseElements(cx, length, args.length());
                if (result != DenseElementResult::Success) {
                    if (result == DenseElementResult::Failure)
                        return false;
                    MOZ_ASSERT(result == DenseElementResult::Incomplete);
                    break;
                }
                aobj->moveDenseElements(args.length(), 0, length);
                for (uint32_t i = 0; i < args.length(); i++)
                    aobj->setDenseElement(i, MagicValue(JS_ELEMENTS_HOLE));
                optimized = true;
            } while (false);

            if (!optimized) {
                double last = length;
                double upperIndex = last + args.length();
                RootedValue value(cx);
                do {
                    --last, --upperIndex;
                    bool hole;
                    if (!CheckForInterrupt(cx))
                        return false;
                    if (!GetElement(cx, obj, last, &hole, &value))
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
        }

        /* Copy from args to the bottom of the array. */
        if (!InitArrayElements(cx, obj, 0, args.length(), args.array()))
            return false;

        newlen += args.length();
    }
    if (!SetLengthProperty(cx, obj, newlen))
        return false;

    /* Follow Perl by returning the new array length. */
    args.rval().setNumber(newlen);
    return true;
}

/*
 * Returns true if this is a dense or unboxed array whose |count| properties
 * starting from |startingIndex| may be accessed (get, set, delete) directly
 * through its contiguous vector of elements without fear of getters, setters,
 * etc. along the prototype chain, or of enumerators requiring notification of
 * modifications.
 */
static inline bool
CanOptimizeForDenseStorage(HandleObject arr, uint32_t startingIndex, uint32_t count, JSContext* cx)
{
    /* If the desired properties overflow dense storage, we can't optimize. */
    if (UINT32_MAX - startingIndex < count)
        return false;

    /* There's no optimizing possible if it's not an array. */
    if (!arr->is<ArrayObject>() && !arr->is<UnboxedArrayObject>())
        return false;

    /*
     * Don't optimize if the array might be in the midst of iteration.  We
     * rely on this to be able to safely move dense array elements around with
     * just a memmove (see NativeObject::moveDenseArrayElements), without worrying
     * about updating any in-progress enumerators for properties implicitly
     * deleted if a hole is moved from one location to another location not yet
     * visited.  See bug 690622.
     */
    ObjectGroup* arrGroup = arr->getGroup(cx);
    if (MOZ_UNLIKELY(!arrGroup || arrGroup->hasAllFlags(OBJECT_FLAG_ITERATED)))
        return false;

    /*
     * Another potential wrinkle: what if the enumeration is happening on an
     * object which merely has |arr| on its prototype chain?
     */
    if (arr->isDelegate())
        return false;

    /*
     * Now watch out for getters and setters along the prototype chain or in
     * other indexed properties on the object.  (Note that non-writable length
     * is subsumed by the initializedLength comparison.)
     */
    return !ObjectMayHaveExtraIndexedProperties(arr) &&
           startingIndex + count <= GetAnyBoxedOrUnboxedInitializedLength(arr);
}

/* ES5 15.4.4.12. */
static bool
array_splice(JSContext* cx, unsigned argc, Value* vp)
{
    return array_splice_impl(cx, argc, vp, true);
}

bool
js::array_splice_impl(JSContext* cx, unsigned argc, Value* vp, bool returnValueIsUsed)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    /* Step 1. */
    RootedObject obj(cx, ToObject(cx, args.thisv()));
    if (!obj)
        return false;

    /* Steps 3-4. */
    uint32_t len;
    if (!GetLengthProperty(cx, obj, &len))
        return false;

    /* Step 5. */
    double relativeStart;
    if (!ToInteger(cx, args.get(0), &relativeStart))
        return false;

    /* Step 6. */
    uint32_t actualStart;
    if (relativeStart < 0)
        actualStart = Max(len + relativeStart, 0.0);
    else
        actualStart = Min(relativeStart, double(len));

    /* Step 7. */
    uint32_t actualDeleteCount;
    if (args.length() != 1) {
        double deleteCountDouble;
        RootedValue cnt(cx, args.length() >= 2 ? args[1] : Int32Value(0));
        if (!ToInteger(cx, cnt, &deleteCountDouble))
            return false;
        actualDeleteCount = Min(Max(deleteCountDouble, 0.0), double(len - actualStart));
    } else {
        /*
         * Non-standard: if start was specified but deleteCount was omitted,
         * delete to the end of the array.  See bug 668024 for discussion.
         */
        actualDeleteCount = len - actualStart;
    }

    MOZ_ASSERT(len - actualStart >= actualDeleteCount);

    /* Steps 2, 8-9. */
    RootedObject arr(cx);
    if (CanOptimizeForDenseStorage(obj, actualStart, actualDeleteCount, cx)) {
        if (returnValueIsUsed) {
            arr = NewFullyAllocatedArrayTryReuseGroup(cx, obj, actualDeleteCount);
            if (!arr)
                return false;
            DebugOnly<DenseElementResult> result =
                CopyAnyBoxedOrUnboxedDenseElements(cx, arr, obj, 0, actualStart, actualDeleteCount);
            MOZ_ASSERT(result.value == DenseElementResult::Success);
        }
    } else {
        arr = NewFullyAllocatedArrayTryReuseGroup(cx, obj, actualDeleteCount);
        if (!arr)
            return false;

        RootedValue fromValue(cx);
        for (uint32_t k = 0; k < actualDeleteCount; k++) {
            bool hole;
            if (!CheckForInterrupt(cx) ||
                !GetElement(cx, obj, actualStart + k, &hole, &fromValue) ||
                (!hole && !DefineElement(cx, arr, k, fromValue)))
            {
                return false;
            }
        }
    }

    /* Step 11. */
    uint32_t itemCount = (args.length() >= 2) ? (args.length() - 2) : 0;

    if (itemCount < actualDeleteCount) {
        /* Step 12: the array is being shrunk. */
        uint32_t sourceIndex = actualStart + actualDeleteCount;
        uint32_t targetIndex = actualStart + itemCount;
        uint32_t finalLength = len - actualDeleteCount + itemCount;

        if (CanOptimizeForDenseStorage(obj, 0, len, cx)) {
            /* Steps 12(a)-(b). */
            DenseElementResult result =
                MoveAnyBoxedOrUnboxedDenseElements(cx, obj, targetIndex, sourceIndex,
                                                   len - sourceIndex);
            MOZ_ASSERT(result != DenseElementResult::Incomplete);
            if (result == DenseElementResult::Failure)
                return false;

            /* Steps 12(c)-(d). */
            SetAnyBoxedOrUnboxedInitializedLength(cx, obj, finalLength);
        } else {
            /*
             * This is all very slow if the length is very large. We don't yet
             * have the ability to iterate in sorted order, so we just do the
             * pessimistic thing and let CheckForInterrupt handle the
             * fallout.
             */

            /* Steps 12(a)-(b). */
            RootedValue fromValue(cx);
            for (uint32_t from = sourceIndex, to = targetIndex; from < len; from++, to++) {
                if (!CheckForInterrupt(cx))
                    return false;

                bool hole;
                if (!GetElement(cx, obj, from, &hole, &fromValue))
                    return false;
                if (hole) {
                    if (!DeletePropertyOrThrow(cx, obj, to))
                        return false;
                } else {
                    if (!SetArrayElement(cx, obj, to, fromValue))
                        return false;
                }
            }

            /* Steps 12(c)-(d). */
            for (uint32_t k = len; k > finalLength; k--) {
                if (!DeletePropertyOrThrow(cx, obj, k - 1))
                    return false;
            }
        }
    } else if (itemCount > actualDeleteCount) {
        /* Step 13. */

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
        if (obj->is<ArrayObject>()) {
            Rooted<ArrayObject*> arr(cx, &obj->as<ArrayObject>());
            if (arr->lengthIsWritable()) {
                DenseElementResult result =
                    arr->ensureDenseElements(cx, arr->length(), itemCount - actualDeleteCount);
                if (result == DenseElementResult::Failure)
                    return false;
            }
        }

        if (CanOptimizeForDenseStorage(obj, len, itemCount - actualDeleteCount, cx)) {
            DenseElementResult result =
                MoveAnyBoxedOrUnboxedDenseElements(cx, obj, actualStart + itemCount,
                                                   actualStart + actualDeleteCount,
                                                   len - (actualStart + actualDeleteCount));
            MOZ_ASSERT(result != DenseElementResult::Incomplete);
            if (result == DenseElementResult::Failure)
                return false;

            /* Steps 12(c)-(d). */
            SetAnyBoxedOrUnboxedInitializedLength(cx, obj, len + itemCount - actualDeleteCount);
        } else {
            RootedValue fromValue(cx);
            for (double k = len - actualDeleteCount; k > actualStart; k--) {
                if (!CheckForInterrupt(cx))
                    return false;

                double from = k + actualDeleteCount - 1;
                double to = k + itemCount - 1;

                bool hole;
                if (!GetElement(cx, obj, from, &hole, &fromValue))
                    return false;

                if (hole) {
                    if (!DeletePropertyOrThrow(cx, obj, to))
                        return false;
                } else {
                    if (!SetArrayElement(cx, obj, to, fromValue))
                        return false;
                }
            }
        }
    }

    /* Step 10. */
    Value* items = args.array() + 2;

    /* Steps 14-15. */
    for (uint32_t k = actualStart, i = 0; i < itemCount; i++, k++) {
        if (!SetArrayElement(cx, obj, k, HandleValue::fromMarkedLocation(&items[i])))
            return false;
    }

    /* Step 16. */
    double finalLength = double(len) - actualDeleteCount + itemCount;
    if (!SetLengthProperty(cx, obj, finalLength))
        return false;

    /* Step 17. */
    if (returnValueIsUsed)
        args.rval().setObject(*arr);

    return true;
}

template <JSValueType TypeOne, JSValueType TypeTwo>
DenseElementResult
ArrayConcatDenseKernel(JSContext* cx, JSObject* obj1, JSObject* obj2, JSObject* result)
{
    uint32_t initlen1 = GetBoxedOrUnboxedInitializedLength<TypeOne>(obj1);
    MOZ_ASSERT(initlen1 == GetAnyBoxedOrUnboxedArrayLength(obj1));

    uint32_t initlen2 = GetBoxedOrUnboxedInitializedLength<TypeTwo>(obj2);
    MOZ_ASSERT(initlen2 == GetAnyBoxedOrUnboxedArrayLength(obj2));

    /* No overflow here due to nelements limit. */
    uint32_t len = initlen1 + initlen2;

    MOZ_ASSERT(GetBoxedOrUnboxedInitializedLength<TypeOne>(result) == 0);

    DenseElementResult rv = EnsureBoxedOrUnboxedDenseElements<TypeOne>(cx, result, len);
    if (rv != DenseElementResult::Success)
        return rv;

    CopyBoxedOrUnboxedDenseElements<TypeOne, TypeOne>(cx, result, obj1, 0, 0, initlen1);
    CopyBoxedOrUnboxedDenseElements<TypeOne, TypeTwo>(cx, result, obj2, initlen1, 0, initlen2);

    SetAnyBoxedOrUnboxedArrayLength(cx, result, len);
    return DenseElementResult::Success;
}

DefineBoxedOrUnboxedFunctorPair4(ArrayConcatDenseKernel,
                                 JSContext*, JSObject*, JSObject*, JSObject*);

bool
js::array_concat_dense(JSContext* cx, HandleObject obj1, HandleObject obj2,
                       HandleObject result)
{
    ArrayConcatDenseKernelFunctor functor(cx, obj1, obj2, result);
    DenseElementResult rv = CallBoxedOrUnboxedSpecialization(functor, obj1, obj2);
    MOZ_ASSERT(rv != DenseElementResult::Incomplete);
    return rv == DenseElementResult::Success;
}

/*
 * Python-esque sequence operations.
 */
bool
js::array_concat(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    /* Treat our |this| object as the first argument; see ECMA 15.4.4.4. */
    Value* p = args.array() - 1;

    /* Create a new Array object and root it using *vp. */
    RootedObject aobj(cx, ToObject(cx, args.thisv()));
    if (!aobj)
        return false;

    RootedObject narr(cx);
    uint32_t length;
    bool isArray;
    if (!IsArray(cx, aobj, &isArray))
        return false;
    if (isArray && !ObjectMayHaveExtraIndexedProperties(aobj)) {
        if (!GetLengthProperty(cx, aobj, &length))
            return false;

        size_t initlen = GetAnyBoxedOrUnboxedInitializedLength(aobj);
        narr = NewFullyAllocatedArrayTryReuseGroup(cx, aobj, initlen);
        if (!narr)
            return false;
        CopyAnyBoxedOrUnboxedDenseElements(cx, narr, aobj, 0, 0, initlen);
        SetAnyBoxedOrUnboxedArrayLength(cx, narr, length);

        args.rval().setObject(*narr);
        if (argc == 0)
            return true;
        argc--;
        p++;

        if (length == initlen) {
            while (argc) {
                HandleValue v = HandleValue::fromMarkedLocation(p);
                if (!v.isObject())
                    break;
                RootedObject obj(cx, &v.toObject());

                // This should be IsConcatSpreadable
                bool isArray;
                if (!IsArray(cx, obj, &isArray))
                    return false;
                if (!isArray || ObjectMayHaveExtraIndexedProperties(obj))
                    break;

                uint32_t argLength;
                if (!GetLengthProperty(cx, obj, &argLength))
                    return false;

                initlen = GetAnyBoxedOrUnboxedInitializedLength(obj);
                if (argLength != initlen)
                    break;

                DenseElementResult result =
                    EnsureAnyBoxedOrUnboxedDenseElements(cx, narr, length + argLength);
                if (result == DenseElementResult::Failure)
                    return false;
                if (result == DenseElementResult::Incomplete)
                    break;

                SetAnyBoxedOrUnboxedInitializedLength(cx, narr, length + argLength);

                bool success = true;
                for (size_t i = 0; i < initlen; i++) {
                    Value v = GetAnyBoxedOrUnboxedDenseElement(obj, i);
                    if (!InitAnyBoxedOrUnboxedDenseElement(cx, narr, length + i, v)) {
                        success = false;
                        break;
                    }
                }
                if (!success) {
                    SetAnyBoxedOrUnboxedInitializedLength(cx, narr, length);
                    break;
                }

                length += argLength;
                SetAnyBoxedOrUnboxedArrayLength(cx, narr, length);

                argc--;
                p++;
            }
        }
    } else {
        narr = NewFullyAllocatedArrayTryReuseGroup(cx, aobj, 0);
        if (!narr)
            return false;
        args.rval().setObject(*narr);
        length = 0;
    }

    /* Loop over [0, argc] to concat args into narr, expanding all Arrays. */
    for (unsigned i = 0; i <= argc; i++) {
        if (!CheckForInterrupt(cx))
            return false;
        HandleValue v = HandleValue::fromMarkedLocation(&p[i]);
        if (v.isObject()) {
            RootedObject obj(cx, &v.toObject());
            // This should be IsConcatSpreadable
            bool isArray;
            if (!IsArray(cx, obj, &isArray))
                return false;
            if (isArray) {
                uint32_t alength;
                if (!GetLengthProperty(cx, obj, &alength))
                    return false;
                RootedValue tmp(cx);
                for (uint32_t slot = 0; slot < alength; slot++) {
                    bool hole;
                    if (!CheckForInterrupt(cx) || !GetElement(cx, obj, slot, &hole, &tmp))
                        return false;

                    /*
                     * Per ECMA 262, 15.4.4.4, step 9, ignore nonexistent
                     * properties.
                     */
                    if (!hole && !SetArrayElement(cx, narr, length + slot, tmp))
                        return false;
                }
                length += alength;
                continue;
            }
        }

        if (!SetArrayElement(cx, narr, length, v))
            return false;
        length++;
    }

    return SetLengthProperty(cx, narr, length);
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
GetIndexedPropertiesInRange(JSContext* cx, HandleObject obj, uint32_t begin, uint32_t end,
                            Vector<uint32_t>& indexes, bool* success)
{
    *success = false;

    // First, look for proxies or class hooks that can introduce extra
    // properties.
    JSObject* pobj = obj;
    do {
        if (!pobj->isNative() || pobj->getClass()->resolve || pobj->getOps()->lookupProperty)
            return true;
    } while ((pobj = pobj->getProto()));

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
        if (IsAnyTypedArray(pobj)) {
            uint32_t len = AnyTypedArrayLength(pobj);
            for (uint32_t i = begin; i < len && i < end; i++) {
                if (!indexes.append(i))
                    return false;
            }
        }

        // Append sparse elements.
        if (pobj->isIndexed()) {
            Shape::Range<NoGC> r(pobj->as<NativeObject>().lastProperty());
            for (; !r.empty(); r.popFront()) {
                Shape& shape = r.front();
                jsid id = shape.propid();
                if (!JSID_IS_INT(id))
                    continue;

                uint32_t i = uint32_t(JSID_TO_INT(id));
                if (!(begin <= i && i < end))
                    continue;

                // Watch out for getters, they can add new properties.
                if (!shape.hasDefaultGetter())
                    return true;

                if (!indexes.append(i))
                    return false;
            }
        }
    } while ((pobj = pobj->getProto()));

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
SliceSlowly(JSContext* cx, HandleObject obj, HandleObject receiver,
            uint32_t begin, uint32_t end, HandleObject result)
{
    RootedValue value(cx);
    for (uint32_t slot = begin; slot < end; slot++) {
        bool hole;
        if (!CheckForInterrupt(cx) ||
            !GetElement(cx, obj, receiver, slot, &hole, &value))
        {
            return false;
        }
        if (!hole && !DefineElement(cx, result, slot - begin, value))
            return false;
    }
    return true;
}

static bool
SliceSparse(JSContext* cx, HandleObject obj, uint32_t begin, uint32_t end, HandleObject result)
{
    MOZ_ASSERT(begin <= end);

    Vector<uint32_t> indexes(cx);
    bool success;
    if (!GetIndexedPropertiesInRange(cx, obj, begin, end, indexes, &success))
        return false;

    if (!success)
        return SliceSlowly(cx, obj, obj, begin, end, result);

    RootedValue value(cx);
    for (size_t i = 0, len = indexes.length(); i < len; i++) {
        uint32_t index = indexes[i];
        MOZ_ASSERT(begin <= index && index < end);

        bool hole;
        if (!GetElement(cx, obj, obj, index, &hole, &value))
            return false;

        if (!hole && !DefineElement(cx, result, index - begin, value))
            return false;
    }

    return true;
}

template <typename T>
static inline uint32_t
NormalizeSliceTerm(T value, uint32_t length)
{
    if (value < 0) {
        value += length;
        if (value < 0)
            return 0;
    } else if (double(value) > double(length)) {
        return length;
    }
    return uint32_t(value);
}

bool
js::array_slice(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    RootedObject obj(cx, ToObject(cx, args.thisv()));
    if (!obj)
        return false;

    uint32_t length;
    if (!GetLengthProperty(cx, obj, &length))
        return false;

    uint32_t begin = 0;
    uint32_t end = length;
    if (args.length() > 0) {
        double d;
        if (!ToInteger(cx, args[0], &d))
            return false;
        begin = NormalizeSliceTerm(d, length);

        if (args.hasDefined(1)) {
            if (!ToInteger(cx, args[1], &d))
                return false;
            end = NormalizeSliceTerm(d, length);
        }
    }

    if (begin > end)
        begin = end;

    if (!ObjectMayHaveExtraIndexedProperties(obj)) {
        size_t initlen = GetAnyBoxedOrUnboxedInitializedLength(obj);
        size_t count = 0;
        if (initlen > begin)
            count = Min<size_t>(initlen - begin, end - begin);

        RootedObject narr(cx, NewFullyAllocatedArrayTryReuseGroup(cx, obj, count));
        if (!narr)
            return false;
        SetAnyBoxedOrUnboxedArrayLength(cx, narr, end - begin);

        if (count) {
            DebugOnly<DenseElementResult> result =
                CopyAnyBoxedOrUnboxedDenseElements(cx, narr, obj, 0, begin, count);
            MOZ_ASSERT(result.value == DenseElementResult::Success);
        }
        args.rval().setObject(*narr);
        return true;
    }

    RootedObject narr(cx, NewPartlyAllocatedArrayTryReuseGroup(cx, obj, end - begin));
    if (!narr)
        return false;

    if (js::GetElementsOp op = obj->getOps()->getElements) {
        ElementAdder adder(cx, narr, end - begin, ElementAdder::CheckHasElemPreserveHoles);
        if (!op(cx, obj, begin, end, &adder))
            return false;

        args.rval().setObject(*narr);
        return true;
    }

    if (obj->isNative() && obj->isIndexed() && end - begin > 1000) {
        if (!SliceSparse(cx, obj, begin, end, narr))
            return false;
    } else {
        if (!SliceSlowly(cx, obj, obj, begin, end, narr))
            return false;
    }

    args.rval().setObject(*narr);
    return true;
}

template <JSValueType Type>
DenseElementResult
ArraySliceDenseKernel(JSContext* cx, JSObject* obj, int32_t beginArg, int32_t endArg, JSObject* result)
{
    int32_t length = GetAnyBoxedOrUnboxedArrayLength(obj);

    uint32_t begin = NormalizeSliceTerm(beginArg, length);
    uint32_t end = NormalizeSliceTerm(endArg, length);

    if (begin > end)
        begin = end;

    size_t initlen = GetBoxedOrUnboxedInitializedLength<Type>(obj);
    if (initlen > begin) {
        size_t count = Min<size_t>(initlen - begin, end - begin);
        if (count) {
            DenseElementResult rv = EnsureBoxedOrUnboxedDenseElements<Type>(cx, result, count);
            if (rv != DenseElementResult::Success)
                return rv;
            CopyBoxedOrUnboxedDenseElements<Type, Type>(cx, result, obj, 0, begin, count);
        }
    }

    SetAnyBoxedOrUnboxedArrayLength(cx, result, end - begin);
    return DenseElementResult::Success;
}

DefineBoxedOrUnboxedFunctor5(ArraySliceDenseKernel,
                             JSContext*, JSObject*, int32_t, int32_t, JSObject*);

JSObject*
js::array_slice_dense(JSContext* cx, HandleObject obj, int32_t begin, int32_t end,
                      HandleObject result)
{
    if (result) {
        ArraySliceDenseKernelFunctor functor(cx, obj, begin, end, result);
        DenseElementResult rv = CallBoxedOrUnboxedSpecialization(functor, result);
        MOZ_ASSERT(rv != DenseElementResult::Incomplete);
        return rv == DenseElementResult::Success ? result : nullptr;
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
IsArrayConstructor(const Value& v)
{
    // This must only return true if v is *the* Array constructor for the
    // current compartment; we rely on the fact that any other Array
    // constructor would be represented as a wrapper.
    return v.isObject() &&
           v.toObject().is<JSFunction>() &&
           v.toObject().as<JSFunction>().isNative() &&
           v.toObject().as<JSFunction>().native() == ArrayConstructor;
}

static bool
ArrayFromCallArgs(JSContext* cx, CallArgs& args, HandleObject proto = nullptr)
{
    JSObject* obj = NewCopiedArrayForCallingAllocationSite(cx, args.array(), args.length(), proto);
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
        ConstructArgs cargs(cx);
        if (!cargs.init(1))
            return false;
        cargs[0].setNumber(args.length());

        RootedValue v(cx);
        if (!Construct(cx, args.thisv(), cargs, args.thisv(), &v))
            return false;

        obj = &v.toObject();
    }

    // Step 8.
    for (unsigned k = 0; k < args.length(); k++) {
        if (!DefineElement(cx, obj, k, args[k]))
            return false;
    }

    // Steps 9-10.
    if (!SetLengthProperty(cx, obj, args.length()))
        return false;

    // Step 11.
    args.rval().setObject(*obj);
    return true;
}

#define GENERIC JSFUN_GENERIC_NATIVE

static const JSFunctionSpec array_methods[] = {
#if JS_HAS_TOSOURCE
    JS_FN(js_toSource_str,      array_toSource,     0,0),
#endif
    JS_SELF_HOSTED_FN(js_toString_str, "ArrayToString",      0,0),
    JS_FN(js_toLocaleString_str,       array_toLocaleString, 0,0),

    /* Perl-ish methods. */
    JS_FN("join",               array_join,         1,JSFUN_GENERIC_NATIVE),
    JS_FN("reverse",            array_reverse,      0,JSFUN_GENERIC_NATIVE),
    JS_FN("sort",               array_sort,         1,JSFUN_GENERIC_NATIVE),
    JS_INLINABLE_FN("push",     array_push,         1,JSFUN_GENERIC_NATIVE, ArrayPush),
    JS_INLINABLE_FN("pop",      array_pop,          0,JSFUN_GENERIC_NATIVE, ArrayPop),
    JS_INLINABLE_FN("shift",    array_shift,        0,JSFUN_GENERIC_NATIVE, ArrayShift),
    JS_FN("unshift",            array_unshift,      1,JSFUN_GENERIC_NATIVE),
    JS_INLINABLE_FN("splice",   array_splice,       2,JSFUN_GENERIC_NATIVE, ArraySplice),

    /* Pythonic sequence methods. */
    JS_INLINABLE_FN("concat",   array_concat,       1,JSFUN_GENERIC_NATIVE, ArrayConcat),
    JS_INLINABLE_FN("slice",    array_slice,        2,JSFUN_GENERIC_NATIVE, ArraySlice),

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

    /* ES7 additions */
    JS_SELF_HOSTED_FN("includes",    "ArrayIncludes",    2,0),
    JS_FS_END
};

static const JSFunctionSpec array_static_methods[] = {
    JS_INLINABLE_FN("isArray",       array_isArray,        1,0, ArrayIsArray),
    JS_SELF_HOSTED_FN("lastIndexOf", "ArrayStaticLastIndexOf", 2,0),
    JS_SELF_HOSTED_FN("indexOf",     "ArrayStaticIndexOf", 2,0),
    JS_SELF_HOSTED_FN("forEach",     "ArrayStaticForEach", 2,0),
    JS_SELF_HOSTED_FN("map",         "ArrayStaticMap",   2,0),
    JS_SELF_HOSTED_FN("filter",      "ArrayStaticFilter", 2,0),
    JS_SELF_HOSTED_FN("every",       "ArrayStaticEvery", 2,0),
    JS_SELF_HOSTED_FN("some",        "ArrayStaticSome",  2,0),
    JS_SELF_HOSTED_FN("reduce",      "ArrayStaticReduce", 2,0),
    JS_SELF_HOSTED_FN("reduceRight", "ArrayStaticReduceRight", 2,0),
    JS_SELF_HOSTED_FN("from",        "ArrayFrom", 3,0),
    JS_FN("of",                 array_of,           0,0),

    JS_FS_END
};

/* ES5 15.4.2 */
bool
js::ArrayConstructor(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    RootedObject proto(cx);
    if (!GetPrototypeFromCallableConstructor(cx, args, &proto))
        return false;

    if (args.length() != 1 || !args[0].isNumber())
        return ArrayFromCallArgs(cx, args, proto);

    uint32_t length;
    if (args[0].isInt32()) {
        int32_t i = args[0].toInt32();
        if (i < 0) {
            JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_BAD_ARRAY_LENGTH);
            return false;
        }
        length = uint32_t(i);
    } else {
        double d = args[0].toDouble();
        length = ToUint32(d);
        if (d != double(length)) {
            JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_BAD_ARRAY_LENGTH);
            return false;
        }
    }

    JSObject* obj = NewPartlyAllocatedArrayForCallingAllocationSite(cx, length, proto);
    if (!obj)
        return false;

    args.rval().setObject(*obj);
    return true;
}

JSObject*
js::ArrayConstructorOneArg(JSContext* cx, HandleObjectGroup group, int32_t lengthInt)
{
    if (lengthInt < 0) {
        JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_BAD_ARRAY_LENGTH);
        return nullptr;
    }

    uint32_t length = uint32_t(lengthInt);
    return NewPartlyAllocatedArrayTryUseGroup(cx, group, length);
}

static JSObject*
CreateArrayPrototype(JSContext* cx, JSProtoKey key)
{
    MOZ_ASSERT(key == JSProto_Array);
    RootedObject proto(cx, cx->global()->getOrCreateObjectPrototype(cx));
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
        !arrayProto->setDelegate(cx) ||
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

const Class ArrayObject::class_ = {
    "Array",
    JSCLASS_HAS_CACHED_PROTO(JSProto_Array) | JSCLASS_DELAY_METADATA_CALLBACK,
    array_addProperty,
    nullptr, /* delProperty */
    nullptr, /* getProperty */
    nullptr, /* setProperty */
    nullptr, /* enumerate */
    nullptr, /* resolve */
    nullptr, /* mayResolve */
    nullptr, /* finalize */
    nullptr, /* call */
    nullptr, /* hasInstance */
    nullptr, /* construct */
    nullptr, /* trace */
    {
        GenericCreateConstructor<ArrayConstructor, 1, AllocKind::FUNCTION, &jit::JitInfo_Array>,
        CreateArrayPrototype,
        array_static_methods,
        nullptr,
        array_methods
    }
};

/*
 * Array allocation functions.
 */

static inline bool
EnsureNewArrayElements(ExclusiveContext* cx, ArrayObject* obj, uint32_t length)
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
NewArray(ExclusiveContext* cxArg, uint32_t length,
         HandleObject protoArg, NewObjectKind newKind = GenericObject)
{
    gc::AllocKind allocKind = GuessArrayGCKind(length);
    MOZ_ASSERT(CanBeFinalizedInBackground(allocKind, &ArrayObject::class_));
    allocKind = GetBackgroundAllocKind(allocKind);

    RootedObject proto(cxArg, protoArg);
    if (!proto && !GetBuiltinPrototype(cxArg, JSProto_Array, &proto))
        return nullptr;

    Rooted<TaggedProto> taggedProto(cxArg, TaggedProto(proto));
    bool isCachable = NewObjectWithTaggedProtoIsCachable(cxArg, taggedProto, newKind, &ArrayObject::class_);
    if (isCachable) {
        JSContext* cx = cxArg->asJSContext();
        JSRuntime* rt = cx->runtime();
        NewObjectCache& cache = rt->newObjectCache;
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

    RootedObjectGroup group(cxArg, ObjectGroup::defaultNewGroup(cxArg, &ArrayObject::class_,
                                                                TaggedProto(proto)));
    if (!group)
        return nullptr;

    /*
     * Get a shape with zero fixed slots, regardless of the size class.
     * See JSObject::createArray.
     */
    RootedShape shape(cxArg, EmptyShape::getInitialShape(cxArg, &ArrayObject::class_,
                                                         TaggedProto(proto),
                                                         gc::AllocKind::OBJECT0));
    if (!shape)
        return nullptr;

    AutoSetNewObjectMetadata metadata(cxArg);
    RootedArrayObject arr(cxArg, ArrayObject::createArray(cxArg, allocKind,
                                                          GetInitialHeap(newKind, &ArrayObject::class_),
                                                          shape, group, length, metadata));
    if (!arr)
        return nullptr;

    if (shape->isEmptyShape()) {
        if (!AddLengthProperty(cxArg, arr))
            return nullptr;
        shape = arr->lastProperty();
        EmptyShape::insertInitialShape(cxArg, shape, proto);
    }

    if (newKind == SingletonObject && !JSObject::setSingleton(cxArg, arr))
        return nullptr;

    if (isCachable) {
        NewObjectCache& cache = cxArg->asJSContext()->runtime()->newObjectCache;
        NewObjectCache::EntryIndex entry = -1;
        cache.lookupProto(&ArrayObject::class_, proto, allocKind, &entry);
        cache.fillProto(entry, &ArrayObject::class_, taggedProto, allocKind, arr);
    }

    if (maxLength > 0 && !EnsureNewArrayElements(cxArg, arr, std::min(maxLength, length)))
        return nullptr;

    probes::CreateObject(cxArg, arr);
    return arr;
}

ArrayObject * JS_FASTCALL
js::NewDenseEmptyArray(JSContext* cx, HandleObject proto /* = nullptr */,
                       NewObjectKind newKind /* = GenericObject */)
{
    return NewArray<0>(cx, 0, proto, newKind);
}

ArrayObject * JS_FASTCALL
js::NewDenseFullyAllocatedArray(ExclusiveContext* cx, uint32_t length,
                                HandleObject proto /* = nullptr */,
                                NewObjectKind newKind /* = GenericObject */)
{
    return NewArray<UINT32_MAX>(cx, length, proto, newKind);
}

ArrayObject * JS_FASTCALL
js::NewDensePartlyAllocatedArray(ExclusiveContext* cx, uint32_t length,
                                 HandleObject proto /* = nullptr */,
                                 NewObjectKind newKind /* = GenericObject */)
{
    return NewArray<ArrayObject::EagerAllocationMaxLength>(cx, length, proto, newKind);
}

ArrayObject * JS_FASTCALL
js::NewDenseUnallocatedArray(ExclusiveContext* cx, uint32_t length,
                             HandleObject proto /* = nullptr */,
                             NewObjectKind newKind /* = GenericObject */)
{
    return NewArray<0>(cx, length, proto, newKind);
}

// values must point at already-rooted Value objects
ArrayObject*
js::NewDenseCopiedArray(ExclusiveContext* cx, uint32_t length, const Value* values,
                        HandleObject proto /* = nullptr */,
                        NewObjectKind newKind /* = GenericObject */)
{
    ArrayObject* arr = NewArray<UINT32_MAX>(cx, length, proto, newKind);
    if (!arr)
        return nullptr;

    MOZ_ASSERT(arr->getDenseCapacity() >= length);

    arr->setDenseInitializedLength(values ? length : 0);

    if (values)
        arr->initDenseElements(0, values, length);

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

JSObject*
js::NewDenseCopyOnWriteArray(JSContext* cx, HandleArrayObject templateObject, gc::InitialHeap heap)
{
    MOZ_ASSERT(!gc::IsInsideNursery(templateObject));

    ArrayObject* arr = ArrayObject::createCopyOnWriteArray(cx, heap, templateObject);
    if (!arr)
        return nullptr;

    probes::CreateObject(cx, arr);
    return arr;
}

// Return a new boxed or unboxed array with the specified length and allocated
// capacity (up to maxLength), using the specified group if possible. If the
// specified group cannot be used, ensure that the created array at least has
// the given [[Prototype]].
template <uint32_t maxLength>
static inline JSObject*
NewArrayTryUseGroup(ExclusiveContext* cx, HandleObjectGroup group, size_t length,
                    NewObjectKind newKind = GenericObject, bool forceAnalyze = false)
{
    MOZ_ASSERT(newKind != SingletonObject);

    if (group->maybePreliminaryObjects())
        group->maybePreliminaryObjects()->maybeAnalyze(cx, group, forceAnalyze);

    if (group->shouldPreTenure() || group->maybePreliminaryObjects())
        newKind = TenuredObject;

    RootedObject proto(cx, group->proto().toObject());
    if (group->maybeUnboxedLayout()) {
        if (length > UnboxedArrayObject::MaximumCapacity)
            return NewArray<maxLength>(cx, length, proto, newKind);
        return UnboxedArrayObject::create(cx, group, length, newKind, maxLength);
    }

    ArrayObject* res = NewArray<maxLength>(cx, length, proto, newKind);
    if (!res)
        return nullptr;

    res->setGroup(group);

    // If the length calculation overflowed, make sure that is marked for the
    // new group.
    if (res->length() > INT32_MAX)
        res->setLength(cx, res->length());

    if (PreliminaryObjectArray* preliminaryObjects = group->maybePreliminaryObjects())
        preliminaryObjects->registerNewObject(res);

    return res;
}

JSObject*
js::NewFullyAllocatedArrayTryUseGroup(ExclusiveContext* cx, HandleObjectGroup group, size_t length,
                                      NewObjectKind newKind, bool forceAnalyze)
{
    return NewArrayTryUseGroup<UINT32_MAX>(cx, group, length, newKind, forceAnalyze);
}

JSObject*
js::NewPartlyAllocatedArrayTryUseGroup(ExclusiveContext* cx, HandleObjectGroup group, size_t length)
{
    return NewArrayTryUseGroup<ArrayObject::EagerAllocationMaxLength>(cx, group, length);
}

// Return a new array with the default prototype and specified allocated
// capacity and length. If possible, try to reuse the group of the input
// object. The resulting array will either reuse the input object's group or
// will have unknown property types. Additionally, the result will have the
// same boxed/unboxed elements representation as the input object, unless
// |length| is larger than the input object's initialized length (in which case
// UnboxedArrayObject::MaximumCapacity might be exceeded).
template <uint32_t maxLength>
static inline JSObject*
NewArrayTryReuseGroup(JSContext* cx, JSObject* obj, size_t length,
                      NewObjectKind newKind = GenericObject, bool forceAnalyze = false)
{
    if (!obj->is<ArrayObject>() && !obj->is<UnboxedArrayObject>())
        return NewArray<maxLength>(cx, length, nullptr, newKind);

    if (obj->getProto() != cx->global()->maybeGetArrayPrototype())
        return NewArray<maxLength>(cx, length, nullptr, newKind);

    RootedObjectGroup group(cx, obj->getGroup(cx));
    if (!group)
        return nullptr;

    return NewArrayTryUseGroup<maxLength>(cx, group, length, newKind, forceAnalyze);
}

JSObject*
js::NewFullyAllocatedArrayTryReuseGroup(JSContext* cx, JSObject* obj, size_t length,
                                        NewObjectKind newKind, bool forceAnalyze)
{
    return NewArrayTryReuseGroup<UINT32_MAX>(cx, obj, length, newKind, forceAnalyze);
}

JSObject*
js::NewPartlyAllocatedArrayTryReuseGroup(JSContext* cx, JSObject* obj, size_t length)
{
    return NewArrayTryReuseGroup<ArrayObject::EagerAllocationMaxLength>(cx, obj, length);
}

JSObject*
js::NewFullyAllocatedArrayForCallingAllocationSite(JSContext* cx, size_t length,
                                                   NewObjectKind newKind, bool forceAnalyze)
{
    RootedObjectGroup group(cx, ObjectGroup::callingAllocationSiteGroup(cx, JSProto_Array));
    if (!group)
        return nullptr;
    return NewArrayTryUseGroup<UINT32_MAX>(cx, group, length, newKind, forceAnalyze);
}

JSObject*
js::NewPartlyAllocatedArrayForCallingAllocationSite(JSContext* cx, size_t length, HandleObject proto)
{
    RootedObjectGroup group(cx, ObjectGroup::callingAllocationSiteGroup(cx, JSProto_Array, proto));
    if (!group)
        return nullptr;
    return NewArrayTryUseGroup<ArrayObject::EagerAllocationMaxLength>(cx, group, length);
}

JSObject*
js::NewCopiedArrayTryUseGroup(ExclusiveContext* cx, HandleObjectGroup group,
                              const Value* vp, size_t length, NewObjectKind newKind,
                              ShouldUpdateTypes updateTypes)
{
    bool forceAnalyze = false;

    static const size_t EagerPreliminaryObjectAnalysisThreshold = 800;

    // Force analysis to see if an unboxed array can be used when making a
    // sufficiently large array, to avoid excessive analysis and copying later
    // on. If this is the first array of its group that is being created, first
    // make a dummy array with the initial elements of the array we are about
    // to make, so there is some basis for the unboxed array analysis.
    if (length > EagerPreliminaryObjectAnalysisThreshold) {
        if (PreliminaryObjectArrayWithTemplate* objects = group->maybePreliminaryObjects()) {
            if (objects->empty()) {
                size_t nlength = Min<size_t>(length, 100);
                JSObject* obj = NewFullyAllocatedArrayTryUseGroup(cx, group, nlength);
                if (!obj)
                    return nullptr;
                DebugOnly<DenseElementResult> result =
                    SetOrExtendAnyBoxedOrUnboxedDenseElements(cx, obj, 0, vp, nlength, updateTypes);
                MOZ_ASSERT(result.value == DenseElementResult::Success);
            }
        }
        forceAnalyze = true;
    }

    JSObject* obj = NewFullyAllocatedArrayTryUseGroup(cx, group, length, newKind, forceAnalyze);
    if (!obj)
        return nullptr;

    DenseElementResult result =
        SetOrExtendAnyBoxedOrUnboxedDenseElements(cx, obj, 0, vp, length, updateTypes);
    if (result == DenseElementResult::Failure)
        return nullptr;
    if (result == DenseElementResult::Success)
        return obj;

    MOZ_ASSERT(obj->is<UnboxedArrayObject>());
    if (!UnboxedArrayObject::convertToNative(cx->asJSContext(), obj))
        return nullptr;

    result = SetOrExtendBoxedOrUnboxedDenseElements<JSVAL_TYPE_MAGIC>(cx, obj, 0, vp, length,
                                                                      updateTypes);
    MOZ_ASSERT(result != DenseElementResult::Incomplete);
    if (result == DenseElementResult::Failure)
        return nullptr;

    return obj;
}

JSObject*
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
    JSObject* obj;

    for (unsigned i = 0; i < args.length(); i++) {
        RootedValue arg(cx, args[i]);

        UniquePtr<char[], JS::FreePolicy> bytes =
            DecompileValueGenerator(cx, JSDVG_SEARCH_STACK, arg, nullptr);
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

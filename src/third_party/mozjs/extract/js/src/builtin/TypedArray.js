/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "TypedArrayConstants.h"

function ViewedArrayBufferIfReified(tarray) {
    assert(IsTypedArray(tarray), "non-typed array asked for its buffer");

    var buf = UnsafeGetReservedSlot(tarray, JS_TYPEDARRAYLAYOUT_BUFFER_SLOT);
    assert(buf === null || (IsObject(buf) && (GuardToArrayBuffer(buf) !== null || GuardToSharedArrayBuffer(buf) !== null)),
           "unexpected value in buffer slot");
    return buf;
}

function IsDetachedBuffer(buffer) {
    // A typed array with a null buffer has never had its buffer exposed to
    // become detached.
    if (buffer === null)
        return false;

    assert(GuardToArrayBuffer(buffer) !== null || GuardToSharedArrayBuffer(buffer) !== null,
           "non-ArrayBuffer passed to IsDetachedBuffer");

    // Shared array buffers are not detachable.
    //
    // This check is more expensive than desirable, but IsDetachedBuffer is
    // only hot for non-shared memory in SetFromNonTypedArray, so there is an
    // optimization in place there to avoid incurring the cost here.  An
    // alternative is to give SharedArrayBuffer the same layout as ArrayBuffer.
    if ((buffer = GuardToArrayBuffer(buffer)) === null)
        return false;

    var flags = UnsafeGetInt32FromReservedSlot(buffer, JS_ARRAYBUFFER_FLAGS_SLOT);
    return (flags & JS_ARRAYBUFFER_DETACHED_FLAG) !== 0;
}

function TypedArrayLengthMethod() {
    return TypedArrayLength(this);
}

function TypedArrayByteOffsetMethod() {
    return TypedArrayByteOffset(this);
}

function GetAttachedArrayBuffer(tarray) {
    var buffer = ViewedArrayBufferIfReified(tarray);
    if (IsDetachedBuffer(buffer))
        ThrowTypeError(JSMSG_TYPED_ARRAY_DETACHED);
    return buffer;
}

function GetAttachedArrayBufferMethod() {
    return GetAttachedArrayBuffer(this);
}

// A function which ensures that the argument is either a typed array or a
// cross-compartment wrapper for a typed array and that the typed array involved
// has an attached array buffer.  If one of those conditions doesn't hold (wrong
// kind of argument, or detached array buffer), an exception is thrown.  The
// return value is `true` if the argument is a typed array, `false` if it's a
// cross-compartment wrapper for a typed array.
function IsTypedArrayEnsuringArrayBuffer(arg) {
    if (IsObject(arg) && IsTypedArray(arg)) {
        GetAttachedArrayBuffer(arg);
        return true;
    }

    callFunction(CallTypedArrayMethodIfWrapped, arg, "GetAttachedArrayBufferMethod");
    return false;
}

// ES2019 draft rev 85ce767c86a1a8ed719fe97e978028bff819d1f2
// 7.3.20 SpeciesConstructor ( O, defaultConstructor )
//
// SpeciesConstructor function optimized for TypedArrays to avoid calling
// ConstructorForTypedArray, a non-inlineable runtime function, in the normal
// case.
function TypedArraySpeciesConstructor(obj) {
    // Step 1.
    assert(IsObject(obj), "not passed an object");

    // Step 2.
    var ctor = obj.constructor;

    // Step 3.
    if (ctor === undefined)
        return ConstructorForTypedArray(obj);

    // Step 4.
    if (!IsObject(ctor))
        ThrowTypeError(JSMSG_OBJECT_REQUIRED, "object's 'constructor' property");

    // Steps 5.
    var s = ctor[GetBuiltinSymbol("species")];

    // Step 6.
    if (s === undefined || s === null)
        return ConstructorForTypedArray(obj);

    // Step 7.
    if (IsConstructor(s))
        return s;

    // Step 8.
    ThrowTypeError(JSMSG_NOT_CONSTRUCTOR, "@@species property of object's constructor");
}

// ES2017 draft rev 6859bb9ccaea9c6ede81d71e5320e3833b92cb3e
// 22.2.3.5.1 Runtime Semantics: ValidateTypedArray ( O )
function ValidateTypedArray(obj) {
    if (IsObject(obj)) {
        /* Steps 3-5 (non-wrapped typed arrays). */
        if (IsTypedArray(obj)) {
            // GetAttachedArrayBuffer throws for detached array buffers.
            GetAttachedArrayBuffer(obj);
            return true;
        }

        /* Steps 3-5 (wrapped typed arrays). */
        if (IsPossiblyWrappedTypedArray(obj)) {
            if (PossiblyWrappedTypedArrayHasDetachedBuffer(obj))
                ThrowTypeError(JSMSG_TYPED_ARRAY_DETACHED);
            return false;
        }
    }

    /* Steps 1-2. */
    ThrowTypeError(JSMSG_NON_TYPED_ARRAY_RETURNED);
}

// ES2017 draft rev 6859bb9ccaea9c6ede81d71e5320e3833b92cb3e
// 22.2.4.6 TypedArrayCreate ( constructor, argumentList )
function TypedArrayCreateWithLength(constructor, length) {
    // Step 1.
    var newTypedArray = new constructor(length);

    // Step 2.
    var isTypedArray = ValidateTypedArray(newTypedArray);

    // Step 3.
    var len;
    if (isTypedArray) {
        len = TypedArrayLength(newTypedArray);
    } else {
        len = callFunction(CallTypedArrayMethodIfWrapped, newTypedArray,
                           "TypedArrayLengthMethod");
    }

    if (len < length)
        ThrowTypeError(JSMSG_SHORT_TYPED_ARRAY_RETURNED, length, len);

    // Step 4.
    return newTypedArray;
}

// ES2017 draft rev 6859bb9ccaea9c6ede81d71e5320e3833b92cb3e
// 22.2.4.6 TypedArrayCreate ( constructor, argumentList )
function TypedArrayCreateWithBuffer(constructor, buffer, byteOffset, length) {
    // Step 1.
    var newTypedArray = new constructor(buffer, byteOffset, length);

    // Step 2.
    ValidateTypedArray(newTypedArray);

    // Step 3 (not applicable).

    // Step 4.
    return newTypedArray;
}

// ES2017 draft rev 6859bb9ccaea9c6ede81d71e5320e3833b92cb3e
// 22.2.4.7 TypedArraySpeciesCreate ( exemplar, argumentList )
function TypedArraySpeciesCreateWithLength(exemplar, length) {
    // Step 1 (omitted).

    // Steps 2-3.
    var C = TypedArraySpeciesConstructor(exemplar);

    // Step 4.
    return TypedArrayCreateWithLength(C, length);
}

// ES2017 draft rev 6859bb9ccaea9c6ede81d71e5320e3833b92cb3e
// 22.2.4.7 TypedArraySpeciesCreate ( exemplar, argumentList )
function TypedArraySpeciesCreateWithBuffer(exemplar, buffer, byteOffset, length) {
    // Step 1 (omitted).

    // Steps 2-3.
    var C = TypedArraySpeciesConstructor(exemplar);

    // Step 4.
    return TypedArrayCreateWithBuffer(C, buffer, byteOffset, length);
}

// ES6 draft rev30 (2014/12/24) 22.2.3.6 %TypedArray%.prototype.entries()
function TypedArrayEntries() {
    // Step 1.
    var O = this;

    // We need to be a bit careful here, because in the Xray case we want to
    // create the iterator in our current compartment.
    //
    // Before doing that, though, we want to check that we have a typed array
    // and it does not have a detached array buffer.  We do the latter by just
    // calling GetAttachedArrayBuffer() and letting it throw if there isn't one.
    // In the case when we're not sure we have a typed array (e.g. we might have
    // a cross-compartment wrapper for one), we can go ahead and call
    // GetAttachedArrayBuffer via IsTypedArrayEnsuringArrayBuffer; that will
    // throw if we're not actually a wrapped typed array, or if we have a
    // detached array buffer.

    // Step 2-6.
    IsTypedArrayEnsuringArrayBuffer(O);

    // Step 7.
    return CreateArrayIterator(O, ITEM_KIND_KEY_AND_VALUE);
}

// ES2021 draft rev 190d474c3d8728653fbf8a5a37db1de34b9c1472
// Plus <https://github.com/tc39/ecma262/pull/2221>
// 22.2.3.7 %TypedArray%.prototype.every ( callbackfn [ , thisArg ] )
function TypedArrayEvery(callbackfn/*, thisArg*/) {
    // Step 1.
    var O = this;

    // Step 2.
    var isTypedArray = IsTypedArrayEnsuringArrayBuffer(O);

    // If we got here, `this` is either a typed array or a wrapper for one.

    // Step 3.
    var len;
    if (isTypedArray)
        len = TypedArrayLength(O);
    else
        len = callFunction(CallTypedArrayMethodIfWrapped, O, "TypedArrayLengthMethod");

    // Step 4.
    if (arguments.length === 0)
        ThrowTypeError(JSMSG_MISSING_FUN_ARG, 0, "%TypedArray%.prototype.every");
    if (!IsCallable(callbackfn))
        ThrowTypeError(JSMSG_NOT_FUNCTION, DecompileArg(0, callbackfn));

    var thisArg = arguments.length > 1 ? arguments[1] : void 0;

    // Steps 5-6.
    for (var k = 0; k < len; k++) {
        // Steps 6.b-d.
        var kValue = O[k];

        // Step 6.c.
        var testResult = callContentFunction(callbackfn, thisArg, kValue, k, O);

        // Step 6.d.
        if (!testResult)
            return false;
    }

    // Step 7.
    return true;
}

// ES2018 draft rev ad2d1c60c5dc42a806696d4b58b4dca42d1f7dd4
// 22.2.3.8 %TypedArray%.prototype.fill ( value [ , start [ , end ] ] )
function TypedArrayFill(value, start = 0, end = undefined) {
    // This function is not generic.
    if (!IsObject(this) || !IsTypedArray(this)) {
        return callFunction(CallTypedArrayMethodIfWrapped, this, value, start, end,
                            "TypedArrayFill");
    }

    // Step 1.
    var O = this;

    // Step 2.
    var buffer = GetAttachedArrayBuffer(this);

    // Step 3.
    var len = TypedArrayLength(O);

    // Step 4.
    var kind = GetTypedArrayKind(O);
    if (kind === TYPEDARRAY_KIND_BIGINT64 || kind === TYPEDARRAY_KIND_BIGUINT64) {
        value = ToBigInt(value);
    } else {
        value = ToNumber(value);
    }

    // Step 5.
    var relativeStart = ToInteger(start);

    // Step 6.
    var k = relativeStart < 0
            ? std_Math_max(len + relativeStart, 0)
            : std_Math_min(relativeStart, len);

    // Step 7.
    var relativeEnd = end === undefined ? len : ToInteger(end);

    // Step 8.
    var final = relativeEnd < 0
                ? std_Math_max(len + relativeEnd, 0)
                : std_Math_min(relativeEnd, len);

    // Step 9.
    if (buffer === null) {
        // A typed array previously using inline storage may acquire a
        // buffer, so we must check with the source.
        buffer = ViewedArrayBufferIfReified(O);
    }

    if (IsDetachedBuffer(buffer))
        ThrowTypeError(JSMSG_TYPED_ARRAY_DETACHED);

    // Step 10.
    for (; k < final; k++) {
        O[k] = value;
    }

    // Step 11.
    return O;
}

// ES2017 draft rev 6859bb9ccaea9c6ede81d71e5320e3833b92cb3e
// %TypedArray%.prototype.filter ( callbackfn [ , thisArg ] )
function TypedArrayFilter(callbackfn/*, thisArg*/) {
    // Step 1.
    var O = this;

    // Step 2.
    // This function is not generic.
    // We want to make sure that we have an attached buffer, per spec prose.
    var isTypedArray = IsTypedArrayEnsuringArrayBuffer(O);

    // If we got here, `this` is either a typed array or a wrapper for one.

    // Step 3.
    var len;
    if (isTypedArray)
        len = TypedArrayLength(O);
    else
        len = callFunction(CallTypedArrayMethodIfWrapped, O, "TypedArrayLengthMethod");

    // Step 4.
    if (arguments.length === 0)
        ThrowTypeError(JSMSG_MISSING_FUN_ARG, 0, "%TypedArray%.prototype.filter");
    if (!IsCallable(callbackfn))
        ThrowTypeError(JSMSG_NOT_FUNCTION, DecompileArg(0, callbackfn));

    // Step 5.
    var T = arguments.length > 1 ? arguments[1] : void 0;

    // Step 6.
    var kept = new_List();

    // Step 8.
    var captured = 0;

    // Steps 7 and 9.e.
    for (var k = 0; k < len; k++) {
        // Steps 9.a-b.
        var kValue = O[k];

        // Step 9.c.
        var selected = ToBoolean(callContentFunction(callbackfn, T, kValue, k, O));

        // Step 9.d.
        if (selected) {
            // Steps 9.d.i-ii.
            kept[captured++] = kValue;
        }
    }

    // Step 10.
    var A = TypedArraySpeciesCreateWithLength(O, captured);

    // Steps 11 and 12.b.
    for (var n = 0; n < captured; n++) {
        // Step 12.a.
        A[n] = kept[n];
    }

    // Step 13.
    return A;
}

// ES2021 draft rev 190d474c3d8728653fbf8a5a37db1de34b9c1472
// Plus <https://github.com/tc39/ecma262/pull/2221>
// 22.2.3.10 %TypedArray%.prototype.find ( predicate [ , thisArg ] )
function TypedArrayFind(predicate/*, thisArg*/) {
    // Step 1.
    var O = this;

    // Step 2.
    var isTypedArray = IsTypedArrayEnsuringArrayBuffer(O);

    // If we got here, `this` is either a typed array or a wrapper for one.

    // Step 3.
    var len;
    if (isTypedArray)
        len = TypedArrayLength(O);
    else
        len = callFunction(CallTypedArrayMethodIfWrapped, O, "TypedArrayLengthMethod");

    // Step 4.
    if (arguments.length === 0)
        ThrowTypeError(JSMSG_MISSING_FUN_ARG, 0, "%TypedArray%.prototype.find");
    if (!IsCallable(predicate))
        ThrowTypeError(JSMSG_NOT_FUNCTION, DecompileArg(0, predicate));

    var thisArg = arguments.length > 1 ? arguments[1] : void 0;

    // Steps 5-6.
    for (var k = 0; k < len; k++) {
        // Steps 6.a-b.
        var kValue = O[k];

        // Steps 6.c-d.
        if (callContentFunction(predicate, thisArg, kValue, k, O))
            return kValue;
    }

    // Step 7.
    return undefined;
}

// ES2021 draft rev 190d474c3d8728653fbf8a5a37db1de34b9c1472
// Plus <https://github.com/tc39/ecma262/pull/2221>
// 22.2.3.11 %TypedArray%.prototype.findIndex ( predicate [ , thisArg ] )
function TypedArrayFindIndex(predicate/*, thisArg*/) {
    // Step 1.
    var O = this;

    // Step 2.
    var isTypedArray = IsTypedArrayEnsuringArrayBuffer(O);

    // If we got here, `this` is either a typed array or a wrapper for one.

    // Step 3.
    var len;
    if (isTypedArray)
        len = TypedArrayLength(O);
    else
        len = callFunction(CallTypedArrayMethodIfWrapped, O, "TypedArrayLengthMethod");

    // Step 4.
    if (arguments.length === 0)
        ThrowTypeError(JSMSG_MISSING_FUN_ARG, 0, "%TypedArray%.prototype.findIndex");
    if (!IsCallable(predicate))
        ThrowTypeError(JSMSG_NOT_FUNCTION, DecompileArg(0, predicate));

    var thisArg = arguments.length > 1 ? arguments[1] : void 0;

    // Steps 5-6.
    for (var k = 0; k < len; k++) {
        // Steps 6.a-f.
        if (callContentFunction(predicate, thisArg, O[k], k, O))
            return k;
    }

    // Step 7.
    return -1;
}

// ES2021 draft rev 190d474c3d8728653fbf8a5a37db1de34b9c1472
// Plus <https://github.com/tc39/ecma262/pull/2221>
// 22.2.3.12 %TypedArray%.prototype.forEach ( callbackfn [ , thisArg ] )
function TypedArrayForEach(callbackfn/*, thisArg*/) {
    // Step 1.
    var O = this;

    // Step 2.
    var isTypedArray = IsTypedArrayEnsuringArrayBuffer(O);

    // If we got here, `this` is either a typed array or a wrapper for one.

    // Step 3.
    var len;
    if (isTypedArray)
        len = TypedArrayLength(O);
    else
        len = callFunction(CallTypedArrayMethodIfWrapped, O, "TypedArrayLengthMethod");

    // Step 4.
    if (arguments.length === 0)
        ThrowTypeError(JSMSG_MISSING_FUN_ARG, 0, "TypedArray.prototype.forEach");
    if (!IsCallable(callbackfn))
        ThrowTypeError(JSMSG_NOT_FUNCTION, DecompileArg(0, callbackfn));

    var thisArg = arguments.length > 1 ? arguments[1] : void 0;

    // Steps 5-6.
    for (var k = 0; k < len; k++) {
        // Steps 6.a-c.
        callContentFunction(callbackfn, thisArg, O[k], k, O);
    }

    // Step 7.
    return undefined;
}

// ES2021 draft rev 190d474c3d8728653fbf8a5a37db1de34b9c1472
// Plus <https://github.com/tc39/ecma262/pull/2221>
// 22.2.3.14 %TypedArray%.prototype.indexOf ( searchElement [ , fromIndex ] )
function TypedArrayIndexOf(searchElement, fromIndex = 0) {
    // Step 2.
    if (!IsObject(this) || !IsTypedArray(this)) {
        return callFunction(CallTypedArrayMethodIfWrapped, this, searchElement, fromIndex,
                            "TypedArrayIndexOf");
    }

    GetAttachedArrayBuffer(this);

    // Step 1.
    var O = this;

    // Step 3.
    var len = TypedArrayLength(O);

    // Step 4.
    if (len === 0)
        return -1;

    // Step 5.
    var n = ToInteger(fromIndex);

    // Step 6.
    assert(fromIndex !== undefined || n === 0, "ToInteger(undefined) is zero");

    // Reload O.[[ArrayLength]] in case ToInteger() detached the ArrayBuffer.
    // This let's us avoid executing the HasProperty operation in step 11.a.
    len = TypedArrayLength(O);

    assert(len === 0 || !IsDetachedBuffer(ViewedArrayBufferIfReified(O)),
           "TypedArrays with detached buffers have a length of zero");

    // Step 7.
    if (n >= len)
        return -1;

    // Steps 7-10.
    // Steps 7-8 are handled implicitly.
    var k;
    if (n >= 0) {
        // Step 9.a.
        k = n;
    } else {
        // Step 10.a.
        k = len + n;

        // Step 10.b.
        if (k < 0)
            k = 0;
    }

    // Step 11.
    for (; k < len; k++) {
        // Step 11.a (not necessary in our implementation).
        assert(k in O, "unexpected missing element");

        // Steps 11.b.i-iii.
        if (O[k] === searchElement)
            return k;
    }

    // Step 12.
    return -1;
}

// ES2021 draft rev 190d474c3d8728653fbf8a5a37db1de34b9c1472
// Plus <https://github.com/tc39/ecma262/pull/2221>
// 22.2.3.15 %TypedArray%.prototype.join ( separator )
function TypedArrayJoin(separator) {
    // Step 2.
    if (!IsObject(this) || !IsTypedArray(this)) {
        return callFunction(CallTypedArrayMethodIfWrapped, this, separator, "TypedArrayJoin");
    }

    GetAttachedArrayBuffer(this);

    // Step 1.
    var O = this;

    // Step 3.
    var len = TypedArrayLength(O);

    // Steps 4-5.
    var sep = separator === undefined ? "," : ToString(separator);

    // Steps 6 and 9.
    if (len === 0)
        return "";

    // ToString() might have detached the underlying ArrayBuffer. To avoid
    // checking for this condition when looping in step 8.c, do it once here.
    if (TypedArrayLength(O) === 0) {
        assert(IsDetachedBuffer(ViewedArrayBufferIfReified(O)),
               "TypedArrays with detached buffers have a length of zero");

        return callFunction(String_repeat, ",", len - 1);
    }

    assert(!IsDetachedBuffer(ViewedArrayBufferIfReified(O)),
           "TypedArrays with detached buffers have a length of zero");

    var element0 = O[0];

    // Omit the 'if' clause in step 8.c, since typed arrays can't have undefined or null elements.
    assert(element0 !== undefined, "unexpected undefined element");

    // Step 6.
    var R = ToString(element0);

    // Steps 7-8.
    for (var k = 1; k < len; k++) {
        // Step 8.b.
        var element = O[k];

        // Omit the 'if' clause in step 8.c, since typed arrays can't have undefined or null elements.
        assert(element !== undefined, "unexpected undefined element");

        // Steps 8.a and 8.c-d.
        R += sep + ToString(element);
    }

    // Step 9.
    return R;
}

// ES6 draft (2016/1/11) 22.2.3.15 %TypedArray%.prototype.keys()
function TypedArrayKeys() {
    // Step 1.
    var O = this;

    // See the big comment in TypedArrayEntries for what we're doing here.

    // Step 2.
    IsTypedArrayEnsuringArrayBuffer(O);

    // Step 3.
    return CreateArrayIterator(O, ITEM_KIND_KEY);
}

// ES2021 draft rev 190d474c3d8728653fbf8a5a37db1de34b9c1472
// Plus <https://github.com/tc39/ecma262/pull/2221>
// 22.2.3.17 %TypedArray%.prototype.lastIndexOf ( searchElement [ , fromIndex ] )
function TypedArrayLastIndexOf(searchElement/*, fromIndex*/) {
    // Step 2.
    if (!IsObject(this) || !IsTypedArray(this)) {
        if (arguments.length > 1) {
            return callFunction(CallTypedArrayMethodIfWrapped, this, searchElement, arguments[1],
                                "TypedArrayLastIndexOf");
        }
        return callFunction(CallTypedArrayMethodIfWrapped, this, searchElement,
                            "TypedArrayLastIndexOf");
    }

    GetAttachedArrayBuffer(this);

    // Step 1.
    var O = this;

    // Step 3.
    var len = TypedArrayLength(O);

    // Step 4.
    if (len === 0)
        return -1;

    // Step 5.
    var n = arguments.length > 1 ? ToInteger(arguments[1]) : len - 1;

    // Reload O.[[ArrayLength]] in case ToInteger() detached the ArrayBuffer.
    // This let's us avoid executing the HasProperty operation in step 9.a.
    len = TypedArrayLength(O);

    assert(len === 0 || !IsDetachedBuffer(ViewedArrayBufferIfReified(O)),
           "TypedArrays with detached buffers have a length of zero");

    // Steps 6-8.
    var k = n >= 0 ? std_Math_min(n, len - 1) : len + n;

    // Step 9.
    for (; k >= 0; k--) {
        // Step 9.a (not necessary in our implementation).
        assert(k in O, "unexpected missing element");

        // Steps 9.b.i-iii.
        if (O[k] === searchElement)
            return k;
    }

    // Step 10.
    return -1;
}

// ES2017 draft rev 6859bb9ccaea9c6ede81d71e5320e3833b92cb3e
// 22.2.3.19 %TypedArray%.prototype.map ( callbackfn [ , thisArg ] )
function TypedArrayMap(callbackfn/*, thisArg*/) {
    // Step 1.
    var O = this;

    // Step 2.
    // This function is not generic.
    // We want to make sure that we have an attached buffer, per spec prose.
    var isTypedArray = IsTypedArrayEnsuringArrayBuffer(O);

    // If we got here, `this` is either a typed array or a wrapper for one.

    // Step 3.
    var len;
    if (isTypedArray)
        len = TypedArrayLength(O);
    else
        len = callFunction(CallTypedArrayMethodIfWrapped, O, "TypedArrayLengthMethod");

    // Step 4.
    if (arguments.length === 0)
        ThrowTypeError(JSMSG_MISSING_FUN_ARG, 0, "%TypedArray%.prototype.map");
    if (!IsCallable(callbackfn))
        ThrowTypeError(JSMSG_NOT_FUNCTION, DecompileArg(0, callbackfn));

    // Step 5.
    var T = arguments.length > 1 ? arguments[1] : void 0;

    // Step 6.
    var A = TypedArraySpeciesCreateWithLength(O, len);

    // Steps 7, 8.a (implicit) and 8.e.
    for (var k = 0; k < len; k++) {
        // Steps 8.b-c.
        var mappedValue = callContentFunction(callbackfn, T, O[k], k, O);

        // Steps 8.d.
        A[k] = mappedValue;
    }

    // Step 9.
    return A;
}

// ES2021 draft rev 190d474c3d8728653fbf8a5a37db1de34b9c1472
// Plus <https://github.com/tc39/ecma262/pull/2221>
// 22.2.3.20 %TypedArray%.prototype.reduce ( callbackfn [ , initialValue ] )
function TypedArrayReduce(callbackfn/*, initialValue*/) {
    // Step 1.
    var O = this;

    // Step 2.
    var isTypedArray = IsTypedArrayEnsuringArrayBuffer(O);

    // If we got here, `this` is either a typed array or a wrapper for one.

    // Step 3.
    var len;
    if (isTypedArray)
        len = TypedArrayLength(O);
    else
        len = callFunction(CallTypedArrayMethodIfWrapped, O, "TypedArrayLengthMethod");

    // Step 4.
    if (arguments.length === 0)
        ThrowTypeError(JSMSG_MISSING_FUN_ARG, 0, "%TypedArray%.prototype.reduce");
    if (!IsCallable(callbackfn))
        ThrowTypeError(JSMSG_NOT_FUNCTION, DecompileArg(0, callbackfn));

    // Step 5.
    if (len === 0 && arguments.length === 1)
        ThrowTypeError(JSMSG_EMPTY_ARRAY_REDUCE);

    // Step 6.
    var k = 0;

    // Steps 7-9.
    var accumulator = arguments.length > 1 ? arguments[1] : O[k++];

    // Step 10.
    for (; k < len; k++) {
        accumulator = callContentFunction(callbackfn, undefined, accumulator, O[k], k, O);
    }

    // Step 11.
    return accumulator;
}

// ES2021 draft rev 190d474c3d8728653fbf8a5a37db1de34b9c1472
// Plus <https://github.com/tc39/ecma262/pull/2221>
// 22.2.3.21 %TypedArray%.prototype.reduceRight ( callbackfn [ , initialValue ] )
function TypedArrayReduceRight(callbackfn/*, initialValue*/) {
    // Step 1.
    var O = this;

    // Step 2.
    var isTypedArray = IsTypedArrayEnsuringArrayBuffer(O);

    // If we got here, `this` is either a typed array or a wrapper for one.

    // Step 3.
    var len;
    if (isTypedArray)
        len = TypedArrayLength(O);
    else
        len = callFunction(CallTypedArrayMethodIfWrapped, O, "TypedArrayLengthMethod");

    // Step 4.
    if (arguments.length === 0)
        ThrowTypeError(JSMSG_MISSING_FUN_ARG, 0, "%TypedArray%.prototype.reduceRight");
    if (!IsCallable(callbackfn))
        ThrowTypeError(JSMSG_NOT_FUNCTION, DecompileArg(0, callbackfn));

    // Step 5.
    if (len === 0 && arguments.length === 1)
        ThrowTypeError(JSMSG_EMPTY_ARRAY_REDUCE);

    // Step 6.
    var k = len - 1;

    // Steps 7-9.
    var accumulator = arguments.length > 1 ? arguments[1] : O[k--];

    // Step 10.
    for (; k >= 0; k--) {
        accumulator = callContentFunction(callbackfn, undefined, accumulator, O[k], k, O);
    }

    // Step 11.
    return accumulator;
}

// ES2021 draft rev 190d474c3d8728653fbf8a5a37db1de34b9c1472
// Plus <https://github.com/tc39/ecma262/pull/2221>
// 22.2.3.22 %TypedArray%.prototype.reverse ( )
function TypedArrayReverse() {
    // Step 2.
    if (!IsObject(this) || !IsTypedArray(this)) {
        return callFunction(CallTypedArrayMethodIfWrapped, this, "TypedArrayReverse");
    }

    GetAttachedArrayBuffer(this);

    // Step 1.
    var O = this;

    // Step 3.
    var len = TypedArrayLength(O);

    // Step 4.
    var middle = std_Math_floor(len / 2);

    // Steps 5-6.
    for (var lower = 0; lower !== middle; lower++) {
        // Step 6.a.
        var upper = len - lower - 1;

        // Step 6.d.
        var lowerValue = O[lower];

        // Step 6.e.
        var upperValue = O[upper];

        // Steps 6.f-g.
        O[lower] = upperValue;
        O[upper] = lowerValue;
    }

    // Step 7.
    return O;
}

// ES2017 draft rev 6859bb9ccaea9c6ede81d71e5320e3833b92cb3e
// 22.2.3.24 %TypedArray%.prototype.slice ( start, end )
function TypedArraySlice(start, end) {
    // Step 1.
    var O = this;

    // Step 2.
    if (!IsObject(O) || !IsTypedArray(O)) {
        return callFunction(CallTypedArrayMethodIfWrapped, O, start, end, "TypedArraySlice");
    }

    var buffer = GetAttachedArrayBuffer(O);

    // Step 3.
    var len = TypedArrayLength(O);

    // Step 4.
    var relativeStart = ToInteger(start);

    // Step 5.
    var k = relativeStart < 0
            ? std_Math_max(len + relativeStart, 0)
            : std_Math_min(relativeStart, len);

    // Step 6.
    var relativeEnd = end === undefined ? len : ToInteger(end);

    // Step 7.
    var final = relativeEnd < 0
                ? std_Math_max(len + relativeEnd, 0)
                : std_Math_min(relativeEnd, len);

    // Step 8.
    var count = std_Math_max(final - k, 0);

    // Step 9.
    var A = TypedArraySpeciesCreateWithLength(O, count);

    // Steps 14-15.
    if (count > 0) {
        // Steps 14.b.ii, 15.b.
        if (buffer === null) {
            // A typed array previously using inline storage may acquire a
            // buffer, so we must check with the source.
            buffer = ViewedArrayBufferIfReified(O);
        }

        if (IsDetachedBuffer(buffer))
            ThrowTypeError(JSMSG_TYPED_ARRAY_DETACHED);

        // Steps 10-13, 15.
        var sliced = TypedArrayBitwiseSlice(O, A, k, count);

        // Step 14.
        if (!sliced) {
            // Step 14.a.
            var n = 0;

            // Step 14.b.
            while (k < final) {
                // Steps 14.b.i-v.
                A[n++] = O[k++];
            }
        }
    }

    // Step 16.
    return A;
}

// ES2021 draft rev 190d474c3d8728653fbf8a5a37db1de34b9c1472
// Plus <https://github.com/tc39/ecma262/pull/2221>
// 22.2.3.25 %TypedArray%.prototype.some ( callbackfn [ , thisArg ] )
function TypedArraySome(callbackfn/*, thisArg*/) {
    // Step 1.
    var O = this;

    // Step 2.
    var isTypedArray = IsTypedArrayEnsuringArrayBuffer(O);

    // If we got here, `this` is either a typed array or a wrapper for one.

    // Step 3.
    var len;
    if (isTypedArray)
        len = TypedArrayLength(O);
    else
        len = callFunction(CallTypedArrayMethodIfWrapped, O, "TypedArrayLengthMethod");

    // Step 4.
    if (arguments.length === 0)
        ThrowTypeError(JSMSG_MISSING_FUN_ARG, 0, "%TypedArray%.prototype.some");
    if (!IsCallable(callbackfn))
        ThrowTypeError(JSMSG_NOT_FUNCTION, DecompileArg(0, callbackfn));

    var thisArg = arguments.length > 1 ? arguments[1] : void 0;

    // Steps 5-6.
    for (var k = 0; k < len; k++) {
        // Steps 6.a-b.
        var kValue = O[k];

        // Step 6.c.
        var testResult = callContentFunction(callbackfn, thisArg, kValue, k, O);

        // Step 6.d.
        if (testResult)
            return true;
    }

    // Step 7.
    return false;
}

// ES2017 draft rev 6859bb9ccaea9c6ede81d71e5320e3833b92cb3e
// 22.2.3.26 TypedArray SortCompare abstract operation
// Cases are ordered according to likelihood of occurrence
// as opposed to the ordering in the spec.
function TypedArrayCompare(x, y) {
    // Step 1.
    assert(typeof x === "number" && typeof y === "number",
           "x and y are not numbers.");

    // Step 2 (Implemented in TypedArraySort).

    // Step 6.
    if (x < y)
        return -1;

    // Step 7.
    if (x > y)
        return 1;

    // Steps 8-9.
    if (x === 0 && y === 0)
        return ((1 / x) > 0 ? 1 : 0) - ((1 / y) > 0 ? 1 : 0);

    // Steps 3-4.
    if (Number_isNaN(x))
        return Number_isNaN(y) ? 0 : 1;

    // Steps 5, 10.
    return Number_isNaN(y) ? -1 : 0;
}

// TypedArray SortCompare specialization for integer values.
function TypedArrayCompareInt(x, y) {
    // Step 1.
    assert(typeof x === "number" && typeof y === "number",
           "x and y are not numbers.");
    assert((x === (x | 0) || x === (x >>> 0)) && (y === (y | 0) || y === (y >>> 0)),
           "x and y are not int32/uint32 numbers.");

    // Step 2 (Implemented in TypedArraySort).

    // Steps 6-7.
    var diff = x - y;
    if (diff)
        return diff;

    // Steps 3-5, 8-9 (Not applicable when sorting integer values).

    // Step 10.
    return 0;
}

// https://tc39.github.io/proposal-bigint/#sec-%typedarray%.prototype.sort
// TypedArray SortCompare specialization for BigInt values.
function TypedArrayCompareBigInt(x, y) {
    // Step 1.
    assert(typeof x === "bigint" && typeof y === "bigint",
           "x and y are not BigInts.");

    // Step 2 (Implemented in TypedArraySort).

    // Step 6.
    if (x < y)
        return -1;

    // Step 7.
    if (x > y)
        return 1;

    // Steps 3-5, 8-9 (Not applicable when sorting BigInt values).

    // Step 10.
    return 0;
}

// ES2019 draft rev 8a16cb8d18660a1106faae693f0f39b9f1a30748
// 22.2.3.26 %TypedArray%.prototype.sort ( comparefn )
function TypedArraySort(comparefn) {
    // This function is not generic.

    // Step 1.
    if (comparefn !== undefined) {
        if (!IsCallable(comparefn))
            ThrowTypeError(JSMSG_NOT_FUNCTION, DecompileArg(0, comparefn));
    }

    // Step 2.
    var obj = this;

    // Step 3.
    var isTypedArray = IsObject(obj) && IsTypedArray(obj);

    var buffer;
    if (isTypedArray) {
        buffer = GetAttachedArrayBuffer(obj);
    } else {
        buffer = callFunction(CallTypedArrayMethodIfWrapped, obj, "GetAttachedArrayBufferMethod");
    }

    // Step 4.
    var len;
    if (isTypedArray) {
        len = TypedArrayLength(obj);
    } else {
        len = callFunction(CallTypedArrayMethodIfWrapped, obj, "TypedArrayLengthMethod");
    }

    // Arrays with less than two elements remain unchanged when sorted.
    if (len <= 1)
        return obj;

    if (comparefn === undefined) {
        var kind = GetTypedArrayKind(obj);
        switch (kind) {
          case TYPEDARRAY_KIND_UINT8:
          case TYPEDARRAY_KIND_UINT8CLAMPED:
            return CountingSort(obj, len, false /* signed */, TypedArrayCompareInt);
          case TYPEDARRAY_KIND_INT8:
            return CountingSort(obj, len, true /* signed */, TypedArrayCompareInt);
          case TYPEDARRAY_KIND_UINT16:
            return RadixSort(obj, len, buffer,
                             2 /* nbytes */, false /* signed */, false /* floating */,
                             TypedArrayCompareInt);
          case TYPEDARRAY_KIND_INT16:
            return RadixSort(obj, len, buffer,
                             2 /* nbytes */, true /* signed */, false /* floating */,
                             TypedArrayCompareInt);
          case TYPEDARRAY_KIND_UINT32:
            return RadixSort(obj, len, buffer,
                             4 /* nbytes */, false /* signed */, false /* floating */,
                             TypedArrayCompareInt);
          case TYPEDARRAY_KIND_INT32:
            return RadixSort(obj, len, buffer,
                             4 /* nbytes */, true /* signed */, false /* floating */,
                             TypedArrayCompareInt);
          case TYPEDARRAY_KIND_BIGINT64:
          case TYPEDARRAY_KIND_BIGUINT64:
            return QuickSort(obj, len, TypedArrayCompareBigInt);
          case TYPEDARRAY_KIND_FLOAT32:
            return RadixSort(obj, len, buffer,
                             4 /* nbytes */, true /* signed */, true /* floating */,
                             TypedArrayCompare);
          case TYPEDARRAY_KIND_FLOAT64:
          default:
            // Include |default| to ensure Ion marks this call as the
            // last instruction in the if-statement.
            assert(kind === TYPEDARRAY_KIND_FLOAT64, "unexpected typed array kind");
            return QuickSort(obj, len, TypedArrayCompare);
        }
    }

    // To satisfy step 2 from TypedArray SortCompare described in 22.2.3.26
    // the user supplied comparefn is wrapped.
    var wrappedCompareFn = function(x, y) {
        // Step a.
        var v = +comparefn(x, y);

        // Step b.
        var length;
        if (isTypedArray) {
            length = TypedArrayLength(obj);
        } else {
            length = callFunction(CallTypedArrayMethodIfWrapped, obj, "TypedArrayLengthMethod");
        }

        // It's faster for us to check the typed array's length than to check
        // for detached buffers.
        if (length === 0) {
            assert(PossiblyWrappedTypedArrayHasDetachedBuffer(obj),
                   "Length can only change from non-zero to zero when the buffer was detached");
            ThrowTypeError(JSMSG_TYPED_ARRAY_DETACHED);
        }

        // Step c.
        if (v !== v)
            return 0;

        // Step d.
        return v;
    };

    return MergeSortTypedArray(obj, len, wrappedCompareFn);
}

// ES2017 draft rev f8a9be8ea4bd97237d176907a1e3080dce20c68f
//   22.2.3.28 %TypedArray%.prototype.toLocaleString ([ reserved1 [ , reserved2 ] ])
// ES2017 Intl draft rev 78bbe7d1095f5ff3760ac4017ed366026e4cb276
//   13.4.1 Array.prototype.toLocaleString ([ locales [ , options ]])
function TypedArrayToLocaleString(locales = undefined, options = undefined) {
    // ValidateTypedArray, then step 1.
    var array = this;

    // This function is not generic.
    // We want to make sure that we have an attached buffer, per spec prose.
    var isTypedArray = IsTypedArrayEnsuringArrayBuffer(array);

    // If we got here, `this` is either a typed array or a wrapper for one.

    // Step 2.
    var len;
    if (isTypedArray)
        len = TypedArrayLength(array);
    else
        len = callFunction(CallTypedArrayMethodIfWrapped, array, "TypedArrayLengthMethod");

    // Step 4.
    if (len === 0)
        return "";

    // Step 5.
    var firstElement = array[0];

    // Steps 6-7.
    // Omit the 'if' clause in step 6, since typed arrays can't have undefined
    // or null elements.
#if JS_HAS_INTL_API
    var R = ToString(callContentFunction(firstElement.toLocaleString, firstElement, locales, options));
#else
    var R = ToString(callContentFunction(firstElement.toLocaleString, firstElement));
#endif

    // Step 3 (reordered).
    // We don't (yet?) implement locale-dependent separators.
    var separator = ",";

    // Steps 8-9.
    for (var k = 1; k < len; k++) {
        // Step 9.a.
        var S = R + separator;

        // Step 9.b.
        var nextElement = array[k];

        // Step 9.c *should* be unreachable: typed array elements are numbers.
        // But bug 1079853 means |nextElement| *could* be |undefined|, if the
        // previous iteration's step 9.d or step 7 detached |array|'s buffer.
        // Conveniently, if this happens, evaluating |nextElement.toLocaleString|
        // throws the required TypeError, and the only observable difference is
        // the error message. So despite bug 1079853, we can skip step 9.c.

        // Step 9.d.
#if JS_HAS_INTL_API
        R = ToString(callContentFunction(nextElement.toLocaleString, nextElement, locales, options));
#else
        R = ToString(callContentFunction(nextElement.toLocaleString, nextElement));
#endif

        // Step 9.e.
        R = S + R;
    }

    // Step 10.
    return R;
}

// ES2020 draft rev dc1e21c454bd316810be1c0e7af0131a2d7f38e9
// 22.2.3.27 %TypedArray%.prototype.subarray ( begin, end )
function TypedArraySubarray(begin, end) {
    // Step 1.
    var obj = this;

    // Steps 2-3.
    // This function is not generic.
    if (!IsObject(obj) || !IsTypedArray(obj)) {
        return callFunction(CallTypedArrayMethodIfWrapped, this, begin, end,
                            "TypedArraySubarray");
    }

    // Step 4.
    var buffer = ViewedArrayBufferIfReified(obj);
    if (buffer === null) {
        buffer = TypedArrayBuffer(obj);
    }

    // Step 5.
    var srcLength = TypedArrayLength(obj);

    // Step 13 (Reordered because otherwise it'd be observable that we reset
    // the byteOffset to zero when the underlying array buffer gets detached).
    var srcByteOffset = TypedArrayByteOffset(obj);

    // Step 6.
    var relativeBegin = ToInteger(begin);

    // Step 7.
    var beginIndex = relativeBegin < 0 ? std_Math_max(srcLength + relativeBegin, 0)
                                       : std_Math_min(relativeBegin, srcLength);

    // Step 8.
    var relativeEnd = end === undefined ? srcLength : ToInteger(end);

    // Step 9.
    var endIndex = relativeEnd < 0 ? std_Math_max(srcLength + relativeEnd, 0)
                                   : std_Math_min(relativeEnd, srcLength);

    // Step 10.
    var newLength = std_Math_max(endIndex - beginIndex, 0);

    // Steps 11-12.
    var elementSize = TypedArrayElementSize(obj);

    // Step 14.
    var beginByteOffset = srcByteOffset + (beginIndex * elementSize);

    // Steps 15-16.
    return TypedArraySpeciesCreateWithBuffer(obj, buffer, beginByteOffset, newLength);
}

// https://tc39.es/proposal-relative-indexing-method
// %TypedArray%.prototype.at ( index )
function TypedArrayAt(index) {
    // Step 1.
    var obj = this;

    // Step 2.
    // This function is not generic.
    if (!IsObject(obj) || !IsTypedArray(obj)) {
        return callFunction(CallTypedArrayMethodIfWrapped, obj, index,
                            "TypedArrayAt");
    }
    GetAttachedArrayBuffer(obj);

    // If we got here, `this` is either a typed array or a wrapper for one.

    // Step 3.
    var len = TypedArrayLength(obj);

    // Step 4.
    var relativeIndex = ToInteger(index);

    // Steps 5-6.
    var k;
    if (relativeIndex >= 0) {
        k = relativeIndex;
    } else {
        k = len + relativeIndex;
    }

    // Step 7.
    if (k < 0 || k >= len) {
        return undefined;
    }

    // Step 8.
    return obj[k];
}

// ES6 draft rev30 (2014/12/24) 22.2.3.30 %TypedArray%.prototype.values()
//
// Uncloned functions with `$` prefix are allocated as extended function
// to store the original name in `SetCanonicalName`.
function $TypedArrayValues() {
    // Step 1.
    var O = this;

    // See the big comment in TypedArrayEntries for what we're doing here.
    IsTypedArrayEnsuringArrayBuffer(O);

    // Step 7.
    return CreateArrayIterator(O, ITEM_KIND_VALUE);
}
SetCanonicalName($TypedArrayValues, "values");

// ES2021 draft rev 190d474c3d8728653fbf8a5a37db1de34b9c1472
// Plus <https://github.com/tc39/ecma262/pull/2221>
// 22.2.3.13 %TypedArray%.prototype.includes ( searchElement [ , fromIndex ] )
function TypedArrayIncludes(searchElement, fromIndex = 0) {
    // Step 2.
    if (!IsObject(this) || !IsTypedArray(this)) {
        return callFunction(CallTypedArrayMethodIfWrapped, this, searchElement,
                            fromIndex, "TypedArrayIncludes");
    }

    GetAttachedArrayBuffer(this);

    // Step 1.
    var O = this;

    // Step 3.
    var len = TypedArrayLength(O);

    // Step 4.
    if (len === 0)
        return false;

    // Step 5.
    var n = ToInteger(fromIndex);

    // Step 6.
    assert(fromIndex !== undefined || n === 0, "ToInteger(undefined) is zero");

    // Steps 7-10.
    // Steps 7-8 are handled implicitly.
    var k;
    if (n >= 0) {
        // Step 9.a
        k = n;
    } else {
        // Step 10.a.
        k = len + n;

        // Step 10.b.
        if (k < 0)
            k = 0;
    }

    // Step 11.
    while (k < len) {
        // Steps 11.a-b.
        if (SameValueZero(searchElement, O[k]))
            return true;

        // Step 11.c.
        k++;
    }

    // Step 12.
    return false;
}

// ES2017 draft rev 6859bb9ccaea9c6ede81d71e5320e3833b92cb3e
// 22.2.2.1 %TypedArray%.from ( source [ , mapfn [ , thisArg ] ] )
function TypedArrayStaticFrom(source, mapfn = undefined, thisArg = undefined) {
    // Step 1.
    var C = this;

    // Step 2.
    if (!IsConstructor(C))
        ThrowTypeError(JSMSG_NOT_CONSTRUCTOR, typeof C);

    // Step 3.
    var mapping;
    if (mapfn !== undefined) {
        // Step 3.a.
        if (!IsCallable(mapfn))
            ThrowTypeError(JSMSG_NOT_FUNCTION, DecompileArg(1, mapfn));

        // Step 3.b.
        mapping = true;
    } else {
        // Step 4.
        mapping = false;
    }

    // Step 5.
    var T = thisArg;

    // Step 6.
    // Inlined: GetMethod, steps 1-2.
    var usingIterator = source[GetBuiltinSymbol("iterator")];

    // Step 7.
    // Inlined: GetMethod, step 3.
    if (usingIterator !== undefined && usingIterator !== null) {
        // Inlined: GetMethod, step 4.
        if (!IsCallable(usingIterator))
            ThrowTypeError(JSMSG_NOT_ITERABLE, DecompileArg(0, source));

        // Try to take a fast path when there's no mapper function and the
        // constructor is a built-in TypedArray constructor.
        if (!mapping && IsTypedArrayConstructor(C) && IsObject(source)) {
            // The source is a TypedArray using the default iterator.
            if (usingIterator === $TypedArrayValues && IsTypedArray(source) &&
                ArrayIteratorPrototypeOptimizable())
            {
                // Step 7.a.
                // Omitted but we still need to throw if |source| was detached.
                GetAttachedArrayBuffer(source);

                // Step 7.b.
                var len = TypedArrayLength(source);

                // Step 7.c.
                var targetObj = new C(len);

                // Steps 7.d-f.
                for (var k = 0; k < len; k++) {
                    targetObj[k] = source[k];
                }

                // Step 7.g.
                return targetObj;
            }

            // The source is a packed array using the default iterator.
            if (usingIterator === $ArrayValues && IsPackedArray(source) &&
                ArrayIteratorPrototypeOptimizable())
            {
                // Steps 7.b-c.
                var targetObj = new C(source.length);

                // Steps 7.a, 7.d-f.
                TypedArrayInitFromPackedArray(targetObj, source);

                // Step 7.g.
                return targetObj;
            }
        }

        // Step 7.a.
        var values = IterableToList(source, usingIterator);

        // Step 7.b.
        var len = values.length;

        // Step 7.c.
        var targetObj = TypedArrayCreateWithLength(C, len);

        // Steps 7.d-e.
        for (var k = 0; k < len; k++) {
            // Step 7.e.ii.
            var kValue = values[k];

            // Steps 7.e.iii-iv.
            var mappedValue = mapping ? callContentFunction(mapfn, T, kValue, k) : kValue;

            // Step 7.e.v.
            targetObj[k] = mappedValue;
        }

        // Step 7.f.
        // Asserting that `values` is empty here would require removing them one by one from
        // the list's start in the loop above. That would introduce unacceptable overhead.
        // Additionally, the loop's logic is simple enough not to require the assert.

        // Step 7.g.
        return targetObj;
    }

    // Step 8 is an assertion: items is not an Iterator. Testing this is
    // literally the very last thing we did, so we don't assert here.

    // Step 9.
    var arrayLike = ToObject(source);

    // Step 10.
    var len = ToLength(arrayLike.length);

    // Step 11.
    var targetObj = TypedArrayCreateWithLength(C, len);

    // Steps 12-13.
    for (var k = 0; k < len; k++) {
        // Steps 13.a-b.
        var kValue = arrayLike[k];

        // Steps 13.c-d.
        var mappedValue = mapping ? callContentFunction(mapfn, T, kValue, k) : kValue;

        // Step 13.e.
        targetObj[k] = mappedValue;
    }

    // Step 14.
    return targetObj;
}

// ES2017 draft rev 6859bb9ccaea9c6ede81d71e5320e3833b92cb3e
// 22.2.2.2 %TypedArray%.of ( ...items )
function TypedArrayStaticOf(/*...items*/) {
    // Step 1.
    var len = arguments.length;

    // Step 2.
    var items = arguments;

    // Step 3.
    var C = this;

    // Step 4.
    if (!IsConstructor(C))
        ThrowTypeError(JSMSG_NOT_CONSTRUCTOR, typeof C);

    // Step 5.
    var newObj = TypedArrayCreateWithLength(C, len);

    // Steps 6-7.
    for (var k = 0; k < len; k++)
        newObj[k] = items[k];

    // Step 8.
    return newObj;
}

// ES 2016 draft Mar 25, 2016 22.2.2.4.
function $TypedArraySpecies() {
    // Step 1.
    return this;
}
SetCanonicalName($TypedArraySpecies, "get [Symbol.species]");

// ES2018 draft rev 0525bb33861c7f4e9850f8a222c89642947c4b9c
// 22.2.2.1.1 Runtime Semantics: IterableToList( items, method )
function IterableToList(items, method) {
    // Step 1 (Inlined GetIterator).

    // 7.4.1 GetIterator, step 1.
    assert(IsCallable(method), "method argument is a function");

    // 7.4.1 GetIterator, step 2.
    var iterator = callContentFunction(method, items);

    // 7.4.1 GetIterator, step 3.
    if (!IsObject(iterator))
        ThrowTypeError(JSMSG_GET_ITER_RETURNED_PRIMITIVE);

    // 7.4.1 GetIterator, step 4.
    var nextMethod = iterator.next;

    // Step 2.
    var values = [];

    // Steps 3-4.
    var i = 0;
    while (true) {
        // Step 4.a.
        var next = callContentFunction(nextMethod, iterator);
        if (!IsObject(next))
            ThrowTypeError(JSMSG_ITER_METHOD_RETURNED_PRIMITIVE, "next");

        // Step 4.b.
        if (next.done)
            break;
        DefineDataProperty(values, i++, next.value);
    }

    // Step 5.
    return values;
}

// ES2020 draft rev dc1e21c454bd316810be1c0e7af0131a2d7f38e9
// 24.1.4.3 ArrayBuffer.prototype.slice ( start, end )
function ArrayBufferSlice(start, end) {
    // Step 1.
    var O = this;

    // Steps 2-3,
    // This function is not generic.
    if (!IsObject(O) || (O = GuardToArrayBuffer(O)) === null) {
        return callFunction(CallArrayBufferMethodIfWrapped, this, start, end,
                            "ArrayBufferSlice");
    }

    // Step 4.
    if (IsDetachedBuffer(O))
        ThrowTypeError(JSMSG_TYPED_ARRAY_DETACHED);

    // Step 5.
    var len = ArrayBufferByteLength(O);

    // Step 6.
    var relativeStart = ToInteger(start);

    // Step 7.
    var first = relativeStart < 0 ? std_Math_max(len + relativeStart, 0)
                                  : std_Math_min(relativeStart, len);

    // Step 8.
    var relativeEnd = end === undefined ? len : ToInteger(end);

    // Step 9.
    var final = relativeEnd < 0 ? std_Math_max(len + relativeEnd, 0)
                                : std_Math_min(relativeEnd, len);

    // Step 10.
    var newLen = std_Math_max(final - first, 0);

    // Step 11
    var ctor = SpeciesConstructor(O, GetBuiltinConstructor("ArrayBuffer"));

    // Step 12.
    var new_ = new ctor(newLen);

    // Steps 13-15.
    var isWrapped = false;
    var newBuffer;
    if ((newBuffer = GuardToArrayBuffer(new_)) !== null) {
        // Step 15.
        if (IsDetachedBuffer(newBuffer))
            ThrowTypeError(JSMSG_TYPED_ARRAY_DETACHED);
    } else {
        newBuffer = new_;

        // Steps 13-14.
        if (!IsWrappedArrayBuffer(newBuffer))
            ThrowTypeError(JSMSG_NON_ARRAY_BUFFER_RETURNED);

        isWrapped = true;

        // Step 15.
        if (callFunction(CallArrayBufferMethodIfWrapped, newBuffer, "IsDetachedBufferThis"))
            ThrowTypeError(JSMSG_TYPED_ARRAY_DETACHED);
    }

    // Step 16.
    if (newBuffer === O)
        ThrowTypeError(JSMSG_SAME_ARRAY_BUFFER_RETURNED);

    // Step 17.
    var actualLen = PossiblyWrappedArrayBufferByteLength(newBuffer);
    if (actualLen < newLen)
        ThrowTypeError(JSMSG_SHORT_ARRAY_BUFFER_RETURNED, newLen, actualLen);

    // Steps 18-19.
    if (IsDetachedBuffer(O))
        ThrowTypeError(JSMSG_TYPED_ARRAY_DETACHED);

    // Steps 20-22.
    ArrayBufferCopyData(newBuffer, 0, O, first, newLen, isWrapped);

    // Step 23.
    return newBuffer;
}

function IsDetachedBufferThis() {
  return IsDetachedBuffer(this);
}

// ES 2016 draft Mar 25, 2016 24.1.3.3.
function $ArrayBufferSpecies() {
    // Step 1.
    return this;
}
SetCanonicalName($ArrayBufferSpecies, "get [Symbol.species]");

// Shared memory and atomics proposal (30 Oct 2016)
function $SharedArrayBufferSpecies() {
    // Step 1.
    return this;
}
SetCanonicalName($SharedArrayBufferSpecies, "get [Symbol.species]");

// ES2020 draft rev dc1e21c454bd316810be1c0e7af0131a2d7f38e9
// 24.2.4.3 SharedArrayBuffer.prototype.slice ( start, end )
function SharedArrayBufferSlice(start, end) {
    // Step 1.
    var O = this;

    // Steps 2-3.
    // This function is not generic.
    if (!IsObject(O) || (O = GuardToSharedArrayBuffer(O)) === null) {
        return callFunction(CallSharedArrayBufferMethodIfWrapped, this, start, end,
                            "SharedArrayBufferSlice");
    }

    // Step 4.
    var len = SharedArrayBufferByteLength(O);

    // Step 5.
    var relativeStart = ToInteger(start);

    // Step 6.
    var first = relativeStart < 0 ? std_Math_max(len + relativeStart, 0)
                                  : std_Math_min(relativeStart, len);

    // Step 7.
    var relativeEnd = end === undefined ? len : ToInteger(end);

    // Step 8.
    var final = relativeEnd < 0 ? std_Math_max(len + relativeEnd, 0)
                                : std_Math_min(relativeEnd, len);

    // Step 9.
    var newLen = std_Math_max(final - first, 0);

    // Step 10
    var ctor = SpeciesConstructor(O, GetBuiltinConstructor("SharedArrayBuffer"));

    // Step 11.
    var new_ = new ctor(newLen);

    // Steps 12-13.
    var isWrapped = false;
    var newObj;
    if ((newObj = GuardToSharedArrayBuffer(new_)) === null) {
        if (!IsWrappedSharedArrayBuffer(new_))
            ThrowTypeError(JSMSG_NON_SHARED_ARRAY_BUFFER_RETURNED);
        isWrapped = true;
        newObj = new_;
    }

    // Step 14.
    if (newObj === O || SharedArrayBuffersMemorySame(newObj, O))
        ThrowTypeError(JSMSG_SAME_SHARED_ARRAY_BUFFER_RETURNED);

    // Step 15.
    var actualLen = PossiblyWrappedSharedArrayBufferByteLength(newObj);
    if (actualLen < newLen)
        ThrowTypeError(JSMSG_SHORT_SHARED_ARRAY_BUFFER_RETURNED, newLen, actualLen);

    // Steps 16-18.
    SharedArrayBufferCopyData(newObj, 0, O, first, newLen, isWrapped);

    // Step 19.
    return newObj;
}

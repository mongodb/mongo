/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// ES6 draft rev30 (2014/12/24) 22.2.3.6 %TypedArray%.prototype.entries()
function TypedArrayEntries() {
    // Step 1.
    var O = this;

    // Step 2-3.
    if (!IsObject(O) || !IsTypedArray(O)) {
        return callFunction(CallTypedArrayMethodIfWrapped, O, "TypedArrayEntries");
    }

    // Step 4-6. Bug 1101256: detachment checks

    // Step 7.
    return CreateArrayIterator(O, ITEM_KIND_KEY_AND_VALUE);
}

// ES6 draft rev30 (2014/12/24) 22.2.3.7 %TypedArray%.prototype.every(callbackfn[, thisArg]).
function TypedArrayEvery(callbackfn, thisArg = undefined) {
    // This function is not generic.
    if (!IsObject(this) || !IsTypedArray(this)) {
        return callFunction(CallTypedArrayMethodIfWrapped, this, callbackfn, thisArg,
                            "TypedArrayEvery");
    }

    // Steps 1-2.
    var O = this;

    // Steps 3-5.
    var len = TypedArrayLength(O);

    // Step 6.
    if (arguments.length === 0)
        ThrowError(JSMSG_MISSING_FUN_ARG, 0, "%TypedArray%.prototype.every");
    if (!IsCallable(callbackfn))
        ThrowError(JSMSG_NOT_FUNCTION, DecompileArg(0, callbackfn));

    // Step 7.
    var T = thisArg;

    // Steps 8-9.
    // Omit steps 9.a-9.c and the 'if' clause in step 9.d, since there are no holes in typed arrays.
    for (var k = 0; k < len; k++) {
        // Steps 9.d.i-9.d.ii.
        var kValue = O[k];

        // Steps 9.d.iii-9.d.iv.
        var testResult = callFunction(callbackfn, T, kValue, k, O);

        // Step 9.d.v.
        if (!testResult)
            return false;
    }

    // Step 10.
    return true;
}

// ES6 draft rev29 (2014/12/06) 22.2.3.8 %TypedArray%.prototype.fill(value [, start [, end ]])
function TypedArrayFill(value, start = 0, end = undefined) {
    // This function is not generic.
    if (!IsObject(this) || !IsTypedArray(this)) {
        return callFunction(CallTypedArrayMethodIfWrapped, this, value, start, end,
                            "TypedArrayFill");
    }

    // Steps 1-2.
    var O = this;

    // Steps 3-5.
    var len = TypedArrayLength(O);

    // Steps 6-7.
    var relativeStart = ToInteger(start);

    // Step 8.
    var k = relativeStart < 0
            ? std_Math_max(len + relativeStart, 0)
            : std_Math_min(relativeStart, len);

    // Steps 9-10.
    var relativeEnd = end === undefined ? len : ToInteger(end);

    // Step 11.
    var final = relativeEnd < 0
                ? std_Math_max(len + relativeEnd, 0)
                : std_Math_min(relativeEnd, len);

    // Step 12.
    for (; k < final; k++) {
        O[k] = value;
    }

    // Step 13.
    return O;
}

// ES6 draft 32 (2015-02-02) 22.2.3.9 %TypedArray%.prototype.filter(callbackfn[, thisArg])
function TypedArrayFilter(callbackfn, thisArg = undefined) {
    // Step 1.
    var O = this;

    // Steps 2-3.
    // This function is not generic.
    if (!IsObject(O) || !IsTypedArray(O)) {
        return callFunction(CallTypedArrayMethodIfWrapped, this, callbackfn, thisArg,
                           "TypedArrayFilter");
    }

    // Step 4.
    var len = TypedArrayLength(O);

    // Step 5.
    if (arguments.length === 0)
        ThrowError(JSMSG_MISSING_FUN_ARG, 0, "%TypedArray%.prototype.filter");
    if (!IsCallable(callbackfn))
        ThrowError(JSMSG_NOT_FUNCTION, DecompileArg(0, callbackfn));

    // Step 6.
    var T = thisArg;

    // Step 7.
    var defaultConstructor = _ConstructorForTypedArray(O);

    // Steps 8-9.
    var C = SpeciesConstructor(O, defaultConstructor);

    // Step 10.
    var kept = new List();

    // Step 12.
    var captured = 0;

    // Steps 11, 13 and 13.g.
    for (var k = 0; k < len; k++) {
        // Steps 13.b-c.
        var kValue = O[k];
        // Steps 13.d-e.
        var selected = ToBoolean(callFunction(callbackfn, T, kValue, k, O));
        // Step 13.f.
        if (selected) {
            // Step 13.f.i.
            kept.push(kValue);
            // Step 13.f.ii.
            captured++;
        }
    }

    // Steps 14-15.
    var A = new C(captured);

    // Steps 16 and 17.c.
    for (var n = 0; n < captured; n++) {
        // Steps 17.a-b.
        A[n] = kept[n];
    }

    // Step 18.
    return A;
}

// ES6 draft rev28 (2014/10/14) 22.2.3.10 %TypedArray%.prototype.find(predicate[, thisArg]).
function TypedArrayFind(predicate, thisArg = undefined) {
    // This function is not generic.
    if (!IsObject(this) || !IsTypedArray(this)) {
        return callFunction(CallTypedArrayMethodIfWrapped, this, predicate, thisArg,
                            "TypedArrayFind");
    }

    // Steps 1-2.
    var O = this;

    // Steps 3-5.
    var len = TypedArrayLength(O);

    // Step 6.
    if (arguments.length === 0)
        ThrowError(JSMSG_MISSING_FUN_ARG, 0, "%TypedArray%.prototype.find");
    if (!IsCallable(predicate))
        ThrowError(JSMSG_NOT_FUNCTION, DecompileArg(0, predicate));

    // Step 7.
    var T = thisArg;

    // Steps 8-9.
    // Steps a (implicit), and g.
    for (var k = 0; k < len; k++) {
        // Steps a-c.
        var kValue = O[k];
        // Steps d-f.
        if (callFunction(predicate, T, kValue, k, O))
            return kValue;
    }

    // Step 10.
    return undefined;
}

// ES6 draft rev28 (2014/10/14) 22.2.3.11 %TypedArray%.prototype.findIndex(predicate[, thisArg]).
function TypedArrayFindIndex(predicate, thisArg = undefined) {
    // This function is not generic.
    if (!IsObject(this) || !IsTypedArray(this)) {
        return callFunction(CallTypedArrayMethodIfWrapped, this, predicate, thisArg,
                            "TypedArrayFindIndex");
    }

    // Steps 1-2.
    var O = this;

    // Steps 3-5.
    var len = TypedArrayLength(O);

    // Step 6.
    if (arguments.length === 0)
        ThrowError(JSMSG_MISSING_FUN_ARG, 0, "%TypedArray%.prototype.findIndex");
    if (!IsCallable(predicate))
        ThrowError(JSMSG_NOT_FUNCTION, DecompileArg(0, predicate));

    // Step 7.
    var T = thisArg;

    // Steps 8-9.
    // Steps a (implicit), and g.
    for (var k = 0; k < len; k++) {
        // Steps a-f.
        if (callFunction(predicate, T, O[k], k, O))
            return k;
    }

    // Step 10.
    return -1;
}

// ES6 draft rev31 (2015-01-15) 22.1.3.10 %TypedArray%.prototype.forEach(callbackfn[,thisArg])
function TypedArrayForEach(callbackfn, thisArg = undefined) {
    // This function is not generic.
    if (!IsObject(this) || !IsTypedArray(this)) {
	return callFunction(CallTypedArrayMethodIfWrapped, this, callbackfn, thisArg,
			    "TypedArrayForEach");
    }

    // Step 1-2.
    var O = this;

    // Step 3-4.
    var len = TypedArrayLength(O);

    // Step 5.
    if (arguments.length === 0)
	ThrowError(JSMSG_MISSING_FUN_ARG, 0, 'TypedArray.prototype.forEach');
    if (!IsCallable(callbackfn))
	ThrowError(JSMSG_NOT_FUNCTION, DecompileArg(0, callbackfn));

    // Step 6.
    var T = thisArg;

    // Step 7-8.
    // Step 7, 8a (implicit) and 8e.
    for (var k = 0; k < len; k++) {
	// Step 8b-8c are unnecessary since the condition always holds true for TypedArray.
	// Step 8d.
	callFunction(callbackfn, T, O[k], k, O);
    }

    // Step 9.
    return undefined;
}

// ES6 draft rev29 (2014/12/06) 22.2.3.13 %TypedArray%.prototype.indexOf(searchElement[, fromIndex]).
function TypedArrayIndexOf(searchElement, fromIndex = 0) {
    // This function is not generic.
    if (!IsObject(this) || !IsTypedArray(this)) {
        return callFunction(CallTypedArrayMethodIfWrapped, this, searchElement, fromIndex,
                            "TypedArrayIndexOf");
    }

    // Steps 1-2.
    var O = this;

    // Steps 3-5.
    var len = TypedArrayLength(O);

    // Step 6.
    if (len === 0)
        return -1;

    // Steps 7-8.
    var n = ToInteger(fromIndex);

    // Step 9.
    if (n >= len)
        return -1;

    var k;
    // Step 10.
    if (n >= 0) {
        k = n;
    }
    // Step 11.
    else {
        // Step a.
        k = len + n;
        // Step b.
        if (k < 0)
            k = 0;
    }

    // Step 12.
    // Omit steps a-b, since there are no holes in typed arrays.
    for (; k < len; k++) {
        if (O[k] === searchElement)
            return k;
    }

    // Step 13.
    return -1;
}

// ES6 draft rev30 (2014/12/24) 22.2.3.14 %TypedArray%.prototype.join(separator).
function TypedArrayJoin(separator) {
    // This function is not generic.
    if (!IsObject(this) || !IsTypedArray(this)) {
        return callFunction(CallTypedArrayMethodIfWrapped, this, separator, "TypedArrayJoin");
    }

    // Steps 1-2.
    var O = this;

    // Steps 3-5.
    var len = TypedArrayLength(O);

    // Steps 6-7.
    var sep = separator === undefined ? "," : ToString(separator);

    // Step 8.
    if (len === 0)
        return "";

    // Step 9.
    var element0 = O[0];

    // Steps 10-11.
    // Omit the 'if' clause in step 10, since typed arrays can not have undefined or null elements.
    var R = ToString(element0);

    // Steps 12-13.
    for (var k = 1; k < len; k++) {
        // Step 13.a.
        var S = R + sep;

        // Step 13.b.
        var element = O[k];

        // Steps 13.c-13.d.
        // Omit the 'if' clause in step 13.c, since typed arrays can not have undefined or null elements.
        var next = ToString(element);

        // Step 13.e.
        R = S + next;
    }

    // Step 14.
    return R;
}

// ES6 draft rev30 (2014/12/24) 22.2.3.15 %TypedArray%.prototype.keys()
function TypedArrayKeys() {
    // Step 1.
    var O = this;

    // Step 2-3.
    if (!IsObject(O) || !IsTypedArray(O)) {
        return callFunction(CallTypedArrayMethodIfWrapped, O, "TypedArrayKeys");
    }

    // Step 4-6. Bug 1101256: detachment checks

    // Step 7.
    return CreateArrayIterator(O, ITEM_KIND_KEY);
}

// ES6 draft rev29 (2014/12/06) 22.2.3.16 %TypedArray%.prototype.lastIndexOf(searchElement [,fromIndex]).
function TypedArrayLastIndexOf(searchElement, fromIndex = undefined) {
    // This function is not generic.
    if (!IsObject(this) || !IsTypedArray(this)) {
        return callFunction(CallTypedArrayMethodIfWrapped, this, searchElement, fromIndex,
                            "TypedArrayLastIndexOf");
    }

    // Steps 1-2.
    var O = this;

    // Steps 3-5.
    var len = TypedArrayLength(O);

    // Step 6.
    if (len === 0)
        return -1;

    // Steps 7-8.
    var n = fromIndex === undefined ? len - 1 : ToInteger(fromIndex);

    // Steps 9-10.
    var k = n >= 0 ? std_Math_min(n, len - 1) : len + n;

    // Step 11.
    // Omit steps a-b, since there are no holes in typed arrays.
    for (; k >= 0; k--) {
        if (O[k] === searchElement)
            return k;
    }

    // Step 12.
    return -1;
}

// ES6 draft rev32 (2015-02-02) 22.2.3.18 %TypedArray%.prototype.map(callbackfn [, thisArg]).
function TypedArrayMap(callbackfn, thisArg = undefined) {
    // Step 1.
    var O = this;

    // Steps 2-3.
    // This function is not generic.
    if (!IsObject(O) || !IsTypedArray(O)) {
        return callFunction(CallTypedArrayMethodIfWrapped, this, callbackfn, thisArg,
                            "TypedArrayMap");
    }

    // Step 4.
    var len = TypedArrayLength(O);

    // Step 5.
    if (arguments.length === 0)
        ThrowError(JSMSG_MISSING_FUN_ARG, 0, '%TypedArray%.prototype.map');
    if (!IsCallable(callbackfn))
        ThrowError(JSMSG_NOT_FUNCTION, DecompileArg(0, callbackfn));

    // Step 6.
    var T = thisArg;

    // Step 7.
    var defaultConstructor = _ConstructorForTypedArray(O);

    // Steps 8-9.
    var C = SpeciesConstructor(O, defaultConstructor);

    // Steps 10-11.
    var A = new C(len);

    // Steps 12, 13.a (implicit) and 13.h.
    for (var k = 0; k < len; k++) {
        // Steps 13.d-e.
        var mappedValue = callFunction(callbackfn, T, O[k], k, O);
        // Steps 13.f-g.
        A[k] = mappedValue;
    }

    // Step 14.
    return A;
}

// ES6 draft rev30 (2014/12/24) 22.2.3.19 %TypedArray%.prototype.reduce(callbackfn[, initialValue]).
function TypedArrayReduce(callbackfn/*, initialValue*/) {
    // This function is not generic.
    if (!IsObject(this) || !IsTypedArray(this))
        return callFunction(CallTypedArrayMethodIfWrapped, this, callbackfn, "TypedArrayReduce");

    // Steps 1-2.
    var O = this;

    // Steps 3-5.
    var len = TypedArrayLength(O);

    // Step 6.
    if (arguments.length === 0)
        ThrowError(JSMSG_MISSING_FUN_ARG, 0, "%TypedArray%.prototype.reduce");
    if (!IsCallable(callbackfn))
        ThrowError(JSMSG_NOT_FUNCTION, DecompileArg(0, callbackfn));

    // Step 7.
    if (len === 0 && arguments.length === 1)
        ThrowError(JSMSG_EMPTY_ARRAY_REDUCE);

    // Step 8.
    var k = 0;

    // Steps 9-10.
    // Omit some steps, since 'accumulator' should always be O[0] in step 10 for typed arrays.
    var accumulator = arguments.length > 1 ? arguments[1] : O[k++];

    // Step 11.
    // Omit steps 11.b-11.c and the 'if' clause in step 11.d, since there are no holes in typed arrays.
    for (; k < len; k++) {
        accumulator = callFunction(callbackfn, undefined, accumulator, O[k], k, O);
    }

    // Step 12.
    return accumulator;
}

// ES6 draft rev30 (2014/12/24) 22.2.3.20 %TypedArray%.prototype.reduceRight(callbackfn[, initialValue]).
function TypedArrayReduceRight(callbackfn/*, initialValue*/) {
    // This function is not generic.
    if (!IsObject(this) || !IsTypedArray(this))
        return callFunction(CallTypedArrayMethodIfWrapped, this, callbackfn, "TypedArrayReduceRight");

    // Steps 1-2.
    var O = this;

    // Steps 3-5.
    var len = TypedArrayLength(O);

    // Step 6.
    if (arguments.length === 0)
        ThrowError(JSMSG_MISSING_FUN_ARG, 0, "%TypedArray%.prototype.reduceRight");
    if (!IsCallable(callbackfn))
        ThrowError(JSMSG_NOT_FUNCTION, DecompileArg(0, callbackfn));

    // Step 7.
    if (len === 0 && arguments.length === 1)
        ThrowError(JSMSG_EMPTY_ARRAY_REDUCE);

    // Step 8.
    var k = len - 1;

    // Steps 9-10.
    // Omit some steps, since 'accumulator' should always be O[len-1] in step 10 for typed arrays.
    var accumulator = arguments.length > 1 ? arguments[1] : O[k--];

    // Step 11.
    // Omit steps 11.b-11.c and the 'if' clause in step 11.d, since there are no holes in typed arrays.
    for (; k >= 0; k--) {
        accumulator = callFunction(callbackfn, undefined, accumulator, O[k], k, O);
    }

    // Step 12.
    return accumulator;
}

// ES6 draft rev29 (2014/12/06) 22.2.3.21 %TypedArray%.prototype.reverse().
function TypedArrayReverse() {
    // This function is not generic.
    if (!IsObject(this) || !IsTypedArray(this)) {
        return callFunction(CallTypedArrayMethodIfWrapped, this, "TypedArrayReverse");
    }

    // Steps 1-2.
    var O = this;

    // Steps 3-5.
    var len = TypedArrayLength(O);

    // Step 6.
    var middle = std_Math_floor(len / 2);

    // Steps 7-8.
    // Omit some steps, since there are no holes in typed arrays.
    // Especially all the HasProperty/*exists checks always succeed.
    for (var lower = 0; lower !== middle; lower++) {
        // Step 8.a.
        var upper = len - lower - 1;

        // Step 8.f.i.
        var lowerValue = O[lower];

        // Step 8.i.i.
        var upperValue = O[upper];

        // We always end up in the step 8.j. case.
        O[lower] = upperValue;
        O[upper] = lowerValue;
    }

    // Step 9.
    return O;
}

// ES6 draft rev32 (2015-02-02) 22.2.3.23 %TypedArray%.prototype.slice(start, end).
function TypedArraySlice(start, end) {

    // Step 1.
    var O = this;

    // Step 2-3.
    if (!IsObject(O) || !IsTypedArray(O)) {
        return callFunction(CallTypedArrayMethodIfWrapped, O, start, end, "TypedArraySlice");
    }

    // Step 4.
    var len = TypedArrayLength(O);

    // Steps 5-6.
    var relativeStart = ToInteger(start);

    // Step 7.
    var k = relativeStart < 0
            ? std_Math_max(len + relativeStart, 0)
            : std_Math_min(relativeStart, len);

    // Steps 8-9.
    var relativeEnd = end === undefined ? len : ToInteger(end);

    // Step 10.
    var final = relativeEnd < 0
                ? std_Math_max(len + relativeEnd, 0)
                : std_Math_min(relativeEnd, len);

    // Step 11.
    var count = std_Math_max(final - k, 0);

    // Step 12.
    var defaultConstructor = _ConstructorForTypedArray(O);

    // Steps 13-14.
    var C = SpeciesConstructor(O, defaultConstructor);

    // Steps 15-16.
    var A = new C(count);

    // Step 17.
    var n = 0;

    // Step 18.
    while (k < final) {
        // Steps 18.a-e.
        A[n] = O[k];
        // Step 18f.
        k++;
        // Step 18g.
        n++;
    }

    // Step 19.
    return A;
}

// ES6 draft rev30 (2014/12/24) 22.2.3.25 %TypedArray%.prototype.some(callbackfn[, thisArg]).
function TypedArraySome(callbackfn, thisArg = undefined) {
    // This function is not generic.
    if (!IsObject(this) || !IsTypedArray(this)) {
        return callFunction(CallTypedArrayMethodIfWrapped, this, callbackfn, thisArg,
                            "TypedArraySome");
    }

    // Steps 1-2.
    var O = this;

    // Steps 3-5.
    var len = TypedArrayLength(O);

    // Step 6.
    if (arguments.length === 0)
        ThrowError(JSMSG_MISSING_FUN_ARG, 0, "%TypedArray%.prototype.some");
    if (!IsCallable(callbackfn))
        ThrowError(JSMSG_NOT_FUNCTION, DecompileArg(0, callbackfn));

    // Step 7.
    var T = thisArg;

    // Steps 8-9.
    // Omit steps 9.a-9.c and the 'if' clause in step 9.d, since there are no holes in typed arrays.
    for (var k = 0; k < len; k++) {
        // Steps 9.d.i-9.d.ii.
        var kValue = O[k];

        // Steps 9.d.iii-9.d.iv.
        var testResult = callFunction(callbackfn, T, kValue, k, O);

        // Step 9.d.v.
        if (testResult)
            return true;
    }

    // Step 10.
    return false;
}

// ES6 draft rev30 (2014/12/24) 22.2.3.30 %TypedArray%.prototype.values()
function TypedArrayValues() {
    // Step 1.
    var O = this;

    // Step 2-3.
    if (!IsObject(O) || !IsTypedArray(O)) {
        return callFunction(CallTypedArrayMethodIfWrapped, O, "TypedArrayValues");
    }

    // Step 4-6. Bug 1101256: detachment checks

    // Step 7.
    return CreateArrayIterator(O, ITEM_KIND_VALUE);
}

// Proposed for ES7:
// https://github.com/tc39/Array.prototype.includes/blob/7c023c19a0/spec.md
function TypedArrayIncludes(searchElement, fromIndex = 0) {
    // This function is not generic.
    if (!IsObject(this) || !IsTypedArray(this)) {
        return callFunction(CallTypedArrayMethodIfWrapped, this, searchElement,
                            fromIndex, "TypedArrayIncludes");
    }

    // Steps 1-2.
    var O = this;

    // Steps 3-4.
    var len = TypedArrayLength(O);

    // Step 5.
    if (len === 0)
        return false;

    // Steps 6-7.
    var n = ToInteger(fromIndex);

    var k;
    // Step 8.
    if (n >= 0) {
        k = n;
    }
    // Step 9.
    else {
        // Step a.
        k = len + n;
        // Step b.
        if (k < 0)
            k = 0;
    }

    // Step 10.
    while (k < len) {
        // Steps a-c.
        if (SameValueZero(searchElement, O[k]))
            return true;

        // Step d.
        k++;
    }

    // Step 11.
    return false;
}

// ES6 draft rev30 (2014/12/24) 22.2.2.1 %TypedArray%.from(source[, mapfn[, thisArg]]).
function TypedArrayStaticFrom(source, mapfn = undefined, thisArg = undefined) {
    // Step 1.
    var C = this;

    // Step 2.
    if (!IsConstructor(C))
        ThrowError(JSMSG_NOT_CONSTRUCTOR, DecompileArg(1, C));

    // Step 3.
    var f = mapfn;

    // Step 4.
    if (f !== undefined && !IsCallable(f))
        ThrowError(JSMSG_NOT_FUNCTION, DecompileArg(1, f));

    // Steps 5-6.
    return TypedArrayFrom(C, undefined, source, f, thisArg);
}

// ES6 draft rev30 (2014/12/24) 22.2.2.1.1 TypedArrayFrom().
function TypedArrayFrom(constructor, target, items, mapfn, thisArg) {
    // Step 1.
    var C = constructor;

    // Step 2.
    assert(C === undefined || target === undefined,
           "Neither of 'constructor' and 'target' is undefined");

    // Step 3.
    assert(IsConstructor(C) || C === undefined,
           "'constructor' is neither an constructor nor undefined");

    // Step 4.
    assert(target === undefined || IsTypedArray(target),
           "'target' is neither a typed array nor undefined");

    // Step 5.
    assert(IsCallable(mapfn) || mapfn === undefined,
           "'target' is neither a function nor undefined");

    // Steps 6-7.
    var mapping = mapfn !== undefined;
    var T = thisArg;

    // Steps 8-9.
    var usingIterator = GetMethod(items, std_iterator);

    // Step 10.
    if (usingIterator !== undefined) {
        // Steps 10.a-b.
        var iterator = GetIterator(items, usingIterator);

        // Step 10.c.
        var values = new List();

        // Steps 10.d-e.
        while (true) {
            // Steps 10.e.i-ii.
            var next = iterator.next();
            if (!IsObject(next))
                ThrowError(JSMSG_NEXT_RETURNED_PRIMITIVE);

            // Steps 10.e.iii-vi.
            if (next.done)
                break;
            values.push(next.value);
        }

        // Step 10.f.
        var len = values.length;

        // Steps 10.g-h.
        // There is no need to implement the 22.2.2.1.2 - TypedArrayAllocOrInit() method,
        // since `%TypedArray%(object)` currently doesn't call this self-hosted TypedArrayFrom().
        var targetObj = new C(len);

        // Steps 10.i-j.
        for (var k = 0; k < len; k++) {
            // Steps 10.j.i-ii.
            var kValue = values[k];

            // Steps 10.j.iii-iv.
            var mappedValue = mapping ? callFunction(mapfn, T, kValue, k) : kValue;

            // Steps 10.j.v-vi.
            targetObj[k] = mappedValue;
        }

        // Step 10.k.
        // asserting that `values` is empty here would require removing them one by one from
        // the list's start in the loop above. That would introduce unacceptable overhead.
        // Additionally, the loop's logic is simple enough not to require the assert.

        // Step 10.l.
        return targetObj;
    }

    // Step 11 is an assertion: items is not an Iterator. Testing this is
    // literally the very last thing we did, so we don't assert here.

    // Steps 12-13.
    var arrayLike = ToObject(items);

    // Steps 14-16.
    var len = ToLength(arrayLike.length);

    // Steps 17-18.
    // See comment for steps 10.g-h.
    var targetObj = new C(len);

    // Steps 19-20.
    for (var k = 0; k < len; k++) {
        // Steps 20.a-c.
        var kValue = arrayLike[k];

        // Steps 20.d-e.
        var mappedValue = mapping ? callFunction(mapfn, T, kValue, k) : kValue;

        // Steps 20.f-g.
        targetObj[k] = mappedValue;
    }

    // Step 21.
    return targetObj;
}

// ES6 draft rev30 (2014/12/24) 22.2.2.2 %TypedArray%.of(...items).
function TypedArrayStaticOf(/*...items*/) {
    // Step 1.
    var len = arguments.length;

    // Step 2.
    var items = arguments;

    // Step 3.
    var C = this;

    // Steps 4-5.
    if (!IsConstructor(C))
        ThrowError(JSMSG_NOT_CONSTRUCTOR, typeof C);

    var newObj = new C(len);

    // Steps 6-7.
    for (var k = 0; k < len; k++)
        newObj[k] = items[k]

    // Step 8.
    return newObj;
}

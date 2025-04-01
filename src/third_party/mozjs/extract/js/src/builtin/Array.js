/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* ES5 15.4.4.16. */
function ArrayEvery(callbackfn /*, thisArg*/) {
  /* Step 1. */
  var O = ToObject(this);

  /* Steps 2-3. */
  var len = ToLength(O.length);

  /* Step 4. */
  if (ArgumentsLength() === 0) {
    ThrowTypeError(JSMSG_MISSING_FUN_ARG, 0, "Array.prototype.every");
  }
  if (!IsCallable(callbackfn)) {
    ThrowTypeError(JSMSG_NOT_FUNCTION, DecompileArg(0, callbackfn));
  }

  /* Step 5. */
  var T = ArgumentsLength() > 1 ? GetArgument(1) : undefined;

  /* Steps 6-7. */
  /* Steps a (implicit), and d. */
  for (var k = 0; k < len; k++) {
    /* Step b */
    if (k in O) {
      /* Step c. */
      if (!callContentFunction(callbackfn, T, O[k], k, O)) {
        return false;
      }
    }
  }

  /* Step 8. */
  return true;
}
// Inlining this enables inlining of the callback function.
SetIsInlinableLargeFunction(ArrayEvery);

/* ES5 15.4.4.17. */
function ArraySome(callbackfn /*, thisArg*/) {
  /* Step 1. */
  var O = ToObject(this);

  /* Steps 2-3. */
  var len = ToLength(O.length);

  /* Step 4. */
  if (ArgumentsLength() === 0) {
    ThrowTypeError(JSMSG_MISSING_FUN_ARG, 0, "Array.prototype.some");
  }
  if (!IsCallable(callbackfn)) {
    ThrowTypeError(JSMSG_NOT_FUNCTION, DecompileArg(0, callbackfn));
  }

  /* Step 5. */
  var T = ArgumentsLength() > 1 ? GetArgument(1) : undefined;

  /* Steps 6-7. */
  /* Steps a (implicit), and d. */
  for (var k = 0; k < len; k++) {
    /* Step b */
    if (k in O) {
      /* Step c. */
      if (callContentFunction(callbackfn, T, O[k], k, O)) {
        return true;
      }
    }
  }

  /* Step 8. */
  return false;
}
// Inlining this enables inlining of the callback function.
SetIsInlinableLargeFunction(ArraySome);

/* ES5 15.4.4.18. */
function ArrayForEach(callbackfn /*, thisArg*/) {
  /* Step 1. */
  var O = ToObject(this);

  /* Steps 2-3. */
  var len = ToLength(O.length);

  /* Step 4. */
  if (ArgumentsLength() === 0) {
    ThrowTypeError(JSMSG_MISSING_FUN_ARG, 0, "Array.prototype.forEach");
  }
  if (!IsCallable(callbackfn)) {
    ThrowTypeError(JSMSG_NOT_FUNCTION, DecompileArg(0, callbackfn));
  }

  /* Step 5. */
  var T = ArgumentsLength() > 1 ? GetArgument(1) : undefined;

  /* Steps 6-7. */
  /* Steps a (implicit), and d. */
  for (var k = 0; k < len; k++) {
    /* Step b */
    if (k in O) {
      /* Step c. */
      callContentFunction(callbackfn, T, O[k], k, O);
    }
  }

  /* Step 8. */
  return undefined;
}
// Inlining this enables inlining of the callback function.
SetIsInlinableLargeFunction(ArrayForEach);

/* ES 2016 draft Mar 25, 2016 22.1.3.15. */
function ArrayMap(callbackfn /*, thisArg*/) {
  /* Step 1. */
  var O = ToObject(this);

  /* Step 2. */
  var len = ToLength(O.length);

  /* Step 3. */
  if (ArgumentsLength() === 0) {
    ThrowTypeError(JSMSG_MISSING_FUN_ARG, 0, "Array.prototype.map");
  }
  if (!IsCallable(callbackfn)) {
    ThrowTypeError(JSMSG_NOT_FUNCTION, DecompileArg(0, callbackfn));
  }

  /* Step 4. */
  var T = ArgumentsLength() > 1 ? GetArgument(1) : undefined;

  /* Steps 5. */
  var A = ArraySpeciesCreate(O, len);

  /* Steps 6-7. */
  /* Steps 7.a (implicit), and 7.d. */
  for (var k = 0; k < len; k++) {
    /* Steps 7.b-c. */
    if (k in O) {
      /* Steps 7.c.i-iii. */
      var mappedValue = callContentFunction(callbackfn, T, O[k], k, O);
      DefineDataProperty(A, k, mappedValue);
    }
  }

  /* Step 8. */
  return A;
}
// Inlining this enables inlining of the callback function.
SetIsInlinableLargeFunction(ArrayMap);

/* ES 2016 draft Mar 25, 2016 22.1.3.7 Array.prototype.filter. */
function ArrayFilter(callbackfn /*, thisArg*/) {
  /* Step 1. */
  var O = ToObject(this);

  /* Step 2. */
  var len = ToLength(O.length);

  /* Step 3. */
  if (ArgumentsLength() === 0) {
    ThrowTypeError(JSMSG_MISSING_FUN_ARG, 0, "Array.prototype.filter");
  }
  if (!IsCallable(callbackfn)) {
    ThrowTypeError(JSMSG_NOT_FUNCTION, DecompileArg(0, callbackfn));
  }

  /* Step 4. */
  var T = ArgumentsLength() > 1 ? GetArgument(1) : undefined;

  /* Step 5. */
  var A = ArraySpeciesCreate(O, 0);

  /* Steps 6-8. */
  /* Steps 8.a (implicit), and 8.d. */
  for (var k = 0, to = 0; k < len; k++) {
    /* Steps 8.b-c. */
    if (k in O) {
      /* Step 8.c.i. */
      var kValue = O[k];
      /* Steps 8.c.ii-iii. */
      if (callContentFunction(callbackfn, T, kValue, k, O)) {
        DefineDataProperty(A, to++, kValue);
      }
    }
  }

  /* Step 9. */
  return A;
}
// Inlining this enables inlining of the callback function.
SetIsInlinableLargeFunction(ArrayFilter);

/* ES5 15.4.4.21. */
function ArrayReduce(callbackfn /*, initialValue*/) {
  /* Step 1. */
  var O = ToObject(this);

  /* Steps 2-3. */
  var len = ToLength(O.length);

  /* Step 4. */
  if (ArgumentsLength() === 0) {
    ThrowTypeError(JSMSG_MISSING_FUN_ARG, 0, "Array.prototype.reduce");
  }
  if (!IsCallable(callbackfn)) {
    ThrowTypeError(JSMSG_NOT_FUNCTION, DecompileArg(0, callbackfn));
  }

  /* Step 6. */
  var k = 0;

  /* Steps 5, 7-8. */
  var accumulator;
  if (ArgumentsLength() > 1) {
    accumulator = GetArgument(1);
  } else {
    /* Step 5. */
    // Add an explicit |throw| here and below to inform Ion that the
    // ThrowTypeError calls exit this function.
    if (len === 0) {
      throw ThrowTypeError(JSMSG_EMPTY_ARRAY_REDUCE);
    }

    // Use a |do-while| loop to let Ion know that the loop will definitely
    // be entered at least once. When Ion is then also able to inline the
    // |in| operator, it can optimize away the whole loop.
    var kPresent = false;
    do {
      if (k in O) {
        kPresent = true;
        break;
      }
    } while (++k < len);
    if (!kPresent) {
      throw ThrowTypeError(JSMSG_EMPTY_ARRAY_REDUCE);
    }

    // Moved outside of the loop to ensure the assignment is non-conditional.
    accumulator = O[k++];
  }

  /* Step 9. */
  /* Steps a (implicit), and d. */
  for (; k < len; k++) {
    /* Step b */
    if (k in O) {
      /* Step c. */
      accumulator = callContentFunction(
        callbackfn,
        undefined,
        accumulator,
        O[k],
        k,
        O
      );
    }
  }

  /* Step 10. */
  return accumulator;
}

/* ES5 15.4.4.22. */
function ArrayReduceRight(callbackfn /*, initialValue*/) {
  /* Step 1. */
  var O = ToObject(this);

  /* Steps 2-3. */
  var len = ToLength(O.length);

  /* Step 4. */
  if (ArgumentsLength() === 0) {
    ThrowTypeError(JSMSG_MISSING_FUN_ARG, 0, "Array.prototype.reduce");
  }
  if (!IsCallable(callbackfn)) {
    ThrowTypeError(JSMSG_NOT_FUNCTION, DecompileArg(0, callbackfn));
  }

  /* Step 6. */
  var k = len - 1;

  /* Steps 5, 7-8. */
  var accumulator;
  if (ArgumentsLength() > 1) {
    accumulator = GetArgument(1);
  } else {
    /* Step 5. */
    // Add an explicit |throw| here and below to inform Ion that the
    // ThrowTypeError calls exit this function.
    if (len === 0) {
      throw ThrowTypeError(JSMSG_EMPTY_ARRAY_REDUCE);
    }

    // Use a |do-while| loop to let Ion know that the loop will definitely
    // be entered at least once. When Ion is then also able to inline the
    // |in| operator, it can optimize away the whole loop.
    var kPresent = false;
    do {
      if (k in O) {
        kPresent = true;
        break;
      }
    } while (--k >= 0);
    if (!kPresent) {
      throw ThrowTypeError(JSMSG_EMPTY_ARRAY_REDUCE);
    }

    // Moved outside of the loop to ensure the assignment is non-conditional.
    accumulator = O[k--];
  }

  /* Step 9. */
  /* Steps a (implicit), and d. */
  for (; k >= 0; k--) {
    /* Step b */
    if (k in O) {
      /* Step c. */
      accumulator = callContentFunction(
        callbackfn,
        undefined,
        accumulator,
        O[k],
        k,
        O
      );
    }
  }

  /* Step 10. */
  return accumulator;
}

/* ES6 draft 2013-05-14 15.4.3.23. */
function ArrayFind(predicate /*, thisArg*/) {
  /* Steps 1-2. */
  var O = ToObject(this);

  /* Steps 3-5. */
  var len = ToLength(O.length);

  /* Step 6. */
  if (ArgumentsLength() === 0) {
    ThrowTypeError(JSMSG_MISSING_FUN_ARG, 0, "Array.prototype.find");
  }
  if (!IsCallable(predicate)) {
    ThrowTypeError(JSMSG_NOT_FUNCTION, DecompileArg(0, predicate));
  }

  /* Step 7. */
  var T = ArgumentsLength() > 1 ? GetArgument(1) : undefined;

  /* Steps 8-9. */
  /* Steps a (implicit), and g. */
  for (var k = 0; k < len; k++) {
    /* Steps a-c. */
    var kValue = O[k];
    /* Steps d-f. */
    if (callContentFunction(predicate, T, kValue, k, O)) {
      return kValue;
    }
  }

  /* Step 10. */
  return undefined;
}
// Inlining this enables inlining of the callback function.
SetIsInlinableLargeFunction(ArrayFind);

/* ES6 draft 2013-05-14 15.4.3.23. */
function ArrayFindIndex(predicate /*, thisArg*/) {
  /* Steps 1-2. */
  var O = ToObject(this);

  /* Steps 3-5. */
  var len = ToLength(O.length);

  /* Step 6. */
  if (ArgumentsLength() === 0) {
    ThrowTypeError(JSMSG_MISSING_FUN_ARG, 0, "Array.prototype.find");
  }
  if (!IsCallable(predicate)) {
    ThrowTypeError(JSMSG_NOT_FUNCTION, DecompileArg(0, predicate));
  }

  /* Step 7. */
  var T = ArgumentsLength() > 1 ? GetArgument(1) : undefined;

  /* Steps 8-9. */
  /* Steps a (implicit), and g. */
  for (var k = 0; k < len; k++) {
    /* Steps a-f. */
    if (callContentFunction(predicate, T, O[k], k, O)) {
      return k;
    }
  }

  /* Step 10. */
  return -1;
}
// Inlining this enables inlining of the callback function.
SetIsInlinableLargeFunction(ArrayFindIndex);

// ES2020 draft rev dc1e21c454bd316810be1c0e7af0131a2d7f38e9
// 22.1.3.3 Array.prototype.copyWithin ( target, start [ , end ] )
function ArrayCopyWithin(target, start, end = undefined) {
  // Step 1.
  var O = ToObject(this);

  // Step 2.
  var len = ToLength(O.length);

  // Step 3.
  var relativeTarget = ToInteger(target);

  // Step 4.
  var to =
    relativeTarget < 0
      ? std_Math_max(len + relativeTarget, 0)
      : std_Math_min(relativeTarget, len);

  // Step 5.
  var relativeStart = ToInteger(start);

  // Step 6.
  var from =
    relativeStart < 0
      ? std_Math_max(len + relativeStart, 0)
      : std_Math_min(relativeStart, len);

  // Step 7.
  var relativeEnd = end === undefined ? len : ToInteger(end);

  // Step 8.
  var final =
    relativeEnd < 0
      ? std_Math_max(len + relativeEnd, 0)
      : std_Math_min(relativeEnd, len);

  // Step 9.
  var count = std_Math_min(final - from, len - to);

  // Steps 10-12.
  if (from < to && to < from + count) {
    // Steps 10.b-c.
    from = from + count - 1;
    to = to + count - 1;

    // Step 12.
    while (count > 0) {
      if (from in O) {
        O[to] = O[from];
      } else {
        delete O[to];
      }

      from--;
      to--;
      count--;
    }
  } else {
    // Step 12.
    while (count > 0) {
      if (from in O) {
        O[to] = O[from];
      } else {
        delete O[to];
      }

      from++;
      to++;
      count--;
    }
  }

  // Step 13.
  return O;
}

// ES2020 draft rev dc1e21c454bd316810be1c0e7af0131a2d7f38e9
// 22.1.3.6 Array.prototype.fill ( value [ , start [ , end ] ] )
function ArrayFill(value, start = 0, end = undefined) {
  // Step 1.
  var O = ToObject(this);

  // Step 2.
  var len = ToLength(O.length);

  // Step 3.
  var relativeStart = ToInteger(start);

  // Step 4.
  var k =
    relativeStart < 0
      ? std_Math_max(len + relativeStart, 0)
      : std_Math_min(relativeStart, len);

  // Step 5.
  var relativeEnd = end === undefined ? len : ToInteger(end);

  // Step 6.
  var final =
    relativeEnd < 0
      ? std_Math_max(len + relativeEnd, 0)
      : std_Math_min(relativeEnd, len);

  // Step 7.
  for (; k < final; k++) {
    O[k] = value;
  }

  // Step 8.
  return O;
}

// ES6 draft specification, section 22.1.5.1, version 2013-09-05.
function CreateArrayIterator(obj, kind) {
  var iteratedObject = ToObject(obj);
  var iterator = NewArrayIterator();
  UnsafeSetReservedSlot(iterator, ITERATOR_SLOT_TARGET, iteratedObject);
  UnsafeSetReservedSlot(iterator, ITERATOR_SLOT_NEXT_INDEX, 0);
  UnsafeSetReservedSlot(iterator, ITERATOR_SLOT_ITEM_KIND, kind);
  return iterator;
}

// ES6, 22.1.5.2.1
// http://www.ecma-international.org/ecma-262/6.0/index.html#sec-%arrayiteratorprototype%.next
function ArrayIteratorNext() {
  // Step 1-3.
  var obj = this;
  if (!IsObject(obj) || (obj = GuardToArrayIterator(obj)) === null) {
    return callFunction(
      CallArrayIteratorMethodIfWrapped,
      this,
      "ArrayIteratorNext"
    );
  }

  // Step 4.
  var a = UnsafeGetReservedSlot(obj, ITERATOR_SLOT_TARGET);
  var result = { value: undefined, done: false };

  // Step 5.
  if (a === null) {
    result.done = true;
    return result;
  }

  // Step 6.
  // The index might not be an integer, so we have to do a generic get here.
  var index = UnsafeGetReservedSlot(obj, ITERATOR_SLOT_NEXT_INDEX);

  // Step 7.
  var itemKind = UnsafeGetInt32FromReservedSlot(obj, ITERATOR_SLOT_ITEM_KIND);

  // Step 8-9.
  var len;
  if (IsPossiblyWrappedTypedArray(a)) {
    len = PossiblyWrappedTypedArrayLength(a);

    // If the length is non-zero, the buffer can't be detached.
    if (len === 0) {
      if (PossiblyWrappedTypedArrayHasDetachedBuffer(a)) {
        ThrowTypeError(JSMSG_TYPED_ARRAY_DETACHED);
      }
    }
  } else {
    len = ToLength(a.length);
  }

  // Step 10.
  if (index >= len) {
    UnsafeSetReservedSlot(obj, ITERATOR_SLOT_TARGET, null);
    result.done = true;
    return result;
  }

  // Step 11.
  UnsafeSetReservedSlot(obj, ITERATOR_SLOT_NEXT_INDEX, index + 1);

  // Step 16.
  if (itemKind === ITEM_KIND_VALUE) {
    result.value = a[index];
    return result;
  }

  // Step 13.
  if (itemKind === ITEM_KIND_KEY_AND_VALUE) {
    var pair = [index, a[index]];
    result.value = pair;
    return result;
  }

  // Step 12.
  assert(itemKind === ITEM_KIND_KEY, itemKind);
  result.value = index;
  return result;
}
// We want to inline this to do scalar replacement of the result object.
SetIsInlinableLargeFunction(ArrayIteratorNext);

// Uncloned functions with `$` prefix are allocated as extended function
// to store the original name in `SetCanonicalName`.
function $ArrayValues() {
  return CreateArrayIterator(this, ITEM_KIND_VALUE);
}
SetCanonicalName($ArrayValues, "values");

function ArrayEntries() {
  return CreateArrayIterator(this, ITEM_KIND_KEY_AND_VALUE);
}

function ArrayKeys() {
  return CreateArrayIterator(this, ITEM_KIND_KEY);
}

// https://tc39.es/proposal-array-from-async/
// TODO: Bug 1834560 The step numbers in this will need updating when this is merged
// into the main spec.
function ArrayFromAsync(asyncItems, mapfn = undefined, thisArg = undefined) {
  // Step 1. Let C be the this value.
  var C = this;

  // Step 2. Let promiseCapability be ! NewPromiseCapability(%Promise%).
  // Step 3. Let fromAsyncClosure be a new Abstract Closure with no parameters that captures C, mapfn, and thisArg and performs the following steps when called:
  var fromAsyncClosure = async () => {
    // Step 3.a. If mapfn is undefined, let mapping be false.
    // Step 3.b. Else,
    //     Step 3.b.i. If IsCallable(mapfn) is false, throw a TypeError exception.
    //     Step 3.b.ii. Let mapping be true.
    var mapping = mapfn !== undefined;
    if (mapping && !IsCallable(mapfn)) {
      ThrowTypeError(JSMSG_NOT_FUNCTION, ToSource(mapfn));
    }

    // Step 3.c. Let usingAsyncIterator be ? GetMethod(asyncItems, @@asyncIterator).
    var usingAsyncIterator = asyncItems[GetBuiltinSymbol("asyncIterator")];
    if (usingAsyncIterator === null) {
      usingAsyncIterator = undefined;
    }

    var usingSyncIterator = undefined;
    if (usingAsyncIterator !== undefined) {
      if (!IsCallable(usingAsyncIterator)) {
        ThrowTypeError(JSMSG_NOT_ITERABLE, ToSource(asyncItems));
      }
    } else {
      // Step 3.d. If usingAsyncIterator is undefined, then

      // Step 3.d.i. Let usingSyncIterator be ? GetMethod(asyncItems, @@iterator).
      usingSyncIterator = asyncItems[GetBuiltinSymbol("iterator")];
      if (usingSyncIterator === null) {
        usingSyncIterator = undefined;
      }

      if (usingSyncIterator !== undefined) {
        if (!IsCallable(usingSyncIterator)) {
          ThrowTypeError(JSMSG_NOT_ITERABLE, ToSource(asyncItems));
        }
      }
    }

    // Step 3.g. Let iteratorRecord be undefined.
    // Step 3.j. If iteratorRecord is not undefined, then ...
    if (usingAsyncIterator !== undefined || usingSyncIterator !== undefined) {
      // Note: The published spec as of f6acfc4f0277e625f13fd22068138aec61a12df3
      //       is incorrect. See https://github.com/tc39/proposal-array-from-async/issues/33
      //       Here we use the implementation provided by @bakkot in that bug
      //       in lieu for now; This allows to use a for-await loop below.

      // Steps 3.h-i are implicit through the for-await loop.

      // Step 3.h. If usingAsyncIterator is not undefined, then
      //     Step 3.h.i. Set iteratorRecord to ? GetIterator(asyncItems, async, usingAsyncIterator).
      // Step 3.i. Else if usingSyncIterator is not undefined, then
      //     Set iteratorRecord to ? CreateAsyncFromSyncIterator(GetIterator(asyncItems, sync, usingSyncIterator)).

      // https://github.com/tc39/proposal-array-from-async/pull/41
      // Step 3.e. If IsConstructor(C) is true, then
      //     Step 3.e.i. Let A be ? Construct(C).
      // Step 3.f. Else,
      //     Step 3.f.i. Let A be ! ArrayCreate(0).
      var A = IsConstructor(C) ?
        (ReportUsageCounter(C, SUBCLASS_ARRAY_TYPE_II), constructContentFunction(C, C)) : [];


      // Step 3.j.i. Let k be 0.
      var k = 0;

      // Step 3.j.ii. Repeat,
      for await (var nextValue of allowContentIterWith(
        asyncItems,
        usingAsyncIterator,
        usingSyncIterator
      )) {
        // Following in the steps of Array.from, we don't actually implement 3.j.ii.1.
        // The comment in Array.from also applies here; we should only encounter this
        // after a huge loop around a proxy
        // Step 3.j.ii.1. If k ≥ 2**53 - 1, then
        //     Step 3.j.ii.1.a. Let error be ThrowCompletion(a newly created TypeError object).
        //     Step 3.j.ii.1.b. Return ? AsyncIteratorClose(iteratorRecord, error).
        // Step 3.j.ii.2. Let Pk be ! ToString(𝔽(k)).

        // Step 3.j.ii.3. Let next be ? Await(IteratorStep(iteratorRecord)).

        // Step 3.j.ii.5. Let nextValue be ? IteratorValue(next). (Implicit through the for-await loop).

        // Step 3.j.ii.7. Else, let mappedValue be nextValue. (Reordered)
        var mappedValue = nextValue;

        // Step 3.j.ii.6. If mapping is true, then
        if (mapping) {
          // Step 3.j.ii.6.a. Let mappedValue be Call(mapfn, thisArg, « nextValue, 𝔽(k) »).
          // Step 3.j.ii.6.b. IfAbruptCloseAsyncIterator(mappedValue, iteratorRecord).
          //   Abrupt completion will be handled by the for-await loop.
          mappedValue = callContentFunction(mapfn, thisArg, nextValue, k);

          // Step 3.j.ii.6.c. Set mappedValue to Await(mappedValue).
          // Step 3.j.ii.6.d. IfAbruptCloseAsyncIterator(mappedValue, iteratorRecord).
          mappedValue = await mappedValue;
        }

        // Step 3.j.ii.8. Let defineStatus be CreateDataPropertyOrThrow(A, Pk, mappedValue).
        // Step 3.j.ii.9. If defineStatus is an abrupt completion, return ? AsyncIteratorClose(iteratorRecord, defineStatus).
        DefineDataProperty(A, k, mappedValue);

        // Step 3.j.ii.10. Set k to k + 1.
        k = k + 1;
      }

      // Step 3.j.ii.4. If next is false, then (Reordered)

      // Step 3.j.ii.4.a. Perform ? Set(A, "length", 𝔽(k), true).
      A.length = k;

      // Step 3.j.ii.4.b. Return Completion Record { [[Type]]: return, [[Value]]: A, [[Target]]: empty }.
      return A;
    }

    // Step 3.k. Else,

    // Step 3.k.i. NOTE: asyncItems is neither an AsyncIterable nor an Iterable so assume it is an array-like object.
    // Step 3.k.ii. Let arrayLike be ! ToObject(asyncItems).
    var arrayLike = ToObject(asyncItems);

    // Step 3.k.iii. Let len be ? LengthOfArrayLike(arrayLike).
    var len = ToLength(arrayLike.length);

    // Step 3.k.iv. If IsConstructor(C) is true, then
    //     Step 3.k.iv.1. Let A be ? Construct(C, « 𝔽(len) »).
    // Step 3.k.v. Else,
    //     Step 3.k.v.1. Let A be ? ArrayCreate(len).
    var A = IsConstructor(C) ? (ReportUsageCounter(C, SUBCLASS_ARRAY_TYPE_II), constructContentFunction(C, C, len)) : std_Array(len);

    // Step 3.k.vi. Let k be 0.
    var k = 0;

    // Step 3.k.vii. Repeat, while k < len,
    while (k < len) {
      // Step 3.k.vii.1. Let Pk be ! ToString(𝔽(k)).
      // Step 3.k.vii.2. Let kValue be ? Get(arrayLike, Pk).
      // Step 3.k.vii.3. Let kValue be ? Await(kValue).
      var kValue = await arrayLike[k];

      // Step 3.k.vii.4. If mapping is true, then
      //     Step 3.k.vii.4.a. Let mappedValue be ? Call(mapfn, thisArg, « kValue, 𝔽(k) »).
      //     Step 3.k.vii.4.b. Let mappedValue be ? Await(mappedValue).
      // Step 3.k.vii.5. Else, let mappedValue be kValue.
      var mappedValue = mapping
        ? await callContentFunction(mapfn, thisArg, kValue, k)
        : kValue;

      // Step 3.k.vii.6. Perform ? CreateDataPropertyOrThrow(A, Pk, mappedValue).
      DefineDataProperty(A, k, mappedValue);

      // Step 3.k.vii.7. Set k to k + 1.
      k = k + 1;
    }

    // Step 3.k.viii. Perform ? Set(A, "length", 𝔽(len), true).
    A.length = len;

    // Step 3.k.ix. Return Completion Record { [[Type]]: return, [[Value]]: A, [[Target]]: empty }.
    return A;
  };

  // Step 4. Perform AsyncFunctionStart(promiseCapability, fromAsyncClosure).
  // Step 5. Return promiseCapability.[[Promise]].
  return fromAsyncClosure();
}

// ES 2017 draft 0f10dba4ad18de92d47d421f378233a2eae8f077 22.1.2.1
function ArrayFrom(items, mapfn = undefined, thisArg = undefined) {
  // Step 1.
  var C = this;

  // Steps 2-3.
  var mapping = mapfn !== undefined;
  if (mapping && !IsCallable(mapfn)) {
    ThrowTypeError(JSMSG_NOT_FUNCTION, DecompileArg(1, mapfn));
  }
  var T = thisArg;

  // Step 4.
  // Inlined: GetMethod, steps 1-2.
  var usingIterator = items[GetBuiltinSymbol("iterator")];

  // Step 5.
  // Inlined: GetMethod, step 3.
  if (!IsNullOrUndefined(usingIterator)) {
    // Inlined: GetMethod, step 4.
    if (!IsCallable(usingIterator)) {
      ThrowTypeError(JSMSG_NOT_ITERABLE, DecompileArg(0, items));
    }

    // Steps 5.a-b.
    var A = IsConstructor(C) ? (ReportUsageCounter(C, SUBCLASS_ARRAY_TYPE_II), constructContentFunction(C, C)) : [];

    // Step 5.d.
    var k = 0;

    // Steps 5.c, 5.e
    for (var nextValue of allowContentIterWith(items, usingIterator)) {
      // Step 5.e.i.
      // Disabled for performance reason.  We won't hit this case on
      // normal array, since DefineDataProperty will throw before it.
      // We could hit this when |A| is a proxy and it ignores
      // |DefineDataProperty|, but it happens only after too long loop.
      /*
      if (k >= 0x1fffffffffffff) {
          ThrowTypeError(JSMSG_TOO_LONG_ARRAY);
      }
      */

      // Steps 5.e.vi-vii.
      var mappedValue = mapping
        ? callContentFunction(mapfn, T, nextValue, k)
        : nextValue;

      // Steps 5.e.ii (reordered), 5.e.viii.
      DefineDataProperty(A, k++, mappedValue);
    }

    // Step 5.e.iv.
    A.length = k;
    return A;
  }

  // Step 7 is an assertion: items is not an Iterator. Testing this is
  // literally the very last thing we did, so we don't assert here.

  // Steps 8-9.
  var arrayLike = ToObject(items);

  // Steps 10-11.
  var len = ToLength(arrayLike.length);

  // Steps 12-14.
  var A = IsConstructor(C)
    ? (ReportUsageCounter(C, SUBCLASS_ARRAY_TYPE_II), constructContentFunction(C, C, len))
    : std_Array(len);

  // Steps 15-16.
  for (var k = 0; k < len; k++) {
    // Steps 16.a-c.
    var kValue = items[k];

    // Steps 16.d-e.
    var mappedValue = mapping
      ? callContentFunction(mapfn, T, kValue, k)
      : kValue;

    // Steps 16.f-g.
    DefineDataProperty(A, k, mappedValue);
  }

  // Steps 17-18.
  A.length = len;

  // Step 19.
  return A;
}

// ES2015 22.1.3.27 Array.prototype.toString.
function ArrayToString() {
  // Steps 1-2.
  var array = ToObject(this);

  // Steps 3-4.
  var func = array.join;

  // Steps 5-6.
  if (!IsCallable(func)) {
    return callFunction(std_Object_toString, array);
  }
  return callContentFunction(func, array);
}

// ES2017 draft rev f8a9be8ea4bd97237d176907a1e3080dce20c68f
// 22.1.3.27 Array.prototype.toLocaleString ([ reserved1 [ , reserved2 ] ])
// ES2017 Intl draft rev 78bbe7d1095f5ff3760ac4017ed366026e4cb276
// 13.4.1 Array.prototype.toLocaleString ([ locales [ , options ]])
function ArrayToLocaleString(locales, options) {
  // Step 1 (ToObject already performed in native code).
  assert(IsObject(this), "|this| should be an object");
  var array = this;

  // Step 2.
  var len = ToLength(array.length);

  // Step 4.
  if (len === 0) {
    return "";
  }

  // Step 5.
  var firstElement = array[0];

  // Steps 6-7.
  var R;
  if (IsNullOrUndefined(firstElement)) {
    R = "";
  } else {
    #if JS_HAS_INTL_API
    R = ToString(
      callContentFunction(
        firstElement.toLocaleString,
        firstElement,
        locales,
        options
      )
    );
    #else
    R = ToString(
      callContentFunction(firstElement.toLocaleString, firstElement)
    );
    #endif
  }

  // Step 3 (reordered).
  // We don't (yet?) implement locale-dependent separators.
  var separator = ",";

  // Steps 8-9.
  for (var k = 1; k < len; k++) {
    // Step 9.b.
    var nextElement = array[k];

    // Steps 9.a, 9.c-e.
    R += separator;
    if (!IsNullOrUndefined(nextElement)) {
      #if JS_HAS_INTL_API
      R += ToString(
        callContentFunction(
          nextElement.toLocaleString,
          nextElement,
          locales,
          options
        )
      );
      #else
      R += ToString(
        callContentFunction(nextElement.toLocaleString, nextElement)
      );
      #endif
    }
  }

  // Step 10.
  return R;
}

// ES 2016 draft Mar 25, 2016 22.1.2.5.
function $ArraySpecies() {
  // Step 1.
  return this;
}
SetCanonicalName($ArraySpecies, "get [Symbol.species]");

// ES 2016 draft Mar 25, 2016 9.4.2.3.
function ArraySpeciesCreate(originalArray, length) {
  // Step 1.
  assert(typeof length === "number", "length should be a number");
  assert(length >= 0, "length should be a non-negative number");

  // Step 2.
  // eslint-disable-next-line no-compare-neg-zero
  if (length === -0) {
    length = 0;
  }

  // Step 4, 6.
  if (!IsArray(originalArray)) {
    return std_Array(length);
  }

  // Step 5.a.
  var C = originalArray.constructor;

  // Step 5.b.
  if (IsConstructor(C) && IsCrossRealmArrayConstructor(C)) {
    return std_Array(length);
  }

  // Step 5.c.
  if (IsObject(C)) {
    // Step 5.c.i.
    C = C[GetBuiltinSymbol("species")];

    // Optimized path for an ordinary Array.
    if (C === GetBuiltinConstructor("Array")) {
      return std_Array(length);
    }

    // Step 5.c.ii.
    if (C === null) {
      return std_Array(length);
    }

  }

  // Step 6.
  if (C === undefined) {
    return std_Array(length);
  }

  // Step 7.
  if (!IsConstructor(C)) {
    ThrowTypeError(JSMSG_NOT_CONSTRUCTOR, "constructor property");
  }

  // Step 8.
  ReportUsageCounter(C, SUBCLASS_ARRAY_TYPE_III);
  return constructContentFunction(C, C, length);
}

// ES2020 draft rev dc1e21c454bd316810be1c0e7af0131a2d7f38e9
// 22.1.3.11 Array.prototype.flatMap ( mapperFunction [ , thisArg ] )
function ArrayFlatMap(mapperFunction /*, thisArg*/) {
  // Step 1.
  var O = ToObject(this);

  // Step 2.
  var sourceLen = ToLength(O.length);

  // Step 3.
  if (!IsCallable(mapperFunction)) {
    ThrowTypeError(JSMSG_NOT_FUNCTION, DecompileArg(0, mapperFunction));
  }

  // Step 4.
  var T = ArgumentsLength() > 1 ? GetArgument(1) : undefined;

  // Step 5.
  var A = ArraySpeciesCreate(O, 0);

  // Step 6.
  FlattenIntoArray(A, O, sourceLen, 0, 1, mapperFunction, T);

  // Step 7.
  return A;
}

// ES2020 draft rev dc1e21c454bd316810be1c0e7af0131a2d7f38e9
// 22.1.3.10 Array.prototype.flat ( [ depth ] )
function ArrayFlat(/* depth */) {
  // Step 1.
  var O = ToObject(this);

  // Step 2.
  var sourceLen = ToLength(O.length);

  // Step 3.
  var depthNum = 1;

  // Step 4.
  if (ArgumentsLength() && GetArgument(0) !== undefined) {
    depthNum = ToInteger(GetArgument(0));
  }

  // Step 5.
  var A = ArraySpeciesCreate(O, 0);

  // Step 6.
  FlattenIntoArray(A, O, sourceLen, 0, depthNum);

  // Step 7.
  return A;
}

// ES2020 draft rev dc1e21c454bd316810be1c0e7af0131a2d7f38e9
// 22.1.3.10.1 FlattenIntoArray ( target, source, sourceLen, start, depth [ , mapperFunction, thisArg ] )
function FlattenIntoArray(
  target,
  source,
  sourceLen,
  start,
  depth,
  mapperFunction,
  thisArg
) {
  // Step 1.
  var targetIndex = start;

  // Steps 2-3.
  for (var sourceIndex = 0; sourceIndex < sourceLen; sourceIndex++) {
    // Steps 3.a-c.
    if (sourceIndex in source) {
      // Step 3.c.i.
      var element = source[sourceIndex];

      if (mapperFunction) {
        // Step 3.c.ii.1.
        assert(ArgumentsLength() === 7, "thisArg is present");

        // Step 3.c.ii.2.
        element = callContentFunction(
          mapperFunction,
          thisArg,
          element,
          sourceIndex,
          source
        );
      }

      // Step 3.c.iii.
      var shouldFlatten = false;

      // Step 3.c.iv.
      if (depth > 0) {
        // Step 3.c.iv.1.
        shouldFlatten = IsArray(element);
      }

      // Step 3.c.v.
      if (shouldFlatten) {
        // Step 3.c.v.1.
        var elementLen = ToLength(element.length);

        // Step 3.c.v.2.
        targetIndex = FlattenIntoArray(
          target,
          element,
          elementLen,
          targetIndex,
          depth - 1
        );
      } else {
        // Step 3.c.vi.1.
        if (targetIndex >= MAX_NUMERIC_INDEX) {
          ThrowTypeError(JSMSG_TOO_LONG_ARRAY);
        }

        // Step 3.c.vi.2.
        DefineDataProperty(target, targetIndex, element);

        // Step 3.c.vi.3.
        targetIndex++;
      }
    }
  }

  // Step 4.
  return targetIndex;
}

// https://github.com/tc39/proposal-relative-indexing-method
// Array.prototype.at ( index )
function ArrayAt(index) {
  // Step 1.
  var O = ToObject(this);

  // Step 2.
  var len = ToLength(O.length);

  // Step 3.
  var relativeIndex = ToInteger(index);

  // Steps 4-5.
  var k;
  if (relativeIndex >= 0) {
    k = relativeIndex;
  } else {
    k = len + relativeIndex;
  }

  // Step 6.
  if (k < 0 || k >= len) {
    return undefined;
  }

  // Step 7.
  return O[k];
}
// This function is only barely too long for normal inlining.
SetIsInlinableLargeFunction(ArrayAt);

// https://github.com/tc39/proposal-change-array-by-copy
// Array.prototype.toReversed()
function ArrayToReversed() {
  // Step 1. Let O be ? ToObject(this value).
  var O = ToObject(this);

  // Step 2. Let len be ? LengthOfArrayLike(O).
  var len = ToLength(O.length);

  // Step 3. Let A be ArrayCreate(𝔽(len)).
  var A = std_Array(len);

  // Step 4. Let k be 0.
  // Step 5. Repeat, while k < len,
  for (var k = 0; k < len; k++) {
    // Step 5.a. Let from be ! ToString(𝔽(len - k - 1)).
    var from = len - k - 1;

    // Skip Step 5.b. Let Pk be ToString(𝔽(k)).
    // k is coerced into a string through the property access.

    // Step 5.c. Let fromValue be ? Get(O, from).
    var fromValue = O[from];

    // Step 5.d. Perform ! CreateDataPropertyOrThrow(A, 𝔽(k), fromValue).
    DefineDataProperty(A, k, fromValue);
  }

  // Step 6. Return A.
  return A;
}

// https://github.com/tc39/proposal-change-array-by-copy
// Array.prototype.toSorted()
function ArrayToSorted(comparefn) {
  // Step 1.  If comparefn is not undefined and IsCallable(comparefn) is
  // false, throw a TypeError exception.
  if (comparefn !== undefined && !IsCallable(comparefn)) {
    ThrowTypeError(JSMSG_BAD_TOSORTED_ARG);
  }

  // Step 2. Let O be ? ToObject(this value).
  var O = ToObject(this);

  // Step 3. Let len be ? LengthOfArrayLike(O).
  var len = ToLength(O.length);

  // Step 4. Let A be ? ArrayCreate(𝔽(len)).
  var items = std_Array(len);

  // We depart from steps 5-8 of the spec for performance reasons, as
  // following the spec would require copying the input array twice.
  // Instead, we create a new array that replaces holes with undefined,
  // and sort this array.
  for (var k = 0; k < len; k++) {
    DefineDataProperty(items, k, O[k]);
  }

  // Arrays with less than two elements remain unchanged when sorted.
  if (len <= 1) {
    return items;
  }

  // Steps 5-9.
  return callFunction(std_Array_sort, items, comparefn);
}

// https://github.com/tc39/proposal-array-find-from-last
// Array.prototype.findLast ( predicate, thisArg )
function ArrayFindLast(predicate /*, thisArg*/) {
  // Step 1.
  var O = ToObject(this);

  // Step 2.
  var len = ToLength(O.length);

  // Step 3.
  if (ArgumentsLength() === 0) {
    ThrowTypeError(JSMSG_MISSING_FUN_ARG, 0, "Array.prototype.findLast");
  }
  if (!IsCallable(predicate)) {
    ThrowTypeError(JSMSG_NOT_FUNCTION, DecompileArg(0, predicate));
  }

  var thisArg = ArgumentsLength() > 1 ? GetArgument(1) : undefined;

  // Steps 4-5.
  for (var k = len - 1; k >= 0; k--) {
    // Steps 5.a-b.
    var kValue = O[k];

    // Steps 5.c-d.
    if (callContentFunction(predicate, thisArg, kValue, k, O)) {
      return kValue;
    }
  }

  // Step 6.
  return undefined;
}
// Inlining this enables inlining of the callback function.
SetIsInlinableLargeFunction(ArrayFindLast);

// https://github.com/tc39/proposal-array-find-from-last
// Array.prototype.findLastIndex ( predicate, thisArg )
function ArrayFindLastIndex(predicate /*, thisArg*/) {
  // Step 1.
  var O = ToObject(this);

  // Steps 2.
  var len = ToLength(O.length);

  // Step 3.
  if (ArgumentsLength() === 0) {
    ThrowTypeError(JSMSG_MISSING_FUN_ARG, 0, "Array.prototype.findLastIndex");
  }
  if (!IsCallable(predicate)) {
    ThrowTypeError(JSMSG_NOT_FUNCTION, DecompileArg(0, predicate));
  }

  var thisArg = ArgumentsLength() > 1 ? GetArgument(1) : undefined;

  // Steps 4-5.
  for (var k = len - 1; k >= 0; k--) {
    // Steps 5.a-d.
    if (callContentFunction(predicate, thisArg, O[k], k, O)) {
      return k;
    }
  }

  // Step 6.
  return -1;
}
// Inlining this enables inlining of the callback function.
SetIsInlinableLargeFunction(ArrayFindLastIndex);

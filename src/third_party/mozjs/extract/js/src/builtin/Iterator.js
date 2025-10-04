/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

function IteratorIdentity() {
  return this;
}

/* ECMA262 7.2.7 */
function IteratorNext(iteratorRecord, value) {
  // Steps 1-2.
  var result =
    ArgumentsLength() < 2
      ? callContentFunction(iteratorRecord.nextMethod, iteratorRecord.iterator)
      : callContentFunction(
          iteratorRecord.nextMethod,
          iteratorRecord.iterator,
          value
        );
  // Step 3.
  if (!IsObject(result)) {
    ThrowTypeError(JSMSG_OBJECT_REQUIRED, result);
  }
  // Step 4.
  return result;
}

// https://tc39.es/ecma262/#sec-getiterator
function GetIterator(obj, isAsync, method) {
  // Step 1. If hint is not present, set hint to sync.
  // Step 2. If method is not present, then
  if (!method) {
    // Step 2.a. If hint is async, then
    if (isAsync) {
      // Step 2.a.i. Set method to ? GetMethod(obj, @@asyncIterator).
      method = GetMethod(obj, GetBuiltinSymbol("asyncIterator"));

      // Step 2.a.ii. If method is undefined, then
      if (!method) {
        // Step 2.a.ii.1. Let syncMethod be ? GetMethod(obj, @@iterator).
        var syncMethod = GetMethod(obj, GetBuiltinSymbol("iterator"));

        // Step 2.a.ii.2. Let syncIteratorRecord be ? GetIterator(obj, sync, syncMethod).
        var syncIteratorRecord = GetIterator(obj, false, syncMethod);

        // Step 2.a.ii.2. Return CreateAsyncFromSyncIterator(syncIteratorRecord).
        return CreateAsyncFromSyncIterator(syncIteratorRecord.iterator, syncIteratorRecord.nextMethod);
      }
    } else {
      // Step 2.b. Otherwise, set method to ? GetMethod(obj, @@iterator).
      method = GetMethod(obj, GetBuiltinSymbol("iterator"));
    }
  }

  // Step 3. Let iterator be ? Call(method, obj).
  var iterator = callContentFunction(method, obj);

  // Step 4. If Type(iterator) is not Object, throw a TypeError exception.
  if (!IsObject(iterator)) {
    ThrowTypeError(JSMSG_NOT_ITERABLE, obj === null ? "null" : typeof obj);
  }

  // Step 5. Let nextMethod be ? GetV(iterator, "next").
  var nextMethod = iterator.next;

  // Step 6. Let iteratorRecord be the Record { [[Iterator]]: iterator, [[NextMethod]]: nextMethod, [[Done]]: false }.
  var iteratorRecord = {
    __proto__: null,
    iterator,
    nextMethod,
    done: false,
  };

  // Step 7. Return iteratorRecord.
  return iteratorRecord;
}

/**
 * GetIteratorFlattenable ( obj, stringHandling )
 *
 * https://tc39.es/proposal-iterator-helpers/#sec-getiteratorflattenable
 */
function GetIteratorFlattenable(obj, rejectStrings) {
  assert(typeof rejectStrings === "boolean", "rejectStrings is a boolean");

  // Step 1.
  if (!IsObject(obj)) {
    // Step 1.a.
    if (rejectStrings || typeof obj !== "string") {
      ThrowTypeError(JSMSG_OBJECT_REQUIRED, obj === null ? "null" : typeof obj);
    }
  }

  // Step 2.
  var method = obj[GetBuiltinSymbol("iterator")];

  // Steps 3-4.
  var iterator;
  if (IsNullOrUndefined(method)) {
    iterator = obj;
  } else {
    iterator = callContentFunction(method, obj);
  }

  // Step 5.
  if (!IsObject(iterator)) {
    ThrowTypeError(JSMSG_OBJECT_REQUIRED, iterator === null ? "null" : typeof iterator);
  }

  // Step 6. (Caller must call GetIteratorDirect.)
  return iterator;
}

/**
 * Iterator.from ( O )
 *
 * https://tc39.es/proposal-iterator-helpers/#sec-iterator.from
 */
function IteratorFrom(O) {
  // Step 1. (Inlined call to GetIteratorDirect.)
  var iterator = GetIteratorFlattenable(O, /* rejectStrings= */ false);
  var nextMethod = iterator.next;

  // Step 2.
  //
  // Calls |isPrototypeOf| instead of |instanceof| to avoid looking up the
  // `@@hasInstance` property.
  var hasInstance = callFunction(
    std_Object_isPrototypeOf,
    GetBuiltinPrototype("Iterator"),
    iterator
  );

  // Step 3.
  if (hasInstance) {
    return iterator;
  }

  // Step 4.
  var wrapper = NewWrapForValidIterator();

  // Step 5.
  UnsafeSetReservedSlot(
    wrapper,
    WRAP_FOR_VALID_ITERATOR_ITERATOR_SLOT,
    iterator
  );
  UnsafeSetReservedSlot(
    wrapper,
    WRAP_FOR_VALID_ITERATOR_NEXT_METHOD_SLOT,
    nextMethod
  );

  // Step 6.
  return wrapper;
}

/**
 * %WrapForValidIteratorPrototype%.next ( )
 *
 * https://tc39.es/proposal-iterator-helpers/#sec-wrapforvaliditeratorprototype.next
 */
function WrapForValidIteratorNext() {
  // Steps 1-2.
  var O = this;
  if (!IsObject(O) || (O = GuardToWrapForValidIterator(O)) === null) {
    return callFunction(
      CallWrapForValidIteratorMethodIfWrapped,
      this,
      "WrapForValidIteratorNext"
    );
  }

  // Step 3.
  var iterator = UnsafeGetReservedSlot(O, WRAP_FOR_VALID_ITERATOR_ITERATOR_SLOT);
  var nextMethod = UnsafeGetReservedSlot(O, WRAP_FOR_VALID_ITERATOR_NEXT_METHOD_SLOT);

  // Step 4.
  return callContentFunction(nextMethod, iterator);
}

/**
 * %WrapForValidIteratorPrototype%.return ( )
 *
 * https://tc39.es/proposal-iterator-helpers/#sec-wrapforvaliditeratorprototype.return
 */
function WrapForValidIteratorReturn() {
  // Steps 1-2.
  var O = this;
  if (!IsObject(O) || (O = GuardToWrapForValidIterator(O)) === null) {
    return callFunction(
      CallWrapForValidIteratorMethodIfWrapped,
      this,
      "WrapForValidIteratorReturn"
    );
  }

  // Step 3.
  var iterator = UnsafeGetReservedSlot(O, WRAP_FOR_VALID_ITERATOR_ITERATOR_SLOT);

  // Step 4.
  assert(IsObject(iterator), "iterator is an object");

  // Step 5.
  var returnMethod = iterator.return;

  // Step 6.
  if (IsNullOrUndefined(returnMethod)) {
    return {
      value: undefined,
      done: true,
    };
  }

  // Step 7.
  return callContentFunction(returnMethod, iterator);
}

/**
 * %IteratorHelperPrototype%.next ( )
 *
 * https://tc39.es/proposal-iterator-helpers/#sec-%iteratorhelperprototype%.next
 */
function IteratorHelperNext() {
  // Step 1.
  var O = this;
  if (!IsObject(O) || (O = GuardToIteratorHelper(O)) === null) {
    return callFunction(
      CallIteratorHelperMethodIfWrapped,
      this,
      "IteratorHelperNext"
    );
  }
  var generator = UnsafeGetReservedSlot(O, ITERATOR_HELPER_GENERATOR_SLOT);
  return callFunction(GeneratorNext, generator, undefined);
}

/**
 * %IteratorHelperPrototype%.return ( )
 *
 * https://tc39.es/proposal-iterator-helpers/#sec-%iteratorhelperprototype%.return
 */
function IteratorHelperReturn() {
  // Step 1.
  var O = this;

  // Step 2.
  if (!IsObject(O) || (O = GuardToIteratorHelper(O)) === null) {
    return callFunction(
      CallIteratorHelperMethodIfWrapped,
      this,
      "IteratorHelperReturn"
    );
  }

  // Step 3. (Implicit)

  // Steps 4-6.
  var generator = UnsafeGetReservedSlot(O, ITERATOR_HELPER_GENERATOR_SLOT);
  return callFunction(GeneratorReturn, generator, undefined);
}

// Lazy %Iterator.prototype% methods
//
// In order to match the semantics of the built-in generator objects used in
// the proposal, we use a reserved slot on the IteratorHelper objects to store
// a regular generator that is called from the %IteratorHelper.prototype%
// methods.
//
// Each of the lazy methods is divided into a prelude and a body, with the
// eager prelude steps being contained in the corresponding IteratorX method
// and the lazy body steps inside the IteratorXGenerator generator functions.
//
// Each prelude method initializes and returns a new IteratorHelper object.
// As part of this initialization process, the appropriate generator function
// is called, followed by GeneratorNext being called on returned generator
// instance in order to move it to its first yield point. This is done so that
// if the `return` method is called on the IteratorHelper before `next` has been
// called, we can catch them in the try and use the finally block to close the
// underlying iterator.

/**
 * Iterator.prototype.map ( mapper )
 *
 * https://tc39.es/proposal-iterator-helpers/#sec-iteratorprototype.map
 */
function IteratorMap(mapper) {
  // Step 1.
  var iterator = this;

  // Step 2.
  if (!IsObject(iterator)) {
    ThrowTypeError(JSMSG_OBJECT_REQUIRED, iterator === null ? "null" : typeof iterator);
  }

  // Step 3.
  if (!IsCallable(mapper)) {
    ThrowTypeError(JSMSG_NOT_FUNCTION, DecompileArg(0, mapper));
  }

  // Step 4. (Inlined call to GetIteratorDirect.)
  var nextMethod = iterator.next;

  // Steps 5-7.
  var result = NewIteratorHelper();
  var generator = IteratorMapGenerator(iterator, nextMethod, mapper);
  UnsafeSetReservedSlot(
    result,
    ITERATOR_HELPER_GENERATOR_SLOT,
    generator
  );

  // Stop at the initial yield point.
  callFunction(GeneratorNext, generator);

  // Step 8.
  return result;
}

/**
 * Iterator.prototype.map ( mapper )
 *
 * Abstract closure definition.
 *
 * https://tc39.es/proposal-iterator-helpers/#sec-iteratorprototype.map
 */
function* IteratorMapGenerator(iterator, nextMethod, mapper) {
  var isReturnCompletion = true;
  try {
    // Initial yield point to handle closing the iterator before the for-of
    // loop has been entered for the first time.
    yield;

    // Not a Return completion when execution continues normally after |yield|.
    isReturnCompletion = false;
  } finally {
    // Call IteratorClose on a Return completion.
    if (isReturnCompletion) {
      IteratorClose(iterator);
    }
  }

  // Step 5.a.
  var counter = 0;

  // Step 5.b.
  for (var value of allowContentIterWithNext(iterator, nextMethod)) {
    // Steps 5.b.i-iii. (Implicit through for-of loop)

    // Step 5.b.iv.
    var mapped = callContentFunction(mapper, undefined, value, counter);

    // Step 5.b.v. (Implicit through for-of loop)

    // Step 5.b.vi.
    yield mapped;

    // Step 5.b.vii. (Implicit through for-of loop)

    // Step 5.b.viii.
    counter += 1;
  }
}

/**
 * Iterator.prototype.filter ( predicate )
 *
 * https://tc39.es/proposal-iterator-helpers/#sec-iteratorprototype.filter
 */
function IteratorFilter(predicate) {
  // Step 1.
  var iterator = this;

  // Step 2.
  if (!IsObject(iterator)) {
    ThrowTypeError(JSMSG_OBJECT_REQUIRED, iterator === null ? "null" : typeof iterator);
  }

  // Step 3.
  if (!IsCallable(predicate)) {
    ThrowTypeError(JSMSG_NOT_FUNCTION, DecompileArg(0, predicate));
  }

  // Step 4. (Inlined call to GetIteratorDirect.)
  var nextMethod = iterator.next;

  // Steps 5-7.
  var result = NewIteratorHelper();
  var generator = IteratorFilterGenerator(iterator, nextMethod, predicate);
  UnsafeSetReservedSlot(
    result,
    ITERATOR_HELPER_GENERATOR_SLOT,
    generator
  );

  // Stop at the initial yield point.
  callFunction(GeneratorNext, generator);

  // Step 8.
  return result;
}

/**
 * Iterator.prototype.filter ( predicate )
 *
 * Abstract closure definition.
 *
 * https://tc39.es/proposal-iterator-helpers/#sec-iteratorprototype.filter
 */
function* IteratorFilterGenerator(iterator, nextMethod, predicate) {
  var isReturnCompletion = true;
  try {
    // Initial yield point to handle closing the iterator before the for-of
    // loop has been entered for the first time.
    yield;

    // Not a Return completion when execution continues normally after |yield|.
    isReturnCompletion = false;
  } finally {
    // Call IteratorClose on a Return completion.
    if (isReturnCompletion) {
      IteratorClose(iterator);
    }
  }

  // Step 5.a.
  var counter = 0;

  // Step 5.b.
  for (var value of allowContentIterWithNext(iterator, nextMethod)) {
    // Steps 5.b.i-iii. (Implicit through for-of loop)

    // Step 5.b.iv.
    var selected = callContentFunction(predicate, undefined, value, counter);

    // Step 5.b.v. (Implicit through for-of loop)

    // Step 5.b.vi.
    if (selected) {
      // Step 5.b.vi.1.
      yield value;

      // Step 5.b.vi.2. (Implicit through for-of loop)
    }

    // Step 5.b.vii.
    counter += 1;
  }
}

/**
 * Iterator.prototype.take ( limit )
 *
 * https://tc39.es/proposal-iterator-helpers/#sec-iteratorprototype.take
 */
function IteratorTake(limit) {
  // Step 1.
  var iterator = this;

  // Step 2.
  if (!IsObject(iterator)) {
    ThrowTypeError(JSMSG_OBJECT_REQUIRED, iterator === null ? "null" : typeof iterator);
  }

  // Steps 3-6.
  var integerLimit = std_Math_trunc(limit);
  if (!(integerLimit >= 0)) {
    ThrowRangeError(JSMSG_NEGATIVE_LIMIT);
  }

  // Step 7. (Inlined call to GetIteratorDirect.)
  var nextMethod = iterator.next;

  // Steps 8-10.
  var result = NewIteratorHelper();
  var generator = IteratorTakeGenerator(iterator, nextMethod, integerLimit);
  UnsafeSetReservedSlot(
    result,
    ITERATOR_HELPER_GENERATOR_SLOT,
    generator
  );

  // Stop at the initial yield point.
  callFunction(GeneratorNext, generator);

  // Step 11.
  return result;
}

/**
 * Iterator.prototype.take ( limit )
 *
 * Abstract closure definition.
 *
 * https://tc39.es/proposal-iterator-helpers/#sec-iteratorprototype.take
 */
function* IteratorTakeGenerator(iterator, nextMethod, remaining) {
  var isReturnCompletion = true;
  try {
    // Initial yield point to handle closing the iterator before the for-of
    // loop has been entered for the first time.
    yield;

    // Not a Return completion when execution continues normally after |yield|.
    isReturnCompletion = false;
  } finally {
    // Call IteratorClose on a Return completion.
    if (isReturnCompletion) {
      IteratorClose(iterator);
    }
  }

  // Step 8.a. (Implicit)

  // Step 8.b.i. (Reordered before for-of loop entry)
  if (remaining === 0) {
    IteratorClose(iterator);
    return;
  }

  // Step 8.b.
  for (var value of allowContentIterWithNext(iterator, nextMethod)) {
    // Steps 8.b.iii-iv. (Implicit through for-of loop)

    // Step 8.b.v.
    yield value;

    // Step 8.b.vi. (Implicit through for-of loop)

    // Steps 8.b.i-ii. (Reordered)
    if (--remaining === 0) {
      // |break| implicitly calls IteratorClose.
      break;
    }
  }
}

/**
 * Iterator.prototype.drop ( limit )
 *
 * https://tc39.es/proposal-iterator-helpers/#sec-iteratorprototype.drop
 */
function IteratorDrop(limit) {
  // Step 1.
  var iterator = this;

  // Step 2.
  if (!IsObject(iterator)) {
    ThrowTypeError(JSMSG_OBJECT_REQUIRED, iterator === null ? "null" : typeof iterator);
  }

  // Steps 3-6.
  var integerLimit = std_Math_trunc(limit);
  if (!(integerLimit >= 0)) {
    ThrowRangeError(JSMSG_NEGATIVE_LIMIT);
  }

  // Step 7. (Inlined call to GetIteratorDirect.)
  var nextMethod = iterator.next;

  // Steps 8-10.
  var result = NewIteratorHelper();
  var generator = IteratorDropGenerator(iterator, nextMethod, integerLimit);
  UnsafeSetReservedSlot(
    result,
    ITERATOR_HELPER_GENERATOR_SLOT,
    generator
  );

  // Stop at the initial yield point.
  callFunction(GeneratorNext, generator);

  // Step 11.
  return result;
}

/**
 * Iterator.prototype.drop ( limit )
 *
 * Abstract closure definition.
 *
 * https://tc39.es/proposal-iterator-helpers/#sec-iteratorprototype.drop
 */
function* IteratorDropGenerator(iterator, nextMethod, remaining) {
  var isReturnCompletion = true;
  try {
    // Initial yield point to handle closing the iterator before the for-of
    // loop has been entered for the first time.
    yield;

    // Not a Return completion when execution continues normally after |yield|.
    isReturnCompletion = false;
  } finally {
    // Call IteratorClose on a Return completion.
    if (isReturnCompletion) {
      IteratorClose(iterator);
    }
  }

  // Step 8.a. (Implicit)

  // Steps 8.b-c.
  for (var value of allowContentIterWithNext(iterator, nextMethod)) {
    // Step 8.b.i.
    if (remaining-- <= 0) {
      // Steps 8.b.ii-iii. (Implicit through for-of loop)
      // Steps 8.c.i-ii. (Implicit through for-of loop)

      // Step 8.c.iii.
      yield value;

      // Step 8.c.iv. (Implicit through for-of loop)
    }
  }
}

/**
 * Iterator.prototype.flatMap ( mapper )
 *
 * https://tc39.es/proposal-iterator-helpers/#sec-iteratorprototype.flatmap
 */
function IteratorFlatMap(mapper) {
  // Step 1.
  var iterator = this;

  // Step 2.
  if (!IsObject(iterator)) {
    ThrowTypeError(JSMSG_OBJECT_REQUIRED, iterator === null ? "null" : typeof iterator);
  }

  // Step 3.
  if (!IsCallable(mapper)) {
    ThrowTypeError(JSMSG_NOT_FUNCTION, DecompileArg(0, mapper));
  }

  // Step 4. (Inlined call to GetIteratorDirect.)
  var nextMethod = iterator.next;

  // Steps 5-7.
  var result = NewIteratorHelper();
  var generator = IteratorFlatMapGenerator(iterator, nextMethod, mapper);
  UnsafeSetReservedSlot(
    result,
    ITERATOR_HELPER_GENERATOR_SLOT,
    generator
  );

  // Stop at the initial yield point.
  callFunction(GeneratorNext, generator);

  // Step 8.
  return result;
}

/**
 * Iterator.prototype.flatMap ( mapper )
 *
 * https://tc39.es/proposal-iterator-helpers/#sec-iteratorprototype.flatmap
 */
function* IteratorFlatMapGenerator(iterator, nextMethod, mapper) {
  var isReturnCompletion = true;
  try {
    // Initial yield point to handle closing the iterator before the for-of
    // loop has been entered for the first time.
    yield;

    // Not a Return completion when execution continues normally after |yield|.
    isReturnCompletion = false;
  } finally {
    // Call IteratorClose on a Return completion.
    if (isReturnCompletion) {
      IteratorClose(iterator);
    }
  }

  // Step 5.a.
  var counter = 0;

  // Step 5.b.
  for (var value of allowContentIterWithNext(iterator, nextMethod)) {
    // Steps 5.b.i-iii. (Implicit through for-of loop)

    // Step 5.b.iv.
    var mapped = callContentFunction(mapper, undefined, value, counter);

    // Step 5.b.v. (Implicit through for-of loop)

    // Steps 5.b.vi.
    var innerIterator = GetIteratorFlattenable(mapped, /* rejectStrings= */ true);
    var innerIteratorNextMethod = innerIterator.next;

    // Step 5.b.vii. (Implicit through for-of loop)

    // Steps 5.b.viii-ix.
    for (var innerValue of allowContentIterWithNext(innerIterator, innerIteratorNextMethod)) {
      // Steps 5.b.ix.1-3 and 5.b.ix.4.a-b. (Implicit through for-of loop)

      // Step 5.b.ix.4.c.
      yield innerValue;

      // Step 5.b.ix.4.d. (Implicit through for-of loop)
    }

    // Step 5.b.x.
    counter += 1;
  }
}

/**
 * Iterator.prototype.reduce ( reducer [ , initialValue ] )
 *
 * https://tc39.es/proposal-iterator-helpers/#sec-iteratorprototype.reduce
 */
function IteratorReduce(reducer /*, initialValue*/) {
  // Step 1.
  var iterator = this;

  // Step 2.
  if (!IsObject(iterator)) {
    ThrowTypeError(JSMSG_OBJECT_REQUIRED, iterator === null ? "null" : typeof iterator);
  }

  // Step 3.
  if (!IsCallable(reducer)) {
    ThrowTypeError(JSMSG_NOT_FUNCTION, DecompileArg(0, reducer));
  }

  // Step 4. (Inlined call to GetIteratorDirect.)
  var nextMethod = iterator.next;

  // Steps 5-6.
  var accumulator;
  var counter;
  if (ArgumentsLength() === 1) {
    // Steps 5.a-d. (Moved below.)
    counter = -1;
  } else {
    // Step 6.a.
    accumulator = GetArgument(1);

    // Step 6.b.
    counter = 0;
  }

  // Step 7.
  for (var value of allowContentIterWithNext(iterator, nextMethod)) {
    if (counter < 0) {
      // Step 5. (Reordered steps to compute initial accumulator.)

      // Step 5.c.
      accumulator = value;

      // Step 5.d.
      counter = 1;
    } else {
      // Steps 7.a-c and 7.e. (Implicit through for-of loop)

      // Steps 7.d and 7.f-g.
      accumulator = callContentFunction(reducer, undefined, accumulator, value, counter++);
    }
  }

  // Step 5.b.
  if (counter < 0) {
    ThrowTypeError(JSMSG_EMPTY_ITERATOR_REDUCE);
  }

  // Step 7.b.
  return accumulator;
}

/**
 * Iterator.prototype.toArray ( )
 *
 * https://tc39.es/proposal-iterator-helpers/#sec-iteratorprototype.toarray
 */
function IteratorToArray() {
  // Step 1.
  var iterator = this;

  // Step 2.
  if (!IsObject(iterator)) {
    ThrowTypeError(JSMSG_OBJECT_REQUIRED, iterator === null ? "null" : typeof iterator);
  }

  // Step 3. (Inlined call to GetIteratorDirect.)
  var nextMethod = iterator.next;

  // Steps 4-5.
  return [...allowContentIterWithNext(iterator, nextMethod)];
}

/**
 * Iterator.prototype.forEach ( fn )
 *
 * https://tc39.es/proposal-iterator-helpers/#sec-iteratorprototype.foreach
 */
function IteratorForEach(fn) {
  // Step 1.
  var iterator = this;

  // Step 2.
  if (!IsObject(iterator)) {
    ThrowTypeError(JSMSG_OBJECT_REQUIRED, iterator === null ? "null" : typeof iterator);
  }

  // Step 3.
  if (!IsCallable(fn)) {
    ThrowTypeError(JSMSG_NOT_FUNCTION, DecompileArg(0, fn));
  }

  // Step 4. (Inlined call to GetIteratorDirect.)
  var nextMethod = iterator.next;

  // Step 5.
  var counter = 0;

  // Step 6.
  for (var value of allowContentIterWithNext(iterator, nextMethod)) {
    // Steps 6.a-c. (Implicit through for-of loop)

    // Steps 6.d and 6.f.
    callContentFunction(fn, undefined, value, counter++);

    // Step 6.e. (Implicit through for-of loop)
  }
}

/**
 * Iterator.prototype.some ( predicate )
 *
 * https://tc39.es/proposal-iterator-helpers/#sec-iteratorprototype.some
 */
function IteratorSome(predicate) {
  // Step 1.
  var iterator = this;

  // Step 2.
  if (!IsObject(iterator)) {
    ThrowTypeError(JSMSG_OBJECT_REQUIRED, iterator === null ? "null" : typeof iterator);
  }

  // Step 3.
  if (!IsCallable(predicate)) {
    ThrowTypeError(JSMSG_NOT_FUNCTION, DecompileArg(0, predicate));
  }

  // Step 4. (Inlined call to GetIteratorDirect.)
  var nextMethod = iterator.next;

  // Step 5.
  var counter = 0;

  // Step 6.
  for (var value of allowContentIterWithNext(iterator, nextMethod)) {
    // Steps 6.a-c. (Implicit through for-of loop)

    // Steps 6.d-g.
    if (callContentFunction(predicate, undefined, value, counter++)) {
      return true;
    }
  }

  // Step 6.b.
  return false;
}

/**
 * Iterator.prototype.every ( predicate )
 *
 * https://tc39.es/proposal-iterator-helpers/#sec-iteratorprototype.every
 */
function IteratorEvery(predicate) {
  // Step 1.
  var iterator = this;

  // Step 2.
  if (!IsObject(iterator)) {
    ThrowTypeError(JSMSG_OBJECT_REQUIRED, iterator === null ? "null" : typeof iterator);
  }

  // Step 3.
  if (!IsCallable(predicate)) {
    ThrowTypeError(JSMSG_NOT_FUNCTION, DecompileArg(0, predicate));
  }

  // Step 4. (Inlined call to GetIteratorDirect.)
  var nextMethod = iterator.next;

  // Step 5.
  var counter = 0;

  // Step 6.
  for (var value of allowContentIterWithNext(iterator, nextMethod)) {
    // Steps 6.a-c. (Implicit through for-of loop)

    // Steps 6.d-g.
    if (!callContentFunction(predicate, undefined, value, counter++)) {
      return false;
    }
  }

  // Step 6.b.
  return true;
}

/**
 * Iterator.prototype.find ( predicate )
 *
 * https://tc39.es/proposal-iterator-helpers/#sec-iteratorprototype.find
 */
function IteratorFind(predicate) {
  // Step 1.
  var iterator = this;

  // Step 2.
  if (!IsObject(iterator)) {
    ThrowTypeError(JSMSG_OBJECT_REQUIRED, iterator === null ? "null" : typeof iterator);
  }

  // Step 3.
  if (!IsCallable(predicate)) {
    ThrowTypeError(JSMSG_NOT_FUNCTION, DecompileArg(0, predicate));
  }

  // Step 4. (Inlined call to GetIteratorDirect.)
  var nextMethod = iterator.next;

  // Step 5.
  var counter = 0;

  // Step 6.
  for (var value of allowContentIterWithNext(iterator, nextMethod)) {
    // Steps 6.a-c. (Implicit through for-of loop)

    // Steps 6.d-g.
    if (callContentFunction(predicate, undefined, value, counter++)) {
      return value;
    }
  }
}

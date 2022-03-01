/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

function IteratorIdentity() {
  return this;
}

/* ECMA262 7.2.7 */
function IteratorNext(iteratorRecord, value) {
  // Steps 1-2.
  const result = (arguments.length < 2
      ? callContentFunction(iteratorRecord.nextMethod, iteratorRecord.iterator)
      : callContentFunction(iteratorRecord.nextMethod, iteratorRecord.iterator, value));
  // Step 3.
  if (!IsObject(result)) {
    ThrowTypeError(JSMSG_OBJECT_REQUIRED, result);
  }
  // Step 4.
  return result;
}

/* ECMA262 7.4.6 */
function IteratorClose(iteratorRecord, value) {
  // Step 3.
  const iterator = iteratorRecord.iterator;
  // Step 4.
  const returnMethod = iterator.return;
  // Step 5.
  if (returnMethod !== undefined && returnMethod !== null) {
    const result = callContentFunction(returnMethod, iterator);
    // Step 8.
    if (!IsObject(result)) {
      ThrowTypeError(JSMSG_OBJECT_REQUIRED, DecompileArg(0, result));
    }
  }
  // Step 5b & 9.
  return value;
}

/* Iterator Helpers proposal 1.1.1 */
function GetIteratorDirect(obj) {
  // Step 1.
  if (!IsObject(obj)) {
    ThrowTypeError(JSMSG_OBJECT_REQUIRED, DecompileArg(0, obj));
  }

  // Step 2.
  const nextMethod = obj.next;
  // Step 3.
  if (!IsCallable(nextMethod)) {
    ThrowTypeError(JSMSG_NOT_FUNCTION, DecompileArg(0, nextMethod));
  }

  // Steps 4-5.
  return {
    iterator: obj,
    nextMethod,
    done: false,
  };
}

function GetIteratorDirectWrapper(obj) {
  // Step 1.
  if (!IsObject(obj)) {
    ThrowTypeError(JSMSG_OBJECT_REQUIRED, obj);
  }

  // Step 2.
  const nextMethod = obj.next;
  // Step 3.
  if (!IsCallable(nextMethod)) {
    ThrowTypeError(JSMSG_NOT_FUNCTION, nextMethod);
  }

  // Steps 4-5.
  return {
    // Use a named function expression instead of a method definition, so
    // we don't create an inferred name for this function at runtime.
    [GetBuiltinSymbol("iterator")]: function IteratorMethod() {
      return this;
    },
    next(value) {
      return callContentFunction(nextMethod, obj, value);
    },
    return(value) {
      const returnMethod = obj.return;
      if (returnMethod !== undefined && returnMethod !== null) {
        return callContentFunction(returnMethod, obj, value);
      }
      return {done: true, value};
    },
  };
}

/* Iterator Helpers proposal 1.1.2 */
function IteratorStep(iteratorRecord, value) {
  // Steps 2-3.
  let result;
  if (arguments.length === 2) {
    result = callContentFunction(
      iteratorRecord.nextMethod,
      iteratorRecord.iterator,
      value
    );
  } else {
    result = callContentFunction(
      iteratorRecord.nextMethod,
      iteratorRecord.iterator
    );
  }

  // IteratorNext Step 3.
  if (!IsObject(result)) {
    ThrowTypeError(JSMSG_OBJECT_REQUIRED, DecompileArg(0, result));
  }

  // Steps 4-6.
  return result.done ? false : result;
}

/* Iterator Helpers proposal 2.1.3.3.1 */
function IteratorFrom(O) {
  // Step 1.
  const usingIterator = O[GetBuiltinSymbol("iterator")];

  let iteratorRecord;
  // Step 2.
  if (usingIterator !== undefined && usingIterator !== null) {
    // Step a.
    // Inline call to GetIterator.
    const iterator = callContentFunction(usingIterator, O);
    iteratorRecord = GetIteratorDirect(iterator);
    // Step b-c.
    if (iteratorRecord.iterator instanceof GetBuiltinConstructor("Iterator")) {
      return iteratorRecord.iterator;
    }
  } else {
    // Step 3.
    iteratorRecord = GetIteratorDirect(O);
  }

  // Step 4.
  const wrapper = NewWrapForValidIterator();
  // Step 5.
  UnsafeSetReservedSlot(wrapper, ITERATED_SLOT, iteratorRecord);
  // Step 6.
  return wrapper;
}

/* Iterator Helpers proposal 2.1.3.3.1.1.1 */
function WrapForValidIteratorNext(value) {
  // Step 1-2.
  let O;
  if (!IsObject(this) || (O = GuardToWrapForValidIterator(this)) === null) {
    if (arguments.length === 0) {
      return callFunction(CallWrapForValidIteratorMethodIfWrapped, this,
                          "WrapForValidIteratorNext");
    }
    return callFunction(CallWrapForValidIteratorMethodIfWrapped, this,
                        value, "WrapForValidIteratorNext");
  }
  const iterated = UnsafeGetReservedSlot(O, ITERATED_SLOT);
  // Step 3.
  let result;
  if (arguments.length === 0) {
    result = callContentFunction(iterated.nextMethod, iterated.iterator);
  } else { // Step 4.
    result = callContentFunction(iterated.nextMethod, iterated.iterator, value);
  }
  // Inlined from IteratorNext.
  if (!IsObject(result))
    ThrowTypeError(JSMSG_OBJECT_REQUIRED, DecompileArg(0, result));
  return result;
}

/* Iterator Helpers proposal 2.1.3.3.1.1.2 */
function WrapForValidIteratorReturn(value) {
  // Step 1-2.
  let O;
  if (!IsObject(this) || (O = GuardToWrapForValidIterator(this)) === null) {
    return callFunction(CallWrapForValidIteratorMethodIfWrapped, this,
                        value, "WrapForValidIteratorReturn");
  }
  const iterated = UnsafeGetReservedSlot(O, ITERATED_SLOT);

  // Step 3.
  // Inline call to IteratorClose.
  const iterator = iterated.iterator;
  const returnMethod = iterator.return;
  if (returnMethod !== undefined && returnMethod !== null) {
    let innerResult = callContentFunction(returnMethod, iterator);
    if (!IsObject(innerResult)) {
      ThrowTypeError(JSMSG_OBJECT_REQUIRED, DecompileArg(0, innerResult));
    }
  }
  // Step 4.
  return {
    done: true,
    value,
  };
}

/* Iterator Helpers proposal 2.1.3.3.1.1.3 */
function WrapForValidIteratorThrow(value) {
  // Step 1-2.
  let O;
  if (!IsObject(this) || (O = GuardToWrapForValidIterator(this)) === null) {
    return callFunction(CallWrapForValidIteratorMethodIfWrapped, this,
                        value, "WrapForValidIteratorThrow");
  }
  const iterated = UnsafeGetReservedSlot(O, ITERATED_SLOT);
  // Step 3.
  const iterator = iterated.iterator;
  // Step 4.
  const throwMethod = iterator.throw;
  // Step 5.
  if (throwMethod === undefined || throwMethod === null) {
    throw value;
  }
  // Step 6.
  return callContentFunction(throwMethod, iterator, value);
}

/* Iterator Helper object prototype methods. */
function IteratorHelperNext(value) {
  let O;
  if (!IsObject(this) || (O = GuardToIteratorHelper(this)) === null) {
    return callFunction(CallIteratorHelperMethodIfWrapped, this,
                        value, "IteratorHelperNext");
  }
  const generator = UnsafeGetReservedSlot(O, ITERATOR_HELPER_GENERATOR_SLOT);
  return callContentFunction(GeneratorNext, generator, value);
}

function IteratorHelperReturn(value) {
  let O;
  if (!IsObject(this) || (O = GuardToIteratorHelper(this)) === null) {
    return callFunction(CallIteratorHelperMethodIfWrapped, this,
                        value, "IteratorHelperReturn");
  }
  const generator = UnsafeGetReservedSlot(O, ITERATOR_HELPER_GENERATOR_SLOT);
  return callContentFunction(GeneratorReturn, generator, value);
}

function IteratorHelperThrow(value) {
  let O;
  if (!IsObject(this) || (O = GuardToIteratorHelper(this)) === null) {
    return callFunction(CallIteratorHelperMethodIfWrapped, this,
                        value, "IteratorHelperThrow");
  }
  const generator = UnsafeGetReservedSlot(O, ITERATOR_HELPER_GENERATOR_SLOT);
  return callContentFunction(GeneratorThrow, generator, value);
}

// Lazy %Iterator.prototype% methods
// Iterator Helpers proposal 2.1.5.2-2.1.5.7
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
// instance in order to move it to it's first yield point. This is done so that
// if the return or throw methods are called on the IteratorHelper before next
// has been called, we can catch them in the try and use the finally block to
// close the source iterator.
//
// The needClose flag is used to track when the source iterator should be closed
// following an exception being thrown within the generator, corresponding to
// whether or not the abrupt completions in the spec are being passed back to
// the caller (when needClose is false) or handled with IfAbruptCloseIterator
// (when needClose is true).

/* Iterator Helpers proposal 2.1.5.2 Prelude */
function IteratorMap(mapper) {
  // Step 1.
  const iterated = GetIteratorDirect(this);

  // Step 2.
  if (!IsCallable(mapper)) {
    ThrowTypeError(JSMSG_NOT_FUNCTION, DecompileArg(0, mapper));
  }

  const iteratorHelper = NewIteratorHelper();
  const generator = IteratorMapGenerator(iterated, mapper);
  callContentFunction(GeneratorNext, generator);
  UnsafeSetReservedSlot(iteratorHelper, ITERATOR_HELPER_GENERATOR_SLOT, generator);
  return iteratorHelper;
}

/* Iterator Helpers proposal 2.1.5.2 Body */
function* IteratorMapGenerator(iterated, mapper) {
  // Step 1.
  let lastValue;
  // Step 2.
  let needClose = true;
  try {
    yield;
    needClose = false;

    for (let next = IteratorStep(iterated, lastValue);
        next;
        next = IteratorStep(iterated, lastValue)) {
      // Step c.
      const value = next.value;

      // Steps d-g.
      needClose = true;
      lastValue = yield callContentFunction(mapper, undefined, value);
      needClose = false;
    }
  } finally {
    if (needClose) {
      IteratorClose(iterated);
    }
  }
}

/* Iterator Helpers proposal 2.1.5.3 Prelude */
function IteratorFilter(filterer) {
  // Step 1.
  const iterated = GetIteratorDirect(this);

  // Step 2.
  if (!IsCallable(filterer)) {
    ThrowTypeError(JSMSG_NOT_FUNCTION, DecompileArg(0, filterer));
  }

  const iteratorHelper = NewIteratorHelper();
  const generator = IteratorFilterGenerator(iterated, filterer);
  callContentFunction(GeneratorNext, generator);
  UnsafeSetReservedSlot(iteratorHelper, ITERATOR_HELPER_GENERATOR_SLOT, generator);
  return iteratorHelper;
}

/* Iterator Helpers proposal 2.1.5.3 Body */
function* IteratorFilterGenerator(iterated, filterer) {
  // Step 1.
  let lastValue;
  // Step 2.
  let needClose = true;
  try {
    yield;
    needClose = false;

    for (let next = IteratorStep(iterated, lastValue);
        next;
        next = IteratorStep(iterated, lastValue)) {
      // Step c.
      const value = next.value;

      // Steps d-g.
      needClose = true;
      if (callContentFunction(filterer, undefined, value)) {
        lastValue = yield value;
      }
      needClose = false;
    }
  } finally {
    if (needClose) {
      IteratorClose(iterated);
    }
  }
}

/* Iterator Helpers proposal 2.1.5.4 Prelude */
function IteratorTake(limit) {
  // Step 1.
  const iterated = GetIteratorDirect(this);

  // Step 2.
  const remaining = ToInteger(limit);
  // Step 3.
  if (remaining < 0) {
    ThrowRangeError(JSMSG_NEGATIVE_LIMIT);
  }

  const iteratorHelper = NewIteratorHelper();
  const generator = IteratorTakeGenerator(iterated, remaining);
  callContentFunction(GeneratorNext, generator);
  UnsafeSetReservedSlot(iteratorHelper, ITERATOR_HELPER_GENERATOR_SLOT, generator);
  return iteratorHelper;
}

/* Iterator Helpers proposal 2.1.5.4 Body */
function* IteratorTakeGenerator(iterated, remaining) {
  // Step 1.
  let lastValue;
  // Step 2.
  let needClose = true;
  try {
    yield;
    needClose = false;

    for (; remaining > 0; remaining--) {
      const next = IteratorStep(iterated, lastValue);
      if (!next) {
        return;
      }

      const value = next.value;
      needClose = true;
      lastValue = yield value;
      needClose = false;
    }
  } finally {
    if (needClose) {
      IteratorClose(iterated);
    }
  }

  IteratorClose(iterated);
}

/* Iterator Helpers proposal 2.1.5.5 Prelude */
function IteratorDrop(limit) {
  // Step 1.
  const iterated = GetIteratorDirect(this);

  // Step 2.
  const remaining = ToInteger(limit);
  // Step 3.
  if (remaining < 0) {
    ThrowRangeError(JSMSG_NEGATIVE_LIMIT);
  }

  const iteratorHelper = NewIteratorHelper();
  const generator = IteratorDropGenerator(iterated, remaining);
  callContentFunction(GeneratorNext, generator);
  UnsafeSetReservedSlot(iteratorHelper, ITERATOR_HELPER_GENERATOR_SLOT, generator);
  return iteratorHelper;
}

/* Iterator Helpers proposal 2.1.5.5 Body */
function* IteratorDropGenerator(iterated, remaining) {
  let needClose = true;
  try {
    yield;
    needClose = false;

    // Step 1.
    for (; remaining > 0; remaining--) {
      if (!IteratorStep(iterated)) {
        return;
      }
    }

    // Step 2.
    let lastValue;
    // Step 3.
    for (let next = IteratorStep(iterated, lastValue);
        next;
        next = IteratorStep(iterated, lastValue)) {
      // Steps c-d.
      const value = next.value;

      needClose = true;
      lastValue = yield value;
      needClose = false;
    }
  } finally {
    if (needClose) {
      IteratorClose(iterated);
    }
  }
}

/* Iterator Helpers proposal 2.1.5.6 Prelude */
function IteratorAsIndexedPairs() {
  // Step 1.
  const iterated = GetIteratorDirect(this);

  const iteratorHelper = NewIteratorHelper();
  const generator = IteratorAsIndexedPairsGenerator(iterated);
  callContentFunction(GeneratorNext, generator);
  UnsafeSetReservedSlot(iteratorHelper, ITERATOR_HELPER_GENERATOR_SLOT, generator);
  return iteratorHelper;
}

/* Iterator Helpers proposal 2.1.5.6 Body */
function* IteratorAsIndexedPairsGenerator(iterated) {
  // Step 2.
  let lastValue;
  // Step 3.
  let needClose = true;
  try {
    yield;
    needClose = false;

    for (let next = IteratorStep(iterated, lastValue), index = 0;
        next;
        next = IteratorStep(iterated, lastValue), index++) {
      // Steps c-d.
      const value = next.value;

      needClose = true;
      lastValue = yield [index, value];
      needClose = false;
    }
  } finally {
    if (needClose) {
      IteratorClose(iterated);
    }
  }
}

/* Iterator Helpers proposal 2.1.5.7 Prelude */
function IteratorFlatMap(mapper) {
  // Step 1.
  const iterated = GetIteratorDirect(this);

  // Step 2.
  if (!IsCallable(mapper)) {
    ThrowTypeError(JSMSG_NOT_FUNCTION, DecompileArg(0, mapper));
  }

  const iteratorHelper = NewIteratorHelper();
  const generator = IteratorFlatMapGenerator(iterated, mapper);
  callContentFunction(GeneratorNext, generator);
  UnsafeSetReservedSlot(iteratorHelper, ITERATOR_HELPER_GENERATOR_SLOT, generator);
  return iteratorHelper;
}

/* Iterator Helpers proposal 2.1.5.7 Body */
function* IteratorFlatMapGenerator(iterated, mapper) {
  // Step 1.
  let needClose = true;
  try {
    yield;
    needClose = false;

    for (let next = IteratorStep(iterated);
        next;
        next = IteratorStep(iterated)) {
      // Step c.
      const value = next.value;

      needClose = true;
      // Step d.
      const mapped = callContentFunction(mapper, undefined, value);
      // Steps f-i.
      for (const innerValue of allowContentIter(mapped)) {
        yield innerValue;
      }
      needClose = false;
    }
  } finally {
    if (needClose) {
      IteratorClose(iterated);
    }
  }
}

/* Iterator Helpers proposal 2.1.5.8 */
function IteratorReduce(reducer/*, initialValue*/) {
  // Step 1.
  const iterated = GetIteratorDirectWrapper(this);

  // Step 2.
  if (!IsCallable(reducer)) {
    ThrowTypeError(JSMSG_NOT_FUNCTION, DecompileArg(0, reducer));
  }

  // Step 3.
  let accumulator;
  if (arguments.length === 1) {
    // Step a.
    const next = callContentFunction(iterated.next, iterated);
    if (!IsObject(next)) {
      ThrowTypeError(JSMSG_OBJECT_REQUIRED, DecompileArg(0, next));
    }
    // Step b.
    if (next.done) {
      ThrowTypeError(JSMSG_EMPTY_ITERATOR_REDUCE);
    }
    // Step c.
    accumulator = next.value;
  } else {
    // Step 4.
    accumulator = arguments[1];
  }

  // Step 5.
  for (const value of allowContentIter(iterated)) {
    accumulator = callContentFunction(reducer, undefined, accumulator, value);
  }
  return accumulator;
}

/* Iterator Helpers proposal 2.1.5.9 */
function IteratorToArray() {
  // Step 1.
  const iterated = {[GetBuiltinSymbol("iterator")]: () => this};
  // Steps 2-3.
  return [...allowContentIter(iterated)];
}

/* Iterator Helpers proposal 2.1.5.10 */
function IteratorForEach(fn) {
  // Step 1.
  const iterated = GetIteratorDirectWrapper(this);

  // Step 2.
  if (!IsCallable(fn)) {
    ThrowTypeError(JSMSG_NOT_FUNCTION, DecompileArg(0, fn));
  }

  // Step 3.
  for (const value of allowContentIter(iterated)) {
    callContentFunction(fn, undefined, value);
  }
}

/* Iterator Helpers proposal 2.1.5.11 */
function IteratorSome(fn) {
  // Step 1.
  const iterated = GetIteratorDirectWrapper(this);

  // Step 2.
  if (!IsCallable(fn)) {
    ThrowTypeError(JSMSG_NOT_FUNCTION, DecompileArg(0, fn));
  }

  // Step 3.
  for (const value of allowContentIter(iterated)) {
    // Steps d-f.
    if (callContentFunction(fn, undefined, value)) {
      return true;
    }
  }
  // Step 3b.
  return false;
}

/* Iterator Helpers proposal 2.1.5.12 */
function IteratorEvery(fn) {
  // Step 1.
  const iterated = GetIteratorDirectWrapper(this);

  // Step 2.
  if (!IsCallable(fn)) {
    ThrowTypeError(JSMSG_NOT_FUNCTION, DecompileArg(0, fn));
  }

  // Step 3.
  for (const value of allowContentIter(iterated)) {
    // Steps d-f.
    if (!callContentFunction(fn, undefined, value)) {
      return false;
    }
  }
  // Step 3b.
  return true;
}

/* Iterator Helpers proposal 2.1.5.13 */
function IteratorFind(fn) {
  // Step 1.
  const iterated = GetIteratorDirectWrapper(this);

  // Step 2.
  if (!IsCallable(fn)) {
    ThrowTypeError(JSMSG_NOT_FUNCTION, DecompileArg(0, fn));
  }

  // Step 3.
  for (const value of allowContentIter(iterated)) {
    // Steps d-f.
    if (callContentFunction(fn, undefined, value)) {
      return value;
    }
  }
}

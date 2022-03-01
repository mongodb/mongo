/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

function AsyncIteratorIdentity() {
    return this;
}

function AsyncGeneratorNext(val) {
    assert(IsAsyncGeneratorObject(this),
           "ThisArgument must be a generator object for async generators");
    return resumeGenerator(this, val, "next");
}

function AsyncGeneratorThrow(val) {
    assert(IsAsyncGeneratorObject(this),
           "ThisArgument must be a generator object for async generators");
    return resumeGenerator(this, val, "throw");
}

function AsyncGeneratorReturn(val) {
    assert(IsAsyncGeneratorObject(this),
           "ThisArgument must be a generator object for async generators");
    return resumeGenerator(this, val, "return");
}

/* ECMA262 7.4.7 AsyncIteratorClose */
async function AsyncIteratorClose(iteratorRecord, value) {
  // Step 3.
  const iterator = iteratorRecord.iterator;
  // Step 4.
  const returnMethod = iterator.return;
  // Step 5.
  if (returnMethod !== undefined && returnMethod !== null) {
    const result = await callContentFunction(returnMethod, iterator);
    // Step 8.
    if (!IsObject(result)) {
      ThrowTypeError(JSMSG_OBJECT_REQUIRED, DecompileArg(0, result));
    }
  }
  // Step 5b & 9.
  return value;
}

/* Iterator Helpers proposal 1.1.1 */
function GetAsyncIteratorDirectWrapper(obj) {
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
    [GetBuiltinSymbol("asyncIterator")]: function AsyncIteratorMethod() {
      return this;
    },
    next(value) {
      return callContentFunction(nextMethod, obj, value);
    },
    async return(value) {
      const returnMethod = obj.return;
      if (returnMethod !== undefined && returnMethod !== null) {
        return callContentFunction(returnMethod, obj, value);
      }
      return {done: true, value};
    },
  };
}

/* AsyncIteratorHelper object prototype methods. */
function AsyncIteratorHelperNext(value) {
  let O;
  if (!IsObject(this) || (O = GuardToAsyncIteratorHelper(this)) === null) {
    return callFunction(CallAsyncIteratorHelperMethodIfWrapped, this,
                        value, "AsyncIteratorHelperNext");
  }
  const generator = UnsafeGetReservedSlot(O, ASYNC_ITERATOR_HELPER_GENERATOR_SLOT);
  return callFunction(IntrinsicAsyncGeneratorNext, generator, value);
}

function AsyncIteratorHelperReturn(value) {
  let O;
  if (!IsObject(this) || (O = GuardToAsyncIteratorHelper(this)) === null) {
    return callFunction(CallAsyncIteratorHelperMethodIfWrapped, this,
                        value, "AsyncIteratorHelperReturn");
  }
  const generator = UnsafeGetReservedSlot(O, ASYNC_ITERATOR_HELPER_GENERATOR_SLOT);
  return callFunction(IntrinsicAsyncGeneratorReturn, generator, value);
}

function AsyncIteratorHelperThrow(value) {
  let O;
  if (!IsObject(this) || (O = GuardToAsyncIteratorHelper(this)) === null) {
    return callFunction(CallAsyncIteratorHelperMethodIfWrapped, this,
                        value, "AsyncIteratorHelperThrow");
  }
  const generator = UnsafeGetReservedSlot(O, ASYNC_ITERATOR_HELPER_GENERATOR_SLOT);
  return callFunction(IntrinsicAsyncGeneratorThrow, generator, value);
}

// AsyncIterator lazy Iterator Helper methods
// Iterator Helpers proposal 2.1.6.2-2.1.6.7
//
// The AsyncIterator lazy methods are structured closely to how the Iterator
// lazy methods are. See builtin/Iterator.js for the reasoning.

/* Iterator Helpers proposal 2.1.6.2 Prelude */
function AsyncIteratorMap(mapper) {
  // Step 1.
  const iterated = GetIteratorDirect(this);

  // Step 2.
  if (!IsCallable(mapper)) {
    ThrowTypeError(JSMSG_NOT_FUNCTION, DecompileArg(0, mapper));
  }

  const iteratorHelper = NewAsyncIteratorHelper();
  const generator = AsyncIteratorMapGenerator(iterated, mapper);
  callFunction(IntrinsicAsyncGeneratorNext, generator);
  UnsafeSetReservedSlot(iteratorHelper, ASYNC_ITERATOR_HELPER_GENERATOR_SLOT, generator);
  return iteratorHelper;
}

/* Iterator Helpers proposal 2.1.6.2 Body */
async function* AsyncIteratorMapGenerator(iterated, mapper) {
  // Step 1.
  let lastValue;
  // Step 2.
  let needClose = true;
  try {
    yield;
    needClose = false;

    for (let next = await IteratorNext(iterated, lastValue);
        !next.done;
        next = await IteratorNext(iterated, lastValue)) {
      // Step c.
      const value = next.value;

      // Steps d-i.
      needClose = true;
      lastValue = yield callContentFunction(mapper, undefined, value);
      needClose = false;
    }
  } finally {
    if (needClose) {
      AsyncIteratorClose(iterated);
    }
  }
}

/* Iterator Helpers proposal 2.1.6.3 Prelude */
function AsyncIteratorFilter(filterer) {
  // Step 1.
  const iterated = GetIteratorDirect(this);

  // Step 2.
  if (!IsCallable(filterer)) {
    ThrowTypeError(JSMSG_NOT_FUNCTION, DecompileArg(0, filterer));
  }

  const iteratorHelper = NewAsyncIteratorHelper();
  const generator = AsyncIteratorFilterGenerator(iterated, filterer);
  callFunction(IntrinsicAsyncGeneratorNext, generator);
  UnsafeSetReservedSlot(iteratorHelper, ASYNC_ITERATOR_HELPER_GENERATOR_SLOT, generator);
  return iteratorHelper;
}

/* Iterator Helpers proposal 2.1.6.3 Body */
async function* AsyncIteratorFilterGenerator(iterated, filterer) {
  // Step 1.
  let lastValue;
  // Step 2.
  let needClose = true;
  try {
    yield;
    needClose = false;

    for (let next = await IteratorNext(iterated, lastValue);
        !next.done;
        next = await IteratorNext(iterated, lastValue)) {
      // Step c.
      const value = next.value;

      // Steps d-h.
      needClose = true;
      if (await callContentFunction(filterer, undefined, value)) {
        lastValue = yield value;
      }
      needClose = false;
    }
  } finally {
    if (needClose) {
      AsyncIteratorClose(iterated);
    }
  }
}

/* Iterator Helpers proposal 2.1.6.4 Prelude */
function AsyncIteratorTake(limit) {
  // Step 1.
  const iterated = GetIteratorDirect(this);

  // Step 2.
  const remaining = ToInteger(limit);
  // Step 3.
  if (remaining < 0) {
    ThrowRangeError(JSMSG_NEGATIVE_LIMIT);
  }

  const iteratorHelper = NewAsyncIteratorHelper();
  const generator = AsyncIteratorTakeGenerator(iterated, remaining);
  callFunction(IntrinsicAsyncGeneratorNext, generator);
  UnsafeSetReservedSlot(iteratorHelper, ASYNC_ITERATOR_HELPER_GENERATOR_SLOT, generator);
  return iteratorHelper;
}

/* Iterator Helpers proposal 2.1.6.4 Body */
async function* AsyncIteratorTakeGenerator(iterated, remaining) {
  // Step 1.
  let lastValue;
  // Step 2.
  let needClose = true;
  try {
    yield;
    needClose = false;

    for (; remaining > 0; remaining--) {
      const next = await IteratorNext(iterated, lastValue);
      if (next.done) {
        return undefined;
      }

      const value = next.value;

      needClose = true;
      lastValue = yield value;
      needClose = false;
    }
  } finally {
    if (needClose) {
      AsyncIteratorClose(iterated, undefined);
    }
  }

  return AsyncIteratorClose(iterated, undefined);
}

/* Iterator Helpers proposal 2.1.6.5 Prelude */
function AsyncIteratorDrop(limit) {
  // Step 1.
  const iterated = GetIteratorDirect(this);

  // Step 2.
  const remaining = ToInteger(limit);
  // Step 3.
  if (remaining < 0) {
    ThrowRangeError(JSMSG_NEGATIVE_LIMIT);
  }

  const iteratorHelper = NewAsyncIteratorHelper();
  const generator = AsyncIteratorDropGenerator(iterated, remaining);
  callFunction(IntrinsicAsyncGeneratorNext, generator);
  UnsafeSetReservedSlot(iteratorHelper, ASYNC_ITERATOR_HELPER_GENERATOR_SLOT, generator);
  return iteratorHelper;
}

/* Iterator Helpers proposal 2.1.6.5 Body */
async function* AsyncIteratorDropGenerator(iterated, remaining) {
  let needClose = true;
  try {
    yield;
    needClose = false;

    // Step 1.
    for (; remaining > 0; remaining--) {
      const next = await IteratorNext(iterated);
      if (next.done) {
        return;
      }
    }

    // Step 2.
    let lastValue;
    // Step 3.
    for (let next = await IteratorNext(iterated, lastValue);
        !next.done;
        next = await IteratorNext(iterated, lastValue)) {
      // Steps c-d.
      const value = next.value;

      needClose = true;
      lastValue = yield value;
      needClose = false;
    }
  } finally {
    if (needClose) {
      AsyncIteratorClose(iterated);
    }
  }
}

/* Iterator Helpers proposal 2.1.6.6 Prelude */
function AsyncIteratorAsIndexedPairs() {
  // Step 1.
  const iterated = GetIteratorDirect(this);

  const iteratorHelper = NewAsyncIteratorHelper();
  const generator = AsyncIteratorAsIndexedPairsGenerator(iterated);
  callFunction(IntrinsicAsyncGeneratorNext, generator);
  UnsafeSetReservedSlot(iteratorHelper, ASYNC_ITERATOR_HELPER_GENERATOR_SLOT, generator);
  return iteratorHelper;
}

/* Iterator Helpers proposal 2.1.6.6 Body */
async function* AsyncIteratorAsIndexedPairsGenerator(iterated) {
  let needClose = true;
  try {
    yield;
    needClose = false;

    // Step 2.
    let lastValue;
    // Step 3.
    for (let next = await IteratorNext(iterated, lastValue), index = 0;
        !next.done;
        next = await IteratorNext(iterated, lastValue), index++) {
      // Steps c-g.
      const value = next.value;

      needClose = true;
      lastValue = yield [index, value];
      needClose = false;
    }
  } finally {
    if (needClose) {
      AsyncIteratorClose(iterated);
    }
  }
}

/* Iterator Helpers proposal 2.1.6.7 Prelude */
function AsyncIteratorFlatMap(mapper) {
  // Step 1.
  const iterated = GetIteratorDirect(this);

  // Step 2.
  if (!IsCallable(mapper)) {
    ThrowTypeError(JSMSG_NOT_FUNCTION, DecompileArg(0, mapper));
  }

  const iteratorHelper = NewAsyncIteratorHelper();
  const generator = AsyncIteratorFlatMapGenerator(iterated, mapper);
  callFunction(IntrinsicAsyncGeneratorNext, generator);
  UnsafeSetReservedSlot(iteratorHelper, ASYNC_ITERATOR_HELPER_GENERATOR_SLOT, generator);
  return iteratorHelper;
}

/* Iterator Helpers proposal 2.1.6.7 Body */
async function* AsyncIteratorFlatMapGenerator(iterated, mapper) {
  let needClose = true;
  try {
    yield;
    needClose = false;

    // Step 1.
    for (let next = await IteratorNext(iterated);
        !next.done;
        next = await IteratorNext(iterated)) {
      // Step c.
      const value = next.value;

      needClose = true;
      // Step d.
      const mapped = await callContentFunction(mapper, undefined, value);
      // Steps f-k.
      for await (const innerValue of allowContentIter(mapped)) {
        yield innerValue;
      }
      needClose = false;
    }
  } finally {
    if (needClose) {
      AsyncIteratorClose(iterated);
    }
  }
}

/* Iterator Helpers proposal 2.1.6.8 */
async function AsyncIteratorReduce(reducer/*, initialValue*/) {
  // Step 1.
  const iterated = GetAsyncIteratorDirectWrapper(this);

  // Step 2.
  if (!IsCallable(reducer)) {
    ThrowTypeError(JSMSG_NOT_FUNCTION, DecompileArg(0, reducer));
  }

  // Step 3.
  let accumulator;
  if (arguments.length === 1) {
    // Step a.
    const next = await callContentFunction(iterated.next, iterated);
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
  for await (const value of allowContentIter(iterated)) {
    // Steps d-h.
    accumulator = await callContentFunction(reducer, undefined, accumulator, value);
  }
  // Step 5b.
  return accumulator;
}

/* Iterator Helpers proposal 2.1.6.9 */
async function AsyncIteratorToArray() {
  // Step 1.
  const iterated = {[GetBuiltinSymbol("asyncIterator")]: () => this};
  // Step 2.
  const items = [];
  let index = 0;
  // Step 3.
  for await (const value of allowContentIter(iterated)) {
    // Step d.
    DefineDataProperty(items, index++, value);
  }
  // Step 3b.
  return items;
}

/* Iterator Helpers proposal 2.1.6.10 */
async function AsyncIteratorForEach(fn) {
  // Step 1.
  const iterated = GetAsyncIteratorDirectWrapper(this);

  // Step 2.
  if (!IsCallable(fn)) {
    ThrowTypeError(JSMSG_NOT_FUNCTION, DecompileArg(0, fn));
  }

  // Step 3.
  for await (const value of allowContentIter(iterated)) {
    // Steps d-g.
    await callContentFunction(fn, undefined, value);
  }
}

/* Iterator Helpers proposal 2.1.6.11 */
async function AsyncIteratorSome(fn) {
  // Step 1.
  const iterated = GetAsyncIteratorDirectWrapper(this);

  // Step 2.
  if (!IsCallable(fn)) {
    ThrowTypeError(JSMSG_NOT_FUNCTION, DecompileArg(0, fn));
  }

  // Step 3.
  for await (const value of allowContentIter(iterated)) {
    // Steps d-h.
    if (await callContentFunction(fn, undefined, value)) {
      return true;
    }
  }
  // Step 3b.
  return false;
}

/* Iterator Helpers proposal 2.1.6.12 */
async function AsyncIteratorEvery(fn) {
  // Step 1.
  const iterated = GetAsyncIteratorDirectWrapper(this);

  // Step 2.
  if (!IsCallable(fn)) {
    ThrowTypeError(JSMSG_NOT_FUNCTION, DecompileArg(0, fn));
  }

  // Step 3.
  for await (const value of allowContentIter(iterated)) {
    // Steps d-h.
    if (!await callContentFunction(fn, undefined, value)) {
      return false;
    }
  }
  // Step 3b.
  return true;
}

/* Iterator Helpers proposal 2.1.6.13 */
async function AsyncIteratorFind(fn) {
  // Step 1.
  const iterated = GetAsyncIteratorDirectWrapper(this);

  // Step 2.
  if (!IsCallable(fn)) {
    ThrowTypeError(JSMSG_NOT_FUNCTION, DecompileArg(0, fn));
  }

  // Step 3.
  for await (const value of allowContentIter(iterated)) {
    // Steps d-h.
    if (await callContentFunction(fn, undefined, value)) {
      return value;
    }
  }
}

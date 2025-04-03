/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// ES2017 draft rev 0e10c9f29fca1385980c08a7d5e7bb3eb775e2e4
// 23.2.1.1 Set, steps 6-8
function SetConstructorInit(iterable) {
  var set = this;

  // Step 6.a.
  var adder = set.add;

  // Step 6.b.
  if (!IsCallable(adder)) {
    ThrowTypeError(JSMSG_NOT_FUNCTION, typeof adder);
  }

  // Steps 6.c-8.
  for (var nextValue of allowContentIter(iterable)) {
    callContentFunction(adder, set, nextValue);
  }
}

#ifdef ENABLE_NEW_SET_METHODS
// New Set methods proposal
//
// Set.prototype.union(iterable)
// https://tc39.es/proposal-set-methods/#Set.prototype.union
function SetUnion(iterable) {
  // Step 1. Let set be the this value.
  var set = this;

  // Step 2. If Type(set) is not Object, throw a TypeError exception.
  if (!IsObject(set)) {
    ThrowTypeError(JSMSG_OBJECT_REQUIRED, set === null ? "null" : typeof set);
  }

  // Step 3. Let Ctr be ? SpeciesConstructor(set, %Set%).
  var Ctr = SpeciesConstructor(set, GetBuiltinConstructor("Set"));

  // Step 4. Let newSet be ? Construct(Ctr, set).
  var newSet = constructContentFunction(Ctr, Ctr, set);

  // Step 5. Let adder be ? Get(newSet, "add").
  var adder = newSet.add;

  // Inlined AddEntryFromIterable Step 1. If IsCallable(adder) is false,
  // throw a TypeError exception.
  if (!IsCallable(adder)) {
    ThrowTypeError(JSMSG_PROPERTY_NOT_CALLABLE, "add");
  }

  // Step 6. Return ? AddEntryFromIterable(newSet, iterable, adder).
  return AddEntryFromIterable(newSet, iterable, adder);
}

// New Set methods proposal
//
// Set.prototype.intersection(iterable)
// https://tc39.es/proposal-set-methods/#Set.prototype.intersection
function SetIntersection(iterable) {
  // Step 1. Let set be the this value.
  var set = this;

  // Step 2. If Type(set) is not Object, throw a TypeError exception.
  if (!IsObject(set)) {
    ThrowTypeError(JSMSG_OBJECT_REQUIRED, set === null ? "null" : typeof set);
  }

  // Step 3. Let Ctr be ? SpeciesConstructor(set, %Set%).
  var Ctr = SpeciesConstructor(set, GetBuiltinConstructor("Set"));

  // Step 4. Let newSet be ? Construct(Ctr).
  var newSet = constructContentFunction(Ctr, Ctr);

  // Step 5. Let hasCheck be ? Get(set, "has").
  var hasCheck = set.has;

  // Step 6. If IsCallable(hasCheck) is false, throw a TypeError exception.
  if (!IsCallable(hasCheck)) {
    ThrowTypeError(JSMSG_PROPERTY_NOT_CALLABLE, "has");
  }

  // Step 7. Let adder be ? Get(newSet, "add").
  var adder = newSet.add;

  // Step 8. If IsCallable(adder) is false, throw a TypeError exception.
  if (!IsCallable(adder)) {
    ThrowTypeError(JSMSG_PROPERTY_NOT_CALLABLE, "add");
  }

  // Step 9. Let iteratorRecord be ? GetIterator(iterable).
  var iteratorRecord = GetIteratorSync(iterable);

  // Step 10. Repeat,
  while (true) {
    // Step a. Let next be ? IteratorStep(iteratorRecord).
    var next = IteratorStep(iteratorRecord);

    // Step b. If next is false, return newSet.
    if (!next) {
      return newSet;
    }

    // Step c. Let nextValue be ? IteratorValue(next).
    var nextValue = next.value;
    var needClose = true;
    var has;
    try {
      // Step d. Let has be Call(hasCheck, set, « nextValue »).
      has = callContentFunction(hasCheck, set, nextValue);
      needClose = false;
    } finally {
      if (needClose) {
        // Step e. If has is an abrupt completion,
        // return ? IteratorClose(iteratorRecord, has).
        IteratorClose(iteratorRecord);
      }
    }

    // Step f. If has.[[Value]] is true,
    if (has) {
      needClose = true;
      try {
        // Step i. Let status be Call(adder, newSet, « nextValue »).
        callContentFunction(adder, newSet, nextValue);
        needClose = false;
      } finally {
        if (needClose) {
          // Step ii. If status is an abrupt completion, return ?
          // IteratorClose(iteratorRecord, status).
          IteratorClose(iteratorRecord);
        }
      }
    }
  }
}

// New Set methods proposal
//
// Set.prototype.difference(iterable)
// https://tc39.es/proposal-set-methods/#Set.prototype.difference
function SetDifference(iterable) {
  // Step 1. Let set be the this value.
  var set = this;

  // Step 2. If Type(set) is not Object, throw a TypeError exception.
  if (!IsObject(set)) {
    ThrowTypeError(JSMSG_OBJECT_REQUIRED, set === null ? "null" : typeof set);
  }

  // Step 3. Let Ctr be ? SpeciesConstructor(set, %Set%).
  var Ctr = SpeciesConstructor(set, GetBuiltinConstructor("Set"));

  // Step 4. Let newSet be ? Construct(Ctr, set).
  var newSet = constructContentFunction(Ctr, Ctr, set);

  // Step 5. Let remover be ? Get(newSet, "delete").
  var remover = newSet.delete;

  // Step 6. If IsCallable(remover) is false, throw a TypeError exception.
  if (!IsCallable(remover)) {
    ThrowTypeError(JSMSG_PROPERTY_NOT_CALLABLE, "delete");
  }

  // Step 7. Let iteratorRecord be ? GetIterator(iterable).
  var iteratorRecord = GetIteratorSync(iterable);

  // Step 8. Repeat,
  while (true) {
    // Step a. Let next be ? IteratorStep(iteratorRecord).
    var next = IteratorStep(iteratorRecord);

    // Step b. If next is false, return newSet.
    if (!next) {
      return newSet;
    }

    // Step c. Let nextValue be ? IteratorValue(next).
    var nextValue = next.value;
    var needClose = true;
    try {
      // Step d. Let status be Call(remover, newSet, « nextValue »).
      callContentFunction(remover, newSet, nextValue);
      needClose = false;
    } finally {
      if (needClose) {
        // Step e. If status is an abrupt completion,
        // return ? IteratorClose(iteratorRecord, status).
        IteratorClose(iteratorRecord);
      }
    }
  }
}

// New Set methods proposal
//
// Set.prototype.symmetricDifference(iterable)
// https://tc39.es/proposal-set-methods/#Set.prototype.symmetricDifference
function SetSymmetricDifference(iterable) {
  // Step 1. Let set be the this value.
  var set = this;

  // Step 2. If Type(set) is not Object, throw a TypeError exception.
  if (!IsObject(set)) {
    ThrowTypeError(JSMSG_OBJECT_REQUIRED, set === null ? "null" : typeof set);
  }

  // Step 3. Let Ctr be ? SpeciesConstructor(set, %Set%).
  var Ctr = SpeciesConstructor(set, GetBuiltinConstructor("Set"));

  // Step 4. Let newSet be ? Construct(Ctr, set).
  var newSet = constructContentFunction(Ctr, Ctr, set);

  // Step 5. Let remover be ? Get(newSet, "delete").
  var remover = newSet.delete;

  // Step 6. If IsCallable(remover) is false, throw a TypeError exception.
  if (!IsCallable(remover)) {
    ThrowTypeError(JSMSG_PROPERTY_NOT_CALLABLE, "delete");
  }

  // Step 7. Let adder be ? Get(newSet, "add").
  var adder = newSet.add;

  // Step 8. If IsCallable(adder) is false, throw a TypeError exception.
  if (!IsCallable(adder)) {
    ThrowTypeError(JSMSG_PROPERTY_NOT_CALLABLE, "add");
  }

  // Step 9. Let iteratorRecord be ? GetIterator(iterable).
  var iteratorRecord = GetIteratorSync(iterable);

  // Step 10. Repeat,
  while (true) {
    // Step a. Let next be ? IteratorStep(iteratorRecord).
    var next = IteratorStep(iteratorRecord);

    // Step b. If next is false, return newSet.
    if (!next) {
      return newSet;
    }

    // Step c. Let nextValue be ? IteratorValue(next).
    var nextValue = next.value;
    var needClose = true;
    var removed;
    try {
      // Step d. Let removed be Call(remover, newSet, « nextValue »).
      removed = callContentFunction(remover, newSet, nextValue);
      needClose = false;
    } finally {
      if (needClose) {
        // Step e. If removed is an abrupt completion,
        // return ? IteratorClose(iteratorRecord, removed).
        IteratorClose(iteratorRecord);
      }
    }

    // Step f. If removed.[[Value]] is false,
    if (!removed) {
      needClose = true;
      try {
        // Step i. Let status be Call(adder, newSet, « nextValue »).
        callContentFunction(adder, newSet, nextValue);
        needClose = false;
      } finally {
        if (needClose) {
          // Step ii. If status is an abrupt completion,
          // return ? IteratorClose(iteratorRecord, status).
          IteratorClose(iteratorRecord);
        }
      }
    }
  }
}

// New Set methods proposal
//
// Set.prototype.isSubsetOf(iterable)
// https://tc39.es/proposal-set-methods/#Set.prototype.isSubsetOf
function SetIsSubsetOf(iterable) {
  // Step 1. Let set be the this value.
  var set = this;

  // Step 2. Let iteratorRecord be ? GetIterator(set).
  var iteratorRecord = GetIteratorSync(set);

  // Step 3. If Type(iterable) is not Object, throw a TypeError exception.
  if (!IsObject(iterable)) {
    ThrowTypeError(JSMSG_OBJECT_REQUIRED, set === null ? "null" : typeof set);
  }

  // Step 4. Let otherSet be iterable.
  var otherSet = iterable;

  // Step 5. Let hasCheck be ? Get(otherSet, "has").
  var hasCheck = otherSet.has;

  // Step 6. If IsCallable(hasCheck) is false,
  if (!IsCallable(hasCheck)) {
    // Step a. Let otherSet be ? Construct(%Set%).
    let set = GetBuiltinConstructor("Set");
    otherSet = new set();

    // We are not inlining AddEntryFromIterable Step 1 here because we know
    // std_Set_add is callable Step b. Perform ?
    // AddEntryFromIterable(otherSet, iterable, %SetProto_add%).
    AddEntryFromIterable(otherSet, iterable, std_Set_add);

    // Step c. Let hasCheck be %SetProto_has%.
    hasCheck = std_Set_has;
  }

  // Step 7. Repeat,
  while (true) {
    // Step a. Let next be ? IteratorStep(iteratorRecord).
    var next = IteratorStep(iteratorRecord);

    // Step b. If next is false, return true.
    if (!next) {
      return true;
    }

    // Step c. Let nextValue be ? IteratorValue(next).
    var nextValue = next.value;
    var needClose = true;
    var has;
    try {
      // Step d. Let has be Call(hasCheck, otherSet, « nextValue »).
      has = callContentFunction(hasCheck, otherSet, nextValue);
      needClose = false;
    } finally {
      if (needClose) {
        // Step e. If has is an abrupt completion,
        // return ? IteratorClose(iteratorRecord, has).
        IteratorClose(iteratorRecord);
      }
    }

    // Step f. If has.[[Value]] is false, return false.
    if (!has) {
      return false;
    }
  }
}

// New Set methods proposal
//
// Set.prototype.isSupersetOf(iterable)
// https://tc39.es/proposal-set-methods/#Set.prototype.isSupersetOf
function SetIsSupersetOf(iterable) {
  // Step 1. Let set be the this value.
  var set = this;

  // Step 2. If Type(set) is not Object, throw a TypeError exception.
  if (!IsObject(set)) {
    ThrowTypeError(JSMSG_OBJECT_REQUIRED, set === null ? "null" : typeof set);
  }

  // Step 3. Let hasCheck be ? Get(set, "has").
  var hasCheck = set.has;

  // Step 4. If IsCallable(hasCheck) is false, throw a TypeError exception.
  if (!IsCallable(hasCheck)) {
    ThrowTypeError(JSMSG_PROPERTY_NOT_CALLABLE, "has");
  }

  // Step 5. Let iteratorRecord be ? GetIterator(iterable).
  var iteratorRecord = GetIteratorSync(iterable);

  // Step 6. Repeat,
  while (true) {
    // Step a. Let next be ? IteratorStep(iteratorRecord).
    var next = IteratorStep(iteratorRecord);

    // Step b. If next is false, return true.
    if (!next) {
      return true;
    }

    // Step c. Let nextValue be ? IteratorValue(next).
    var nextValue = next.value;
    var needClose = true;
    var has;
    try {
      // Step d. Let has be Call(hasCheck, set, « nextValue »).
      has = callContentFunction(hasCheck, set, nextValue);
      needClose = false;
    } finally {
      if (needClose) {
        // Step e. If has is an abrupt completion,
        // return ? IteratorClose(iteratorRecord, has).
        IteratorClose(iteratorRecord);
      }
    }

    // Step f. If has.[[Value]] is false, return false.
    if (!has) {
      return false;
    }
  }
}

// New Set methods proposal
//
// Set.prototype.isDisjointFrom(iterable)
// https://tc39.es/proposal-set-methods/#Set.prototype.isDisjointFrom
function SetIsDisjointFrom(iterable) {
  // Step 1. Let set be the this value.
  var set = this;

  // Step 2. If Type(set) is not Object, throw a TypeError exception.
  if (!IsObject(set)) {
    ThrowTypeError(JSMSG_OBJECT_REQUIRED, set === null ? "null" : typeof set);
  }

  // Step 3. Let hasCheck be ? Get(set, "has").
  var hasCheck = set.has;

  // Step 4. If IsCallable(hasCheck) is false, throw a TypeError exception.
  if (!IsCallable(hasCheck)) {
    ThrowTypeError(JSMSG_PROPERTY_NOT_CALLABLE, "has");
  }

  // Step 5. Let iteratorRecord be ? GetIterator(iterable).
  var iteratorRecord = GetIteratorSync(iterable);

  // Step 6. Repeat,
  while (true) {
    // Step a. Let next be ? IteratorStep(iteratorRecord).
    var next = IteratorStep(iteratorRecord);

    // Step b. If next is false, return true.
    if (!next) {
      return true;
    }

    // Step c. Let nextValue be ? IteratorValue(next).
    var nextValue = next.value;
    var needClose = true;
    var has;
    try {
      // Step d. Let has be Call(hasCheck, set, « nextValue »).
      has = callContentFunction(hasCheck, set, nextValue);
      needClose = false;
    } finally {
      if (needClose) {
        // Step e. If has is an abrupt completion,
        // return ? IteratorClose(iteratorRecord, has).
        IteratorClose(iteratorRecord);
      }
    }

    // Step f. If has.[[Value]] is true, return false.
    if (has) {
      return false;
    }
  }
}

// New Set methods proposal
//
// AddEntryFromIterable ( target, iterable, adder )
// https://tc39.es/proposal-set-methods/#AddEntryFromIterable
function AddEntryFromIterable(target, iterable, adder) {
  assert(IsCallable(adder), "adder argument is callable");

  // Step 2. Let iteratorRecord be ? GetIterator(iterable).
  var iteratorRecord = GetIteratorSync(iterable);

  // Step 3. Repeat,
  while (true) {
    // Step a. Let next be ? IteratorStep(iteratorRecord).
    var next = IteratorStep(iteratorRecord);

    // Step b. If next is false, return target.
    if (!next) {
      return target;
    }

    // Step c. Let nextValue be ? IteratorValue(next).
    var nextValue = next.value;
    var needClose = true;
    try {
      // Step d. Let status be Call(adder, target, « nextValue »).
      callContentFunction(adder, target, nextValue);
      needClose = false;
    } finally {
      if (needClose) {
        // Step e. If status is an abrupt completion,
        // return ? IteratorClose(iteratorRecord, status).
        IteratorClose(iteratorRecord);
      }
    }
  }
}
#endif

// ES2018 draft rev f83aa38282c2a60c6916ebc410bfdf105a0f6a54
// 23.2.3.6 Set.prototype.forEach ( callbackfn [ , thisArg ] )
function SetForEach(callbackfn, thisArg = undefined) {
  // Step 1.
  var S = this;

  // Steps 2-3.
  if (!IsObject(S) || (S = GuardToSetObject(S)) === null) {
    return callFunction(
      CallSetMethodIfWrapped,
      this,
      callbackfn,
      thisArg,
      "SetForEach"
    );
  }

  // Step 4.
  if (!IsCallable(callbackfn)) {
    ThrowTypeError(JSMSG_NOT_FUNCTION, DecompileArg(0, callbackfn));
  }

  // Steps 5-8.
  var values = callFunction(std_Set_values, S);

  // Inlined: SetIteratorNext
  var setIterationResult = globalSetIterationResult;

  while (true) {
    var done = GetNextSetEntryForIterator(values, setIterationResult);
    if (done) {
      break;
    }

    var value = setIterationResult[0];
    setIterationResult[0] = null;

    callContentFunction(callbackfn, thisArg, value, value, S);
  }
}

// ES6 final draft 23.2.2.2.
// Uncloned functions with `$` prefix are allocated as extended function
// to store the original name in `SetCanonicalName`.
function $SetSpecies() {
  // Step 1.
  return this;
}
SetCanonicalName($SetSpecies, "get [Symbol.species]");

var globalSetIterationResult = CreateSetIterationResult();

function SetIteratorNext() {
  // Step 1.
  var O = this;

  // Steps 2-3.
  if (!IsObject(O) || (O = GuardToSetIterator(O)) === null) {
    return callFunction(
      CallSetIteratorMethodIfWrapped,
      this,
      "SetIteratorNext"
    );
  }

  // Steps 4-5 (implemented in GetNextSetEntryForIterator).
  // Steps 8-9 (omitted).

  var setIterationResult = globalSetIterationResult;

  var retVal = { value: undefined, done: true };

  // Steps 10.a, 11.
  var done = GetNextSetEntryForIterator(O, setIterationResult);
  if (!done) {
    // Steps 10.b-c (omitted).

    // Step 6.
    var itemKind = UnsafeGetInt32FromReservedSlot(O, ITERATOR_SLOT_ITEM_KIND);

    var result;
    if (itemKind === ITEM_KIND_VALUE) {
      // Step 10.d.i.
      result = setIterationResult[0];
    } else {
      // Step 10.d.ii.
      assert(itemKind === ITEM_KIND_KEY_AND_VALUE, itemKind);
      result = [setIterationResult[0], setIterationResult[0]];
    }

    setIterationResult[0] = null;
    retVal.value = result;
    retVal.done = false;
  }

  // Steps 7, 10.d, 12.
  return retVal;
}

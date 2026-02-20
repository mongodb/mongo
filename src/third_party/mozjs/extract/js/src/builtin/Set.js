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
    var itemKind = UnsafeGetInt32FromReservedSlot(O, MAP_SET_ITERATOR_SLOT_ITEM_KIND);

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

// GetSetRecord ( obj )
//
// https://tc39.es/proposal-set-methods/#sec-getsetrecord
function GetSetRecord(obj) {
  // Step 1.
  if (!IsObject(obj)) {
    ThrowTypeError(JSMSG_OBJECT_REQUIRED, obj === null ? "null" : typeof obj);
  }

  // Step 2.
  var rawSize = obj.size;

  // Step 3.
  var numSize = +rawSize;

  // Steps 4-5.
  if (numSize !== numSize) {
    if (rawSize === undefined) {
      ThrowTypeError(JSMSG_UNEXPECTED_TYPE, "size", "undefined");
    } else {
      ThrowTypeError(JSMSG_UNEXPECTED_TYPE, "size", "NaN");
    }
  }

  // Step 6.
  var intSize = ToInteger(numSize);

  // Step 7.
  if (intSize < 0) {
    ThrowRangeError(JSMSG_SET_NEGATIVE_SIZE);
  }

  // Step 8.
  var has = obj.has;

  // Step 9.
  if (!IsCallable(has)) {
    ThrowTypeError(JSMSG_PROPERTY_NOT_CALLABLE, "has");
  }

  // Step 10.
  var keys = obj.keys;

  // Step 11.
  if (!IsCallable(keys)) {
    ThrowTypeError(JSMSG_PROPERTY_NOT_CALLABLE, "keys");
  }

  // Step 12.
  return { set: obj, size: intSize, has, keys };
}

// 7.4.2 GetIteratorFromMethod ( obj, method )
//
// ES2024 draft rev a103b287cd19bdc51c7a3d8d7c1431b1506a74e2
function GetIteratorFromMethod(setRec) {
  // Step 1.
  var keysIter = callContentFunction(setRec.keys, setRec.set);

  // Step 2.
  if (!IsObject(keysIter)) {
    ThrowTypeError(
      JSMSG_OBJECT_REQUIRED,
      keysIter === null ? "null" : typeof keysIter
    );
  }

  // Step 3. (Implemented in caller.)

  // Step 4.
  return keysIter;
}

// Set.prototype.union ( other )
//
// https://tc39.es/proposal-set-methods/#sec-set.prototype.union
function SetUnion(other) {
  // Step 1.
  var O = this;

  // Step 2.
  if (!IsObject(O) || (O = GuardToSetObject(O)) === null) {
    return callFunction(CallSetMethodIfWrapped, this, other, "SetUnion");
  }

  // Step 3.
  var otherRec = GetSetRecord(other);

  // Step 4.
  var keysIter = GetIteratorFromMethod(otherRec);
  var keysIterNext = keysIter.next;

  // Steps 5 and 8-9. (Reordered)
  var result = SetCopy(O);

  // Steps 6-7.
  for (var nextValue of allowContentIterWithNext(keysIter, keysIterNext)) {
    // Step 7.a and 7.b.i. (Implicit through for-of loop)

    // Steps 7.b.ii-iii. (Implicit through std_Set_add)

    // Step 7.b.iii.1.
    callFunction(std_Set_add, result, nextValue);
  }

  // Step 10.
  return result;
}

// Set.prototype.intersection ( other )
//
// https://tc39.es/proposal-set-methods/#sec-set.prototype.intersection
function SetIntersection(other) {
  // Step 1.
  var O = this;

  // Step 2.
  if (!IsObject(O) || (O = GuardToSetObject(O)) === null) {
    return callFunction(CallSetMethodIfWrapped, this, other, "SetIntersection");
  }

  // Step 3.
  var otherRec = GetSetRecord(other);

  // Steps 4 and 8-9. (Reordered)
  var Set = GetBuiltinConstructor("Set");
  var result = new Set();

  // Step 5.
  var thisSize = callFunction(std_Set_size, O);

  // Steps 6-7.
  if (thisSize <= otherRec.size) {
    // Steps 6.a-b.
    var values = callFunction(std_Set_values, O);
    var setIterationResult = globalSetIterationResult;
    while (true) {
      var done = GetNextSetEntryForIterator(values, setIterationResult);
      if (done) {
        break;
      }

      var value = setIterationResult[0];
      setIterationResult[0] = null;

      // Steps 6.b.i-ii. (Implicit through SetIterator)

      // Steps 6.b.iii.1-2.
      if (callContentFunction(otherRec.has, otherRec.set, value)) {
        // Steps 6.b.iii.2.a-b. (Implicit through std_Set_add)

        // Step 6.b.iii.2.c.i.
        callFunction(std_Set_add, result, value);
      }

      // Steps 6.b.iii.3-4. (Implicit through SetIterator)
    }
  } else {
    // Step 7.a.
    var keysIter = GetIteratorFromMethod(otherRec);

    // Steps 7.b-c.
    for (var nextValue of allowContentIterWithNext(keysIter, keysIter.next)) {
      // Step 7.c.i and 7.c.ii.1. (Implicit through for-of loop)

      // Steps 7.c.ii.2-4. (Implicit through std_Set_add)

      // Steps 7.c.ii.5-6.
      if (callFunction(std_Set_has, O, nextValue)) {
        callFunction(std_Set_add, result, nextValue);
      }
    }
  }

  // Step 10.
  return result;
}

// Set.prototype.difference ( other )
//
// https://tc39.es/proposal-set-methods/#sec-set.prototype.difference
function SetDifference(other) {
  // Step 1.
  var O = this;

  // Step 2.
  if (!IsObject(O) || (O = GuardToSetObject(O)) === null) {
    return callFunction(CallSetMethodIfWrapped, this, other, "SetDifference");
  }

  // Step 3.
  var otherRec = GetSetRecord(other);

  // Steps 4 and 8-9. (Reordered)
  var result = SetCopy(O);

  // Step 5.
  var thisSize = callFunction(std_Set_size, O);

  // Steps 6-7.
  if (thisSize <= otherRec.size) {
    // Steps 6.a-b.
    var values = callFunction(std_Set_values, result);
    var setIterationResult = globalSetIterationResult;
    while (true) {
      var done = GetNextSetEntryForIterator(values, setIterationResult);
      if (done) {
        break;
      }

      var value = setIterationResult[0];
      setIterationResult[0] = null;

      // Steps 6.b.i-ii. (Implicit through SetIterator)

      // Steps 6.b.iii.1-2.
      if (callContentFunction(otherRec.has, otherRec.set, value)) {
        callFunction(std_Set_delete, result, value);
      }
    }
  } else {
    // Step 7.a.
    var keysIter = GetIteratorFromMethod(otherRec);

    // Steps 7.b-c.
    for (var nextValue of allowContentIterWithNext(keysIter, keysIter.next)) {
      // Step 7.c.i and 7.c.ii.1. (Implicit through for-of loop)

      // Steps 7.c.ii.2-3.
      callFunction(std_Set_delete, result, nextValue);
    }
  }

  // Step 10.
  return result;
}

// Set.prototype.symmetricDifference ( other )
//
// https://tc39.es/proposal-set-methods/#sec-set.prototype.symmetricdifference
function SetSymmetricDifference(other) {
  // Step 1.
  var O = this;

  // Step 2.
  if (!IsObject(O) || (O = GuardToSetObject(O)) === null) {
    return callFunction(
      CallSetMethodIfWrapped,
      this,
      other,
      "SetSymmetricDifference"
    );
  }

  // Step 3.
  var otherRec = GetSetRecord(other);

  // Step 4.
  var keysIter = GetIteratorFromMethod(otherRec);
  var keysIterNext = keysIter.next;

  // Steps 5 and 8-9. (Reordered)
  var result = SetCopy(O);

  // Steps 6-7.
  for (var nextValue of allowContentIterWithNext(keysIter, keysIterNext)) {
    // Step 7.a and 7.b.i. (Implicit through for-of loop)

    // Steps 7.b.ii-iii. (Implicit through std_Set_has)

    // Steps 7.b.iv-v.
    if (callFunction(std_Set_has, O, nextValue)) {
      // Step 7.b.iv.1.
      callFunction(std_Set_delete, result, nextValue);
    } else {
      // Step 7.b.v.1.
      callFunction(std_Set_add, result, nextValue);
    }
  }

  // Step 10.
  return result;
}

// Set.prototype.isSubsetOf ( other )
//
// https://tc39.es/proposal-set-methods/#sec-set.prototype.issubsetof
function SetIsSubsetOf(other) {
  // Step 1.
  var O = this;

  // Step 2.
  if (!IsObject(O) || (O = GuardToSetObject(O)) === null) {
    return callFunction(CallSetMethodIfWrapped, this, other, "SetIsSubsetOf");
  }

  // Step 3.
  var otherRec = GetSetRecord(other);

  // Step 4.
  var thisSize = callFunction(std_Set_size, O);

  // Step 5.
  if (thisSize > otherRec.size) {
    return false;
  }

  // Steps 6-7.
  var values = callFunction(std_Set_values, O);
  var setIterationResult = globalSetIterationResult;
  while (true) {
    var done = GetNextSetEntryForIterator(values, setIterationResult);
    if (done) {
      break;
    }

    var value = setIterationResult[0];
    setIterationResult[0] = null;

    // Steps 7.a-b. (Implicit through SetIterator)

    // Steps 7.c-d.
    if (!callContentFunction(otherRec.has, otherRec.set, value)) {
      return false;
    }

    // Steps 7.e-f. (Implicit through SetIterator)
  }

  // Step 7.
  return true;
}

// Set.prototype.isSupersetOf ( other )
//
// https://tc39.es/proposal-set-methods/#sec-set.prototype.issupersetof
function SetIsSupersetOf(other) {
  // Step 1.
  var O = this;

  // Step 2.
  if (!IsObject(O) || (O = GuardToSetObject(O)) === null) {
    return callFunction(CallSetMethodIfWrapped, this, other, "SetIsSupersetOf");
  }

  // Step 3.
  var otherRec = GetSetRecord(other);

  // Step 4.
  var thisSize = callFunction(std_Set_size, O);

  // Step 5.
  if (thisSize < otherRec.size) {
    return false;
  }

  // Step 6.
  var keysIter = GetIteratorFromMethod(otherRec);

  // Steps 7-8.
  for (var nextValue of allowContentIterWithNext(keysIter, keysIter.next)) {
    // Step 8.a and 8.b.i. (Implicit through for-of loop)

    // Step 8.b.ii.
    if (!callFunction(std_Set_has, O, nextValue)) {
      // Step 8.b.ii.1. (Implicit through for-of loop)

      // Step 8.b.ii.2.
      return false;
    }
  }

  // Step 9.
  return true;
}

// Set.prototype.isDisjointFrom ( other )
//
// https://tc39.es/proposal-set-methods/#sec-set.prototype.isdisjointfrom
function SetIsDisjointFrom(other) {
  // Step 1.
  var O = this;

  // Step 2.
  if (!IsObject(O) || (O = GuardToSetObject(O)) === null) {
    return callFunction(
      CallSetMethodIfWrapped,
      this,
      other,
      "SetIsDisjointFrom"
    );
  }

  // Step 3.
  var otherRec = GetSetRecord(other);

  // Step 4.
  var thisSize = callFunction(std_Set_size, O);

  // Steps 5-6.
  if (thisSize <= otherRec.size) {
    // Steps 5.a-b.
    var values = callFunction(std_Set_values, O);
    var setIterationResult = globalSetIterationResult;
    while (true) {
      var done = GetNextSetEntryForIterator(values, setIterationResult);
      if (done) {
        break;
      }

      var value = setIterationResult[0];
      setIterationResult[0] = null;

      // Step 5.b.i-ii. (Implicit through SetIterator)

      // Steps 5.b.iii.1-2.
      if (callContentFunction(otherRec.has, otherRec.set, value)) {
        return false;
      }

      // Steps 5.b.iii.3-4. (Implicit through SetIterator)
    }
  } else {
    // Step 6.a.
    var keysIter = GetIteratorFromMethod(otherRec);

    // Steps 6.b-c.
    for (var nextValue of allowContentIterWithNext(keysIter, keysIter.next)) {
      // Step 6.c.i and 6.c.ii.1. (Implicit through for-of loop)

      // Step 6.c.ii.2.
      if (callFunction(std_Set_has, O, nextValue)) {
        // Step 6.c.ii.2.a. (Implicit through for-of loop)

        // Step 6.c.ii.2.b.
        return false;
      }
    }
  }

  // Step 7.
  return true;
}

/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// ES2017 draft rev 0e10c9f29fca1385980c08a7d5e7bb3eb775e2e4
// 23.3.1.1 WeakMap, steps 6-8
function WeakMapConstructorInit(iterable) {
  var map = this;

  // Step 6.a.
  var adder = map.set;

  // Step 6.b.
  if (!IsCallable(adder)) {
    ThrowTypeError(JSMSG_NOT_FUNCTION, typeof adder);
  }

  // Steps 6.c-8.
  for (var nextItem of allowContentIter(iterable)) {
    // Step 8.d.
    if (!IsObject(nextItem)) {
      ThrowTypeError(JSMSG_INVALID_MAP_ITERABLE, "WeakMap");
    }

    // Steps 8.e-j.
    callContentFunction(adder, map, nextItem[0], nextItem[1]);
  }
}

#ifdef NIGHTLY_BUILD
/**
 * Upsert proposal
 * 
 * WeakMap.prototype.getOrInsertComputed ( key, callbackfn )
 *
 * https://tc39.es/proposal-upsert/
 */
function WeakMapGetOrInsertComputed(key, callbackfn) {
  // Step 1.  Let M be the this value.
  var M = this;

  // Step 2.  Perform ? RequireInternalSlot(M, [[WeakMapData]]).
  if (!IsObject(M) || (M = GuardToWeakMapObject(M)) === null) {
    return callFunction(
      CallWeakMapMethodIfWrapped,
      this,
      key,
      callbackfn,
      "WeakMapGetOrInsertComputed"
    );
  }

  // Step 3.  If IsCallable(callbackfn) is false, throw a TypeError exception.
  if (!IsCallable(callbackfn)) {
    ThrowTypeError(JSMSG_NOT_FUNCTION, DecompileArg(1, callbackfn));
  }

  // Step 4.  If CanBeHeldWeakly(key) is false, throw a TypeError exception.
  // Step 5.  For each Record { [[Key]], [[Value]] } p of M.[[WeakMapData]], do
  // Step 5.a.  If p.[[Key]] is not empty and SameValue(p.[[Key]], key) is true, return p.[[Value]].
  if (callFunction(std_WeakMap_has, M, key)) {
    return callFunction(std_WeakMap_get, M, key);
  }

  // Step 6.  Let value be ? Call(callbackfn, undefined, « key »).
  var value = callContentFunction(callbackfn, undefined, key);

  // Step 7.  For each Record { [[Key]], [[Value]] } p of M.[[WeakMapData]], do
  // Step 7.a.  If p.[[Key]] is not empty and SameValue(p.[[Key]], key) is true, then
  // Step 7.a.i.  Set p.[[Value]] to value.
  // Step 8.  Let p be the Record { [[Key]]: key, [[Value]]: value }.
  // Step 9.  Append p to M.[[WeakMapData]].
  callFunction(std_WeakMap_set, M, key, value);

  // Step 7.a.ii, 10. Return value.
  return value;
}
#endif  // #ifdef NIGHTLY_BUILD

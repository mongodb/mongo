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
    if (!IsCallable(adder))
        ThrowTypeError(JSMSG_NOT_FUNCTION, typeof adder);

    // Steps 6.c-8.
    for (var nextValue of allowContentIter(iterable))
        callContentFunction(adder, set, nextValue);
}

// ES2018 draft rev f83aa38282c2a60c6916ebc410bfdf105a0f6a54
// 23.2.3.6 Set.prototype.forEach ( callbackfn [ , thisArg ] )
function SetForEach(callbackfn, thisArg = undefined) {
    // Step 1.
    var S = this;

    // Steps 2-3.
    if (!IsObject(S) || (S = GuardToSetObject(S)) === null)
        return callFunction(CallSetMethodIfWrapped, this, callbackfn, thisArg, "SetForEach");

    // Step 4.
    if (!IsCallable(callbackfn))
        ThrowTypeError(JSMSG_NOT_FUNCTION, DecompileArg(0, callbackfn));

    // Steps 5-8.
    var values = callFunction(std_Set_iterator, S);

    // Inlined: SetIteratorNext
    var setIterationResult = setIteratorTemp.setIterationResult;
    if (!setIterationResult)
        setIterationResult = setIteratorTemp.setIterationResult = _CreateSetIterationResult();

    while (true) {
        var done = _GetNextSetEntryForIterator(values, setIterationResult);
        if (done)
            break;

        var value = setIterationResult[0];
        setIterationResult[0] = null;

        callContentFunction(callbackfn, thisArg, value, value, S);
    }
}

function SetValues() {
    return callFunction(std_Set_iterator, this);
}
_SetCanonicalName(SetValues, "values");

// ES6 final draft 23.2.2.2.
function SetSpecies() {
    // Step 1.
    return this;
}
_SetCanonicalName(SetSpecies, "get [Symbol.species]");


var setIteratorTemp = { setIterationResult: null };

function SetIteratorNext() {
    // Step 1.
    var O = this;

    // Steps 2-3.
    if (!IsObject(O) || (O = GuardToSetIterator(O)) === null)
        return callFunction(CallSetIteratorMethodIfWrapped, this, "SetIteratorNext");

    // Steps 4-5 (implemented in _GetNextSetEntryForIterator).
    // Steps 8-9 (omitted).

    var setIterationResult = setIteratorTemp.setIterationResult;
    if (!setIterationResult)
        setIterationResult = setIteratorTemp.setIterationResult = _CreateSetIterationResult();

    var retVal = {value: undefined, done: true};

    // Steps 10.a, 11.
    var done = _GetNextSetEntryForIterator(O, setIterationResult);
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

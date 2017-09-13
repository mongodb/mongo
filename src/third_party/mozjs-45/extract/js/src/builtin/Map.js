/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* ES6 20121122 draft 15.14.4.4. */

function MapForEach(callbackfn, thisArg = undefined) {
    /* Step 1-2. */
    var M = this;
    if (!IsObject(M))
        ThrowTypeError(JSMSG_INCOMPATIBLE_PROTO, "Map", "forEach", typeof M);

    /* Step 3-4. */
    try {
        callFunction(std_Map_has, M);
    } catch (e) {
        // has will throw on non-Map objects, throw our own error in that case.
        ThrowTypeError(JSMSG_INCOMPATIBLE_PROTO, "Map", "forEach", typeof M);
    }

    /* Step 5. */
    if (!IsCallable(callbackfn))
        ThrowTypeError(JSMSG_NOT_FUNCTION, DecompileArg(0, callbackfn));

    /* Step 6-8. */
    var entries = callFunction(std_Map_iterator, M);
    while (true) {
        var result = callFunction(std_Map_iterator_next, entries);
        if (result.done)
            break;
        var entry = result.value;
        callFunction(callbackfn, thisArg, entry[1], entry[0], M);
    }
}

var iteratorTemp = { mapIterationResultPair : null };

function MapIteratorNext() {
    // Step 1.
    var O = this;

    // Steps 2-3.
    if (!IsObject(O) || !IsMapIterator(O))
        return callFunction(CallMapIteratorMethodIfWrapped, O, "MapIteratorNext");

    // Steps 4-5 (implemented in _GetNextMapEntryForIterator).
    // Steps 8-9 (omitted).

    var mapIterationResultPair = iteratorTemp.mapIterationResultPair;
    if (!mapIterationResultPair)
        mapIterationResultPair = iteratorTemp.mapIterationResultPair = [null, null];

    var retVal = {value: undefined, done: true};

    // Step 10.a, 11.
    var done = _GetNextMapEntryForIterator(O, mapIterationResultPair);
    if (!done) {
        // Steps 10.b-c (omitted).

        // Step 6.
        var itemKind = UnsafeGetInt32FromReservedSlot(this, ITERATOR_SLOT_ITEM_KIND);

        var result;
        if (itemKind === ITEM_KIND_KEY) {
            // Step 10.d.i.
            result = mapIterationResultPair[0];
        } else if (itemKind === ITEM_KIND_VALUE) {
            // Step 10.d.ii.
            result = mapIterationResultPair[1];
        } else {
            // Step 10.d.iii.
            assert(itemKind === ITEM_KIND_KEY_AND_VALUE, itemKind);
            result = [mapIterationResultPair[0], mapIterationResultPair[1]];
        }

        mapIterationResultPair[0] = null;
        mapIterationResultPair[1] = null;
        retVal.value = result;
        retVal.done = false;
    }

    // Steps 7, 12.
    return retVal;
}

// ES6 final draft 23.1.2.2.
function MapSpecies() {
    // Step 1.
    return this;
}

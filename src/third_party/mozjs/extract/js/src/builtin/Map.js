/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// ES2017 draft rev 0e10c9f29fca1385980c08a7d5e7bb3eb775e2e4
// 23.1.1.1 Map, steps 6-8
function MapConstructorInit(iterable) {
    var map = this;

    // Step 6.a.
    var adder = map.set;

    // Step 6.b.
    if (!IsCallable(adder))
        ThrowTypeError(JSMSG_NOT_FUNCTION, typeof adder);

    // Steps 6.c-8.
    for (var nextItem of allowContentIter(iterable)) {
        // Step 8.d.
        if (!IsObject(nextItem))
            ThrowTypeError(JSMSG_INVALID_MAP_ITERABLE, "Map");

        // Steps 8.e-j.
        callContentFunction(adder, map, nextItem[0], nextItem[1]);
    }
}

// ES2018 draft rev f83aa38282c2a60c6916ebc410bfdf105a0f6a54
// 23.1.3.5 Map.prototype.forEach ( callbackfn [ , thisArg ] )
function MapForEach(callbackfn, thisArg = undefined) {
    // Step 1.
    var M = this;

    // Steps 2-3.
    if (!IsObject(M) || (M = GuardToMapObject(M)) === null)
        return callFunction(CallMapMethodIfWrapped, this, callbackfn, thisArg, "MapForEach");

    // Step 4.
    if (!IsCallable(callbackfn))
        ThrowTypeError(JSMSG_NOT_FUNCTION, DecompileArg(0, callbackfn));

    // Steps 5-8.
    var entries = callFunction(std_Map_iterator, M);

    // Inlined: MapIteratorNext
    var mapIterationResultPair = iteratorTemp.mapIterationResultPair;
    if (!mapIterationResultPair) {
        mapIterationResultPair = iteratorTemp.mapIterationResultPair =
            _CreateMapIterationResultPair();
    }

    while (true) {
        var done = _GetNextMapEntryForIterator(entries, mapIterationResultPair);
        if (done)
            break;

        var key = mapIterationResultPair[0];
        var value = mapIterationResultPair[1];
        mapIterationResultPair[0] = null;
        mapIterationResultPair[1] = null;

        callContentFunction(callbackfn, thisArg, value, key, M);
    }
}

function MapEntries() {
    return callFunction(std_Map_iterator, this);
}
_SetCanonicalName(MapEntries, "entries");

var iteratorTemp = { mapIterationResultPair: null };

function MapIteratorNext() {
    // Step 1.
    var O = this;

    // Steps 2-3.
    if (!IsObject(O) || (O = GuardToMapIterator(O)) === null)
        return callFunction(CallMapIteratorMethodIfWrapped, this, "MapIteratorNext");

    // Steps 4-5 (implemented in _GetNextMapEntryForIterator).
    // Steps 8-9 (omitted).

    var mapIterationResultPair = iteratorTemp.mapIterationResultPair;
    if (!mapIterationResultPair) {
        mapIterationResultPair = iteratorTemp.mapIterationResultPair =
            _CreateMapIterationResultPair();
    }

    var retVal = {value: undefined, done: true};

    // Step 10.a, 11.
    var done = _GetNextMapEntryForIterator(O, mapIterationResultPair);
    if (!done) {
        // Steps 10.b-c (omitted).

        // Step 6.
        var itemKind = UnsafeGetInt32FromReservedSlot(O, ITERATOR_SLOT_ITEM_KIND);

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
_SetCanonicalName(MapSpecies, "get [Symbol.species]");

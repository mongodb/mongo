/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* ES6 20121122 draft 15.14.4.4. */

function MapForEach(callbackfn, thisArg = undefined) {
    /* Step 1-2. */
    var M = this;
    if (!IsObject(M))
        ThrowError(JSMSG_INCOMPATIBLE_PROTO, "Map", "forEach", typeof M);

    /* Step 3-4. */
    try {
        callFunction(std_Map_has, M);
    } catch (e) {
        // has will throw on non-Map objects, throw our own error in that case.
        ThrowError(JSMSG_INCOMPATIBLE_PROTO, "Map", "forEach", typeof M);
    }

    /* Step 5. */
    if (!IsCallable(callbackfn))
        ThrowError(JSMSG_NOT_FUNCTION, DecompileArg(0, callbackfn));

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

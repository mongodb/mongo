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
    if (!IsCallable(adder))
        ThrowTypeError(JSMSG_NOT_FUNCTION, typeof adder);

    // Steps 6.c-8.
    for (var nextItem of allowContentIter(iterable)) {
        // Step 8.d.
        if (!IsObject(nextItem))
            ThrowTypeError(JSMSG_INVALID_MAP_ITERABLE, "WeakMap");

        // Steps 8.e-j.
        callContentFunction(adder, map, nextItem[0], nextItem[1]);
    }
}

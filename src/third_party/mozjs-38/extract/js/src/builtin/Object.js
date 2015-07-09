/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* ES6 draft 2014-07-18 19.1.2.1. */
function ObjectStaticAssign(target, firstSource) {
    // Steps 1-2.
    var to = ToObject(target);

    // Step 3.
    if (arguments.length < 2)
        return to;

    // Step 4.
    var i = 1;
    do {
        // Step 5.a-b, plus an unspecified flourish to skip null/undefined, so
        // any site depending on agreed-upon (but not-yet-drafted) semantics
        // from TC39 meeting minutes will work. (Yes, implausibly, such a site
        // exists. See bug 1054426.)
        var nextSource = arguments[i];
        if (nextSource === null || nextSource === undefined)
            continue;

        var from = ToObject(nextSource);

        // Step 5.c-d.
        var keysArray = OwnPropertyKeys(from);

        // Steps 5.e-f.
        var len = keysArray.length;

        // Step 5.h.
        var nextIndex = 0;

        // Step 5.i (Modified a bit because we can't catch and store the
        // actual Completion Record). Instead we have a marker object.
        const MISSING = {};
        var pendingException = MISSING;

        // Step 5.j.
        while (nextIndex < len) {
            // Step 5.j.i-ii.
            var nextKey = keysArray[nextIndex];

            // Step 5.j.iii-v.
            try {
                // We'd like to use Object.propertyIsEnumerable here, but we
                // can't, because if a property doesn't exist, it won't properly
                // call getOwnPropertyDescriptor (important if |from| is a
                // proxy).
                var desc = std_Object_getOwnPropertyDescriptor(from, nextKey);
                if (desc !== undefined && desc.enumerable)
                    to[nextKey] = from[nextKey];
            } catch (e) {
                if (pendingException === MISSING)
                    pendingException = e;
            }

            // Step 5.j.vi.
            nextIndex++;
        }

        // Step 5.k.
        if (pendingException !== MISSING)
            throw pendingException;
    } while (++i < arguments.length);

    // Step 6.
    return to;
}

function ObjectDefineSetter(name, setter) {
    var object;
    if (this === null || this === undefined)
        object = global;
    else
        object = ToObject(this);

    if (!IsCallable(setter))
        ThrowError(JSMSG_BAD_GETTER_OR_SETTER, "setter");

    var key = ToPropertyKey(name);

    var desc = {
        __proto__: null,
        enumerable: true,
        configurable: true,
        set: setter
    };

    std_Object_defineProperty(object, key, desc);
}

function ObjectDefineGetter(name, getter) {
    var object;
    if (this === null || this === undefined)
        object = global;
    else
        object = ToObject(this);

    if (!IsCallable(getter))
        ThrowError(JSMSG_BAD_GETTER_OR_SETTER, "getter");

    var key = ToPropertyKey(name);

    var desc = {
        __proto__: null,
        enumerable: true,
        configurable: true,
        get: getter
    };

    std_Object_defineProperty(object, key, desc);
}

function ObjectLookupSetter(name) {
    var key = ToPropertyKey(name);
    var object = ToObject(this);

    do {
        var desc = std_Object_getOwnPropertyDescriptor(object, key);
        if (desc) {
            if (callFunction(std_Object_hasOwnProperty, desc, "set"))
                return desc.set;
            return undefined;
        }
        object = std_Object_getPrototypeOf(object);
    } while (object !== null);
}

function ObjectLookupGetter(name) {
    var key = ToPropertyKey(name);
    var object = ToObject(this);

    do {
        var desc = std_Object_getOwnPropertyDescriptor(object, key);
        if (desc) {
            if (callFunction(std_Object_hasOwnProperty, desc, "get"))
                return desc.get;
            return undefined;
        }
        object = std_Object_getPrototypeOf(object);
    } while (object !== null);
}

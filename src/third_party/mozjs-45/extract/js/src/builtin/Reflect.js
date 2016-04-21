/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// ES6 draft rev 32 (2015 Feb 2) 26.1.9
function Reflect_has(target, propertyKey) {
    // Step 1.
    if (!IsObject(target))
        ThrowTypeError(JSMSG_NOT_NONNULL_OBJECT, DecompileArg(0, target));

    // Steps 2-4 are identical to the runtime semantics of the "in" operator.
    return propertyKey in target;
}

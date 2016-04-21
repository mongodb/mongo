/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// ES6 draft rev34 (2015/02/20) 21.2.5.3 get RegExp.prototype.flags
function RegExpFlagsGetter() {
    // Steps 1-2.
    var R = this;
    if (!IsObject(R))
        ThrowTypeError(JSMSG_NOT_NONNULL_OBJECT, R === null ? "null" : typeof R);

    // Step 3.
    var result = "";

    // Steps 4-6.
    if (R.global)
        result += "g";

    // Steps 7-9.
    if (R.ignoreCase)
        result += "i";

    // Steps 10-12.
    if (R.multiline)
        result += "m";

    // Steps 13-15.
    // TODO: Uncomment these steps when bug 1135377 is fixed.
    // if (R.unicode)
    //     result += "u";

    // Steps 16-18.
    if (R.sticky)
        result += "y";

    // Step 19.
    return result;
}

// ES6 draft rc1 21.2.5.14.
function RegExpToString()
{
    // Steps 1-2.
    var R = this;
    if (!IsObject(R))
        ThrowTypeError(JSMSG_NOT_NONNULL_OBJECT, R === null ? "null" : typeof R);

    // Steps 3-4.
    var pattern = R.source;

    // Steps 5-6.
    var flags = R.flags;

    // Step 7.
    return '/' + pattern + '/' + flags;
}

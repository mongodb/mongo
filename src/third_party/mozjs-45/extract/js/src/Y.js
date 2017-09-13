/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// The Y combinator, applied to the factorial function

// Return the function that is the fixed point of f.
var Y = f => (x => f(v => x(x)(v)))
             (x => f(v => x(x)(v)));

// The factorial function is the fixed point of this:
var f = fac => n => (n <= 1) ? 1 : n * fac(n - 1);

print("5! is " + Y(f)(5));

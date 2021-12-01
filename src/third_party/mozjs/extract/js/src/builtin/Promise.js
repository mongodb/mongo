/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// ES6, 25.4.5.1.
function Promise_catch(onRejected) {
    // Steps 1-2.
    return callContentFunction(this.then, this, undefined, onRejected);
}

// Promise.prototype.finally proposal, stage 3.
// Promise.prototype.finally ( onFinally )
function Promise_finally(onFinally) {
    // Step 1.
    var promise = this;

    // Step 2.
    if (!IsObject(promise))
        ThrowTypeError(JSMSG_INCOMPATIBLE_PROTO, "Promise", "finally", "value");

    // Step 3.
    var C = SpeciesConstructor(promise, GetBuiltinConstructor("Promise"));

    // Step 4.
    assert(IsConstructor(C), "SpeciesConstructor returns a constructor function");

    // Steps 5-6.
    var thenFinally, catchFinally;
    if (!IsCallable(onFinally)) {
        thenFinally = onFinally;
        catchFinally = onFinally;
    } else {
        // ThenFinally Function.
        // The parentheses prevent the infering of a function name.
        (thenFinally) = function(value) {
            // Steps 1-2 (implicit).

            // Step 3.
            var result = onFinally();

            // Steps 4-5 (implicit).

            // Step 6.
            var promise = PromiseResolve(C, result);

            // Step 7.
            // FIXME: spec issue - "be equivalent to a function that" is not a defined spec term.
            // https://github.com/tc39/ecma262/issues/933

            // Step 8.
            return callContentFunction(promise.then, promise, function() { return value; });
        };

        // CatchFinally Function.
        (catchFinally) = function(reason) {
            // Steps 1-2 (implicit).

            // Step 3.
            var result = onFinally();

            // Steps 4-5 (implicit).

            // Step 6.
            var promise = PromiseResolve(C, result);

            // Step 7.
            // FIXME: spec issue - "be equivalent to a function that" is not a defined spec term.
            // https://github.com/tc39/ecma262/issues/933

            // Step 8.
            return callContentFunction(promise.then, promise, function() { throw reason; });
        };
    }

    // Step 7.
    return callContentFunction(promise.then, promise, thenFinally, catchFinally);
}

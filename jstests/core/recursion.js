// Basic tests for a form of stack recursion that's been shown to cause C++
// side stack overflows in the past. See SERVER-19614.

(function() {
    "use strict";

    db.recursion.drop();

    // Make sure the shell doesn't blow up
    function shellRecursion() {
        shellRecursion.apply();
    }
    assert.throws(shellRecursion);

    // Make sure db.eval doesn't blow up
    function dbEvalRecursion() {
        db.eval(function() {
            function recursion() {
                recursion.apply();
            }
            recursion();
        });
    }
    assert.commandFailedWithCode(assert.throws(dbEvalRecursion), ErrorCodes.JSInterpreterFailure);

    // Make sure mapReduce doesn't blow up
    function mapReduceRecursion() {
        db.recursion.mapReduce(
            function() {
                (function recursion() {
                    recursion.apply();
                })();
            },
            function() {},
            {out: 'inline'});
    }

    db.recursion.insert({});
    assert.commandFailedWithCode(assert.throws(mapReduceRecursion),
                                 ErrorCodes.JSInterpreterFailure);
}());

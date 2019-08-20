// Basic tests for a form of stack recursion that's been shown to cause C++ side stack overflows in
// the past. See SERVER-19614.
//
// @tags: [
//   does_not_support_stepdowns,
//   requires_non_retryable_commands,
//   uses_map_reduce_with_temp_collections,
// ]

(function() {
"use strict";

db.recursion.drop();

// Make sure the shell doesn't blow up
function shellRecursion() {
    shellRecursion.apply();
}
assert.throws(shellRecursion);

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
assert.commandFailedWithCode(assert.throws(mapReduceRecursion), ErrorCodes.JSInterpreterFailure);
}());

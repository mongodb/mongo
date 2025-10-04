// Basic tests for a form of stack recursion that's been shown to cause C++ side stack overflows in
// the past. See SERVER-19614.
//
// @tags: [
//   # The test runs commands that are not allowed with security token: mapReduce.
//   not_allowed_with_signed_security_token,
//   does_not_support_stepdowns,
//   requires_non_retryable_commands,
//   uses_map_reduce_with_temp_collections,
//   # This test has statements that do not support non-local read concern.
//   does_not_support_causal_consistency,
//   requires_scripting,
// ]

db.recursion.drop();

// Make sure the shell doesn't blow up.
function shellRecursion() {
    shellRecursion.apply();
}
assert.throws(shellRecursion);

// Make sure server side stack overflow doesn't blow up.
function mapReduceRecursion() {
    db.recursion.mapReduce(
        function () {
            (function recursion() {
                recursion.apply();
            })();
        },
        function () {},
        {out: {merge: "out_coll"}},
    );
}

assert.commandWorked(db.recursion.insert({}));
assert.commandFailedWithCode(assert.throws(mapReduceRecursion), ErrorCodes.JSInterpreterFailure);

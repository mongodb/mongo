// @tags: [
//   requires_fastcount,
//   uses_multiple_connections,
//   uses_parallel_shell,
// ]

import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";

// verify that join works
db.sps.drop();
var awaitShell = startParallelShell("sleep(1000); db.sps.insert({x:1});");
awaitShell();
assert.eq(1, db.sps.count(), "join problem?");

// test with a throw
awaitShell = startParallelShell("db.sps.insert({x:1}); throw Error('intentionally_uncaught');");
var exitCode = awaitShell({checkExitSuccess: false});
assert.neq(0, exitCode, "expected shell to exit abnormally due to an uncaught exception");
assert.eq(2, db.sps.count(), "join2 problem?");

// test throwing with checking exit success
function func() {
    throw Error("intentional_throw_to_test_assert_throws");
}
assert.throws(() => {
    const awaitShell = startParallelShell(func);
    awaitShell();
});
assert.throws(() => {
    const awaitShell = startParallelShell(funWithArgs(func, true));
    awaitShell();
});

async function asyncFunc() {
    throw Error("intentional_throw_to_test_assert_throws");
}
assert.throws(() => {
    const awaitShell = startParallelShell(asyncFunc);
    awaitShell();
});
assert.throws(() => {
    const awaitShell = startParallelShell(funWithArgs(asyncFunc, true));
    awaitShell();
});

const asyncLambda = async () => {
    throw Error("intentional_throw_to_test_assert_throws");
};
assert.throws(() => {
    const awaitShell = startParallelShell(asyncLambda);
    awaitShell();
});
assert.throws(() => {
    const awaitShell = startParallelShell(funWithArgs(asyncLambda, true));
    awaitShell();
});

print("shellstartparallel.js SUCCESS");

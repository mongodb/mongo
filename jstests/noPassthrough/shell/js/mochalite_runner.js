
import {describe, it} from "jstests/libs/mochalite.js";

/**
 * The `__mochalite_closer` global variable should be instantiated by the mochalite framework.
 * It is not intended to be used directly in tests, but rather to be called by the shell itself.
 *
 * This validates its behavior if it otherwise must be used in a JS runner context.
 */

const log = [];
const logfn = (msg) => {
    return () => log.push(msg);
};

describe("first", function() {
    it("test1", logfn(1));
    it("test2", logfn(2));
});

// should run tests
await globalThis.__mochalite_closer();
assert.eq(log, [1, 2]);

it("test3", logfn(3));
// should only run the new tests
await globalThis.__mochalite_closer();
assert.eq(log, [1, 2, 3]);

it("fail", () => {
    throw new Error("This test is expected to fail");
});

// assert.throws does not support async functions, so we use a try/catch block
let threw = false;
try {
    await globalThis.__mochalite_closer();
} catch {
    threw = true;
}

assert(threw, "Expected to throw");

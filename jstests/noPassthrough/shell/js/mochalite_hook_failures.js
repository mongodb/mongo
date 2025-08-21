import {after, afterEach, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";

let e;
let expected = [];
let forbidden = [];
let forbid = (msg) => () => forbidden.push(msg);
let expect = (msg) => () => expected.push(msg);

const RUN = () => globalThis.__mochalite_closer();
const FAILURE = () => {
    throw new Error("Intentional failure");
};

// convenience assert functions
Object.extend(assert, {
    throwsAsync: async (func) => {
        let err;
        try {
            await func();
        } catch (e) {
            err = e;
        }
        assert(err, "Expected to throw");
        return err;
    },
    startsWith: (s, prefix) => {
        assert(s.startsWith(prefix), `Expected "${s}" to start with "${prefix}"`);
    },
});

// BEFORE
expected = [];
forbidden = [];
describe("before hook failures", () => {
    describe("skip pending before hooks; skip beforeEach/tests/afterEach; run after", () => {
        before(FAILURE);
        before(forbid("before"));
        beforeEach(forbid("beforeEach"));
        it("test", forbid("test"));
        afterEach(forbid("afterEach"));
        after(expect("after"));
    });
});
e = await assert.throwsAsync(RUN);
assert.eq(e.message, "1 failing tests detected");
assert.eq(forbidden, [], "No forbidden functions should have run");
assert.eq(expected, ["after"]);

// BEFORE EACH
expected = [];
forbidden = [];
describe("beforeEach hook failures", () => {
    describe("skip pending beforeEach hooks; skip tests; run afterEach/after", () => {
        beforeEach(FAILURE);
        beforeEach(forbid("beforeEach"));
        it("test", forbid("test"));
        afterEach(expect("afterEach"));
        after(expect("after"));
    });
});
e = await assert.throwsAsync(RUN);
assert.eq(e.message, "1 failing tests detected");
assert.eq(forbidden, [], "No forbidden functions should have run");
assert.eq(expected, ["afterEach", "after"]);

// TESTS
expected = [];
forbidden = [];
describe("test failures", () => {
    describe("after/afterEach hooks and other tests should still run after test failures", () => {
        beforeEach(expect("beforeEach"));
        it("test1", FAILURE);
        it("test2", expect("test2"));
        afterEach(expect("afterEach"));
        after(expect("after"));
    });
});
e = await assert.throwsAsync(RUN);
assert.eq(e.message, "1 failing tests detected");
assert.eq(forbidden, [], "No forbidden functions should have run");
assert.eq(expected, ["beforeEach", "afterEach", "beforeEach", "test2", "afterEach", "after"]);

// AFTER EACH
expected = [];
forbidden = [];
describe("afterEach hook failures", () => {
    describe("skip pending afterEach's, and pending tests/beforeEach", () => {
        beforeEach(expect("beforeEach"));
        it("test1", expect("test1"));
        it("test2", forbid("test2"));
        afterEach(FAILURE);
        afterEach(forbid("afterEach"));
        after(expect("after"));
    });
});
e = await assert.throwsAsync(RUN);
assert.eq(e.message, "1 failing tests detected");
assert.eq(forbidden, [], "No forbidden functions should have run");
assert.eq(expected, ["beforeEach", "test1", "after"]);

// AFTER
expected = [];
forbidden = [];
describe("after hook failures", () => {
    describe("skip pending after hooks", () => {
        it("test", expect("test"));
        after(FAILURE);
        after(forbid("after"));
    });
});
e = await assert.throwsAsync(RUN);
assert.eq(e.message, "1 failing tests detected");
assert.eq(forbidden, [], "No forbidden functions should have run");
assert.eq(expected, ["test"]);

// Multiple failing tests report multiple failures
let spyloginfo = [];
let previous_info = jsTest.log.info;
jsTest.log.info = (msg) => spyloginfo.push(msg);
let spylogerr = [];
let previous_error = jsTest.log.error;
jsTest.log.error = (msg) => spylogerr.push(msg);
describe("multiple test failures", () => {
    it("test1", expect("test1"));
    it("test2", FAILURE);
    it("test3", expect("test3"));
    it("test4", FAILURE);
});
e = await assert.throwsAsync(RUN);
assert.eq(e.message, "2 failing tests detected");
assert.eq(spyloginfo, [
    "✔ multiple test failures > test1",
    "✔ multiple test failures > test3",
    "Test Report Summary:\n  2 passing\n\u001b[31m  2 failing\u001b[0m\nFailures and stacks are reprinted below.",
]);
assert.eq(spylogerr.length, 4);
assert.eq(spylogerr[0], "\u001b[31m✘ multiple test failures > test2\u001b[0m");
assert.eq(spylogerr[1], "\u001b[31m✘ multiple test failures > test4\u001b[0m");
// these are stack-dependent
assert.startsWith(spylogerr[2], "\u001b[31m✘ multiple test failures > test2\nIntentional failure\nFAILURE@jstests");
assert.startsWith(spylogerr[3], "\u001b[31m✘ multiple test failures > test4\nIntentional failure\nFAILURE@jstests");

jsTest.log.info = previous_info;
jsTest.log.error = previous_error;

/*
 * This test may show "0 passing" at the end of its execution, but that's because
 * we've manually triggered the test runs already there's no more tests in the
 * queue for the shell to run on close.
 */

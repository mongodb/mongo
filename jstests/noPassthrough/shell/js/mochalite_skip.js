import {after, afterEach, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";

// validate test execution and ordering

const log = [];
const logfn = (msg) => () => log.push(msg);
const die = () => {
    throw new Error("This should not run");
};

it("test1", logfn("test1"));
it.skip("test2", die);
describe("describe", () => {
    it("test3", logfn("test3"));
    it.skip("test4", die);
    describe.skip("should skip everything", () => {
        before(die);
        beforeEach(die);
        afterEach(die);
        after(die);
        it("test5", die);
        it.skip("test6", die);
    });
    describe.skip("should even ignore it.only", () => {
        before(die);
        beforeEach(die);
        afterEach(die);
        after(die);
        it.only("test5", die);
    });
});

await globalThis.__mochalite_closer();

// final check on sequencing from prior testpoints
assert.eq(log, ["test1", "test3"]);

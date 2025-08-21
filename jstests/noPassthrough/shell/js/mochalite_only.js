import {after, afterEach, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";

const log = [];
const logfn = (msg) => () => log.push(msg);
const die = () => {
    throw new Error("This should not run");
};

it("test1", die);
describe("describe", () => {
    it("test2", die);
    describe("contains an it.only", () => {
        beforeEach(logfn("beforeEach"));
        afterEach(logfn("afterEach"));
        it("test3", die);
        it.only("test4", logfn("test4"));
        it("test5", die);
    });
    describe("it takes precedence over sibling-describe", () => {
        it("test6", die);
        describe.only("this is only'ed, but knocked out by it.only at the same level", () => {
            before(die);
            beforeEach(die);
            afterEach(die);
            after(die);
            it("test7", die);
            it.only("test8", die);
        });
        it.only("test9", logfn("test9"));
        it("test10", die);
    });
    describe("nested describe", () => {
        describe.only("describe3", () => {
            describe("describe4", () => {
                it("test11", logfn("test11"));
            });
        });
    });
    describe("contains normal it tests - nothing should run", () => {
        before(die);
        beforeEach(die);
        afterEach(die);
        after(die);
        it("test12", die);
    });
    describe("top-level it.only and another one within a describe", () => {
        it.only("test13", logfn("test13"));
        describe("has an only", () => {
            it.only("test14", die);
        });
    });
});

await globalThis.__mochalite_closer();

assert.eq(log, ["beforeEach", "test4", "afterEach", "test9", "test11", "test13"]);

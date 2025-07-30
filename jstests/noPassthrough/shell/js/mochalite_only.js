
import {
    after,
    afterEach,
    before,
    beforeEach,
    describe,
    it,
} from "jstests/libs/mochalite.js";

const pass = () => {};
const die = () => {
    throw new Error("This should not run");
};

it('test1', die);
describe('describe', () => {
    it('test2', die);
    describe('contains an it.only', () => {
        let beforeCount = 0;
        let afterCount = 0;
        beforeEach(() => beforeCount++);
        afterEach(() => afterCount++);
        after(() => {
            // make sure hooks aren't run around tests that are skipped
            assert.eq(beforeCount, 1);
            assert.eq(afterCount, 1);
        });
        it('test3', die);
        it.only('test4', pass);
        it('test5', die);
    });
    describe('it takes precedence over sibling-describe', () => {
        it('test6', die);
        describe.only("this is only'ed, but knocked out by it.only at the same level", () => {
            before(die);
            beforeEach(die);
            afterEach(die);
            after(die);
            it('test7', die);
            it.only('test8', die);
        });
        it.only('test9', pass);
        it('test10', die);
    });
    describe('nested describe', () => {
        describe.only('describe3', () => {
            describe('describe4', () => {
                it('test11', pass);
            });
        });
    });
    describe('contains normal it tests - nothing should run', () => {
        before(die);
        beforeEach(die);
        afterEach(die);
        after(die);
        it('test12', die);
    });
    describe('top-level it.only and another one within a describe', () => {
        it.only('test13', pass);
        describe('has an only', () => {
            it.only('test14', die);
        });
    });
});

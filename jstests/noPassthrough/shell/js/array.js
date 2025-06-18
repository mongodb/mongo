
import {describe, it} from "jstests/libs/mochalite.js";

describe("Array shims and polyfills", function() {
    it("contains", function() {
        const arr = [1, 2, 3];
        assert(Array.contains(arr, 2));
        assert(!Array.contains(arr, 4));

        const e = assert.throws(() => {
            Array.contains({}, 1);
        });
        assert.eq(e.message, "The first argument to Array.contains must be an array");
    });

    it("unique", function() {
        const arr = [1, 2, 3, 2, 3];
        assert.eq(Array.unique(arr), [1, 2, 3]);

        const e = assert.throws(() => {
            Array.unique({}, 1);
        });
        assert.eq(e.message, "The first argument to Array.unique must be an array");
    });
    it("shuffle", function() {
        Random.setRandomSeed();

        const arr = [1, 2, 3, 4, 5];
        const shuffled = Array.shuffle(arr, 1);

        assert.eq(shuffled.length, arr.length);
        assert(Array.contains(shuffled, 1));
        assert(Array.contains(shuffled, 2));
        assert(Array.contains(shuffled, 3));
        assert(Array.contains(shuffled, 4));
        assert(Array.contains(shuffled, 5));

        const e = assert.throws(() => {
            Array.shuffle({}, 1);
        });
        assert.eq(e.message, "The first argument to Array.shuffle must be an array");
    });

    it("fetchRefs", function() {
        let arr = [];
        assert.eq(Array.fetchRefs(arr, "coll"), []);

        arr = [
            {getCollection: () => "coll1", fetch: () => "A"},
            {getCollection: () => "coll2", fetch: () => "B"},
            {getCollection: () => "coll2", fetch: () => "C"},
            {getCollection: () => "coll3", fetch: () => "D"}
        ];
        assert.eq(Array.fetchRefs(arr, "coll2"), ["B", "C"]);

        // non-matching collection returns none
        assert.eq(Array.fetchRefs(arr, "collNA"), []);

        // unspecified collection returns all
        assert.eq(Array.fetchRefs(arr), ["A", "B", "C", "D"]);

        const e = assert.throws(() => {
            Array.fetchRefs({});
        });
        assert.eq(e.message, "The first argument to Array.fetchRefs must be an array");
    });

    it("sum", function() {
        let arr = [];
        assert.isnull(Array.sum(arr));

        arr = [1, 2, 3];
        assert.eq(Array.sum(arr), 6);

        const e = assert.throws(() => {
            Array.sum({});
        });
        assert.eq(e.message, "The first argument to Array.sum must be an array");
    });

    it("avg", function() {
        let arr = [];
        assert.isnull(Array.avg(arr));

        arr = [1, 2, 3];
        assert.eq(Array.avg(arr), 2);

        const e = assert.throws(() => {
            Array.avg({});
        });
        assert.eq(e.message, "The first argument to Array.avg must be an array");
    });

    it("stdDev", function() {
        let arr = [];
        assert.eq(Array.stdDev(arr), NaN);

        arr = [1, 1, 1, 7, 7, 7];
        assert.eq(Array.stdDev(arr), 3);

        const e = assert.throws(() => {
            Array.stdDev({});
        });
        assert.eq(e.message, "The first argument to Array.stdDev must be an array");
    });
});

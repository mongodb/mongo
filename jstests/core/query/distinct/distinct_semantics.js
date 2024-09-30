/**
 * Test that 'distinct' command returns expected results for various inputs.
 */

import {assertArrayEq} from "jstests/aggregation/extras/utils.js";

/**
 * The distinct command will be formed as 'coll.distinct(field, filter)'.
 *
 * We'll try each case with and without an index. There are tests elsewhere for specific index
 * scenarios. Here, unless 'index' is specified, we'll sanity check with a non-compound asc index on
 * the distinct 'field'. In most cases it will cause DISTINCT_SCAN.
 *
 * The result with and without an index should be the same, but there are known bugs when this isn't
 * so. The cases are documented by providing 'resultWithIndex' and the relevant ticket number.
 */
const testCases = [
    /**
     * Empty result.
     */
    {
        docs: [],
        field: "a",
        result: [],
    },
    {
        docs: [{}],
        field: "a",
        result: [],
        resultWithIndex: [null] /* SERVER-14832 */,
    },
    {
        docs: [{a: []}],
        field: "a",
        result: [],
        resultWithIndex: [undefined] /* SERVER-31343 */,
    },
    {
        docs: [{a: 1}],
        field: "b",
        result: [],
        resultWithIndex: [null] /* SERVER-14832 */,
    },

    /**
     *  Values that in some other MQL contexts might be treated as missing but not by 'distinct()'.
     */
    {
        docs: [{a: null}],
        field: "a",
        result: [null],
    },
    {
        docs: [{a: undefined}],
        field: "a",
        result: [undefined],
    },
    {
        docs: [{a: {}}],
        field: "a",
        result: [{}],
    },
    {
        docs: [{a: [[]]}],
        field: "a",
        result: [[]],
        resultWithIndex: [] /* SERVER-83620 */,
    },

    /**
     * Scalars.
     */
    {
        // Same values of different numeric types.
        docs: [{a: 2.0}, {a: 3}, {a: 1}, {a: NumberDecimal(1)}, {a: NumberInt(2)}],
        field: "a",
        result: [1, 2, 3],
    },
    {
        // Various non-numeric types.
        docs: [{a: {b: 1}}, {a: "str"}, {a: null}, {a: "str"}, {a: null}, {a: {b: 1}}],
        field: "a",
        result: [null, "str", {b: 1}],
    },
    {
        // The comparison operator in the filter does type bracketing.
        docs: [{a: 2}, {a: "str"}, {a: null}, {a: 3}, {a: NumberInt(3)}, {a: {b: 1}}],
        field: "a",
        filter: {a: {$gt: 2}},
        result: [3],
    },
    {
        // Filter on a different field.
        docs: [{a: 1, b: 5}, {a: 2, b: 7}, {a: 1, b: 7}, {a: 1, b: 5}, {a: 3, b: "str"}, {a: 4}],
        field: "a",
        filter: {b: 5},
        result: [1],
    },

    /**
     * Arrays.
     */
    {
        // Elements of the first-level arrays are flattened.
        docs: [{a: [2, 3]}, {a: 1}, {a: 2}, {a: [1, [2, 3]]}, {a: [3, "str"]}],
        field: "a",
        result: [1, 2, 3, [2, 3], "str"],
        resultWithIndex: [1, 2, 3, "str"] /* SERVER-83620 */,
    },
    {
        // The filter is applied to the array value as a whole, before distinctifying.
        docs: [{a: 0}, {a: [1, 2]}, {a: [-1, 3]}, {a: [-2, [3]]}, {a: [3, "str"]}, {a: [3, null]}],
        field: "a",
        filter: {a: {$gt: 2}},
        result: [-1, 3, "str", null],
    },
    {
        // Numeric path component.
        docs: [{a: 0}, {a: [1, 2]}, {a: [1, 3]}, {a: [[2, 3], "str"]}, {a: [[[4]], null]}],
        field: "a.0",
        result: [1, 2, 3, [4]],
        index: {a: 1},
    },

    /**
     * Nested objects.
     */
    {
        // Arrays on the path don't cause the terminal arrays to be treated as nested.
        docs: [
            {a: [{b: {c: 1}}, {b: {c: [2, 20]}}, {b: {c: [3]}}]},
            {a: {b: [{c: [3, 30]}, {c: [4]}]}},
            {a: {b: {c: [1, 10]}}},
        ],
        field: "a.b.c",
        result: [1, 2, 3, 4, 10, 20, 30],
    },
    {
        // Arrays on the path can be accessed using numeric path components.
        docs: [
            {a: [{b: [{c: 2}, {c: [1, 10]}]}, {b: [{c: 3}, {c: 4}]}]},
            {a: [{b: [{c: 5}, {c: 10}]}]},
            {a: [{b: [{c: 6}]}]},
            {a: {b: {c: 7}}},
        ],
        field: "a.0.b.1.c",
        result: [1, 10],
        index: {"a.b.c": 1},
    },
    {
        // Missing paths don't contribute to the result.
        docs: [
            {a: {b: {c: 0}}},
            {x: 0},
            {a: 1},
            {a: null},
            {a: []},
            {a: {x: 2}},
            {a: {b: 3}},
            {a: {b: null}},
            {a: {b: {x: 4}}},
            {a: {b: []}}
        ],
        field: "a.b.c",
        result: [0],
        resultWithIndex: [0, null] /* SERVER-14832 */,
    },
    {
        // Arrays on the path are filtered as a whole before distinctifying.
        docs: [{a: [{b: 2}, {b: 4}]}, {a: [{b: 0}, {b: 1}]}],
        field: "a.b",
        filter: {"a.b": {$gt: 3}},
        result: [2, 4],
    },

    /**
     * Distinct on "_id" field (it might use special optimization).
     */
    {
        docs: [{_id: 1, a: 42}, {_id: 2, a: 42}],
        field: "_id",
        result: [1, 2],
    },
    {
        docs: [{_id: 1, a: 42}, {_id: 2, a: 42}],
        field: "_id",
        filter: {_id: {$gt: 1}},
        result: [2],
    },
    {
        docs: [{_id: {a: [1, 2]}}, {_id: {a: [2, 3]}}, {_id: {a: 3}}, {_id: {a: 4}}],
        field: "_id.a",
        result: [1, 2, 3, 4],
    },
    {
        docs: [{_id: {a: [1, 2]}}, {_id: {a: [2, 3]}}, {_id: {a: 3}}, {_id: {a: [1, 4]}}],
        field: "_id.a.0",
        result: [1, 2],
        index: {"_id.a": 1},
    },
];

/**
 * Asserts that at least one of 'expected1' and 'expected2' is equal to 'actual'.
 */
function assertEitherArrayEq({actual, expected1, expected2, extraErrorMsg}) {
    try {
        assertArrayEq({actual, expected: expected1, extraErrorMsg});
    } catch (ex) {
        assertArrayEq({actual, expected: expected2, extraErrorMsg});
    }
}

const coll = db.distinct_semantics;
(function runTests() {
    let result = {};

    for (let testCase of testCases) {
        coll.drop();
        coll.insertMany(testCase.docs);

        const distinctSpec = {"key": testCase.field, "query": testCase.filter};

        // Test with no secondary indexes.
        {
            const testDescription = `distinct("${testCase.field}", ${
                tojson(testCase.filter)}) over ${tojson(testCase.docs)}`;

            result = assert.commandWorked(coll.runCommand("distinct", distinctSpec),
                                          `Attempted ${testDescription}`);

            assertArrayEq({
                expected: testCase.result,
                actual: result.values,
                extraErrorMsg: `Unexpected result for ${testDescription}`
            });
        }

        // Test with an index.
        {
            const indexSpec = testCase.index ? testCase.index : {[testCase.field]: 1};

            const testDescription =
                `distinct("${testCase.field}", ${tojson(testCase.filter)}) over ${
                    tojson(testCase.docs)} with index ${tojson(indexSpec)}`;

            assert.commandWorked(coll.createIndex(indexSpec), `createIndex for ${testDescription}`);

            result = assert.commandWorked(
                coll.runCommand("distinct", {"key": testCase.field, "query": testCase.filter}),
                `Attempted ${testDescription}`);

            // Shard filtering might require fetching, so we may get the correct result even with
            // the index.
            // TODO SERVER-72748: Assert that we always fetch if the collection is sharded.
            assertEitherArrayEq({
                expected1: testCase.resultWithIndex ? testCase.resultWithIndex : testCase.result,
                expected2: testCase.result,
                actual: result.values,
                extraErrorMsg: `Unexpected result for ${testDescription}`
            });
        }
    }
})();

/**
 * Test the "explode for sort" optimization when the index contains a multikey field after the sort
 * field. This is a regression test for SERVER-56865.
 * @tags: [
 *   requires_fcv_81,
 *   # Makes assertions about the number of rejected plans
 *   assumes_no_implicit_index_creation,
 * ]
 */
import {
    hasRejectedPlans,
    planHasStage,
} from "jstests/libs/query/analyze_plan.js";

const coll = db[jsTestName()];
coll.drop();

function runTest({index, query, sort, assertSortMergeUsed}) {
    coll.dropIndexes();
    assert.commandWorked(coll.createIndex(index));
    const results = coll.find(query).sort(sort).toArray();
    const collScanResults = coll.find(query).sort(sort).hint({$natural: 1}).toArray();
    assert.eq(results,
              collScanResults,
              `Index: ${tojson(index)} Query: ${tojson(query)} Sort: ${tojson(sort)}`);

    const explain = coll.find(query).sort(sort).explain();
    assert.eq(0, hasRejectedPlans(explain), explain);
    if (assertSortMergeUsed) {
        assert(planHasStage(db, explain, "SORT_MERGE"), explain);
        assert(!planHasStage(db, explain, "SORT"), explain);
    } else {
        assert(!planHasStage(db, explain, "SORT_MERGE"), explain);
    }
}

assert.commandWorked(coll.insert([
    {_id: 1, a: 1, b: 1, mk: [1, 2], d: 23},
    {_id: 2, a: 1, b: 8, mk: [3, 4], d: 17},
    {_id: 3, a: 2, b: 5, mk: [5, 6], d: 10},
    {_id: 4, a: 2, b: 6, mk: [7, 8], d: 25},
    {_id: 5, a: 11, b: 10, mk2: [20, 30], d: 23},
    {_id: 6, a: 11, b: 80, mk2: [20, 29], d: 17},
    {_id: 7, a: 21, b: 50, mk2: [30, 35], d: 10},
    {_id: 8, a: 21, b: 60, mk2: [30, 32], d: 25},
]));

const testCases = [
    {
        index: {a: 1, b: 1},
        query: {a: {$in: [1, 2]}},
        sort: {b: 1},
        assertSortMergeUsed: true,
    },
    {
        index: {a: 1, b: 1, mk: 1},
        query: {a: {$in: [1, 2]}},
        sort: {b: 1},
        assertSortMergeUsed: true,
    },
    {
        index: {a: 1, b: 1, d: 1, mk: 1},
        query: {a: {$in: [1, 2]}},
        sort: {b: 1, d: 1},
        assertSortMergeUsed: true,
    },
    {
        index: {mk2: 1, b: 1, d: 1, mk: 1},
        query: {mk2: {$in: [20, 30]}},
        sort: {b: 1},
        assertSortMergeUsed: true,
    },
    {
        index: {mk2: 1, b: 1, d: 1, mk: 1},
        query: {mk2: {$in: [20, 30]}},
        sort: {b: 1, d: 1},
        assertSortMergeUsed: true,
    },
    {
        index: {mk: 1, mk2: 1, d: 1},
        query: {mk: {$in: [1, 3, 5]}},
        sort: {mk2: 1, d: 1},
        assertSortMergeUsed: false,
    },
    {
        index: {a: 1, mk: 1, b: 1},
        query: {a: {$in: [1, 2]}},
        sort: {mk: 1, b: 1},
        assertSortMergeUsed: false,
    }
];

for (const tc of testCases) {
    runTest(tc);
}

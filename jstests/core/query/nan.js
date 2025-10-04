/**
 * Tests basic NaN handling. Note that WiredTiger indexes handle -NaN and NaN differently.
 * @tags: [
 *   requires_getmore,
 * ]
 */
import {assertArrayEq} from "jstests/aggregation/extras/utils.js";

const coll = db.jstests_nan;
coll.drop();

const docs = [
    {_id: 0, a: -Infinity},
    {_id: 1, a: -3},
    {_id: 2, a: 0},
    {_id: 3, a: 3},
    {_id: 4, a: Infinity},
    {_id: 5, a: NaN},
    {_id: 6, a: -NaN},
    {_id: 7, a: undefined},
    {_id: 8, a: null},
    {_id: 9, a: []},
    {_id: 10, a: {b: 1}},
    {_id: 11, a: {b: 1}},
];
assert.commandWorked(coll.insert(docs));

/**
 * Ensures correct results for EQ, LT, LTE, GT, GTE, and IN cases.
 */
function testNaNComparisons() {
    // EQ
    let res = coll.find({a: NaN}).toArray();
    assertArrayEq({actual: res, expected: [docs[5], docs[6]]});

    // LT
    res = coll.find({a: {$lt: NaN}}).toArray();
    assertArrayEq({actual: res, expected: []});

    // LTE
    res = coll.find({a: {$lte: NaN}}).toArray();
    assertArrayEq({actual: res, expected: [docs[5], docs[6]]});

    // GT
    res = coll.find({a: {$gt: NaN}}).toArray();
    assertArrayEq({actual: res, expected: []});

    // GTE
    res = coll.find({a: {$gte: NaN}}).toArray();
    assertArrayEq({actual: res, expected: [docs[5], docs[6]]});

    // IN
    // Positive NaN should match both positive and negative NaN. Note that the second value protects
    // the $in from being optimized away.
    res = coll.find({a: {$in: [NaN, 1000]}}).toArray();
    assertArrayEq({actual: res, expected: [docs[5], docs[6]]});

    // Negative NaN should match both positive and negative NaN. Note that the second value protects
    // the $in from being optimized away.
    res = coll.find({a: {$in: [-NaN, 1000]}}).toArray();
    assertArrayEq({actual: res, expected: [docs[5], docs[6]]});

    // NaNs of different types should match both positive and negative NaN. Note that the second
    // value protects the $in from being optimized away.
    res = coll.find({a: {$in: [NumberDecimal(NaN), 1000]}}).toArray();
    assertArrayEq({actual: res, expected: [docs[5], docs[6]]});

    res = coll.find({a: {$in: [NumberDecimal(-NaN), 1000]}}).toArray();
    assertArrayEq({actual: res, expected: [docs[5], docs[6]]});

    // NE
    // Should match all documents except docs with _id 5 and 6.
    let docsCopy = [...docs];
    docsCopy.splice(5, 2);
    res = coll.find({a: {$ne: NaN}}).toArray();
    assertArrayEq({actual: res, expected: docsCopy});
}

// Unindexed.
testNaNComparisons();

// Indexed.
assert.commandWorked(coll.createIndex({a: 1}));
testNaNComparisons();

assert(coll.drop());
assert.commandWorked(coll.insert({a: NaN}));
assert.commandWorked(coll.insert({a: -NaN}));

/**
 * Ensures that documents with NaN values do not get matched when the query has a non-NaN value.
 */
function testNonNaNQuery() {
    const queries = [{a: 1}, {a: {$lt: 1}}, {a: {$lte: 1}}, {a: {$gt: 1}}, {a: {$gte: 1}}];
    for (const query of queries) {
        const cursor = coll.find(query);
        assert(!cursor.hasNext());
    }
}

// Unindexed.
testNonNaNQuery();

// Indexed.
assert.commandWorked(coll.createIndex({a: 1}));
testNonNaNQuery();

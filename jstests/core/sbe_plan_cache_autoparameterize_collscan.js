/**
 * Tests that auto-parameterized collection scan plans are correctly stored and in the SBE plan
 * cache, and that they can be correctly recovered from the cache with new parameter values.
 *
 * @tags: [
 *   assumes_read_concern_unchanged,
 *   assumes_read_preference_unchanged,
 *   assumes_unsharded_collection,
 *   does_not_support_stepdowns,
 *   # The SBE plan cache was introduced in 6.0.
 *   requires_fcv_60,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/analyze_plan.js");
load("jstests/libs/sbe_util.js");

// This test is specifically verifying the behavior of the SBE plan cache, which is only enabled
// when 'featureFlagSbeFull' is on.
if (!checkSBEEnabled(db, ["featureFlagSbeFull"])) {
    jsTestLog("Skipping test because SBE is not fully enabled");
    return;
}

const coll = db.sbe_plan_cache_autoparameterize_collscan;
coll.drop();

let data = [
    {_id: 0, a: 1, c: "foo"},
    {_id: 1, a: 2, c: "foo"},
    {_id: 2, a: 3, c: "foo"},
    {_id: 3, a: 4, c: "foo"},
    {_id: 4, a: 4, c: "foo"},
    {_id: 5, a: [3, 4, 5, 6], c: "foo"},
    {_id: 6, a: [3, 5, 8], c: "foo"},
    {_id: 7, c: "foo"},
    {_id: 8, a: [], c: "foo"},
    {_id: 9, a: undefined, c: "foo"},
    {_id: 10, a: null, c: "foo"},
    {_id: 11, a: [{b: 3}, {b: 4}], c: "foo"},
    {_id: 12, a: [{b: 5}, {b: 6}], c: "foo"},
    {_id: 13, a: "foo", c: "foo"},
    {_id: 14, a: /foo/, c: "foo"},
    {_id: 15, a: "zbarz", c: "foo"},
    // A 12-byte BinData where the last 6 bits are 1 and all preceding bits are 0.
    {_id: 16, a: BinData(0, "AAAAAAAAAAAAAAA/"), c: "foo"},
];
assert.commandWorked(coll.insert(data));

function assertSbePlanCacheEntryExists(cacheKey) {
    const entries =
        coll.aggregate([{$planCacheStats: {}}, {$match: {planCacheKey: cacheKey}}]).toArray();
    assert.eq(entries.length, 1, entries);
    const entry = entries[0];
    // The version:"2" field indicates that this is an SBE plan cache entry.
    assert.eq(entry.version, "2", entry);
    assert.eq(entry.planCacheKey, cacheKey, entry);
    // Since there is only ever one possible candidate plan (collection scan), we expect the cache
    // entry to be both active and pinned.
    assert.eq(entry.isActive, true, entry);
    assert.eq(entry.isPinned, true, entry);
}

// Given a document with the format {query: <filter>, projection: <projection>, sort: <sort>}, where
// each field is optional, runs the corresponding find command and returns the results as an array.
function runFindCommandFromShapeDoc(shape) {
    let cursor = coll.find(shape.query, shape.projection);
    if (shape.sort) {
        cursor = cursor.sort(shape.sort);
    }
    return cursor.toArray();
}

/**
 * Runs a single end-to-end test case for auto-parameterization of collection scan plans.
 *  - 'shape1' is a description of a find command as a document {query: <filter>, projection:
 *  <projection>, sort: <sort>}.
 *  - 'expectedResults1' is an array containing the results expected from running 'shape1' against
 *  the test collection. This function verifies that the actual results match the expected ones.
 *  The order of the result set is not considered significant (since not all test queries specify a
 *  sort).
 *  - 'shape2' is a second find command, expressed with the same format as 'shape1' and whose
 *  results are compared to `expectedResults2'. Again, the order of the result set is not
 *  significant.
 *  - If 'sameCacheKey' is true, then verifies that 'shape1' and 'shape2' have the same plan cache
 *  key using $planCacheStats. Otherwise, verifies that the two test queries have different plan
 *  cache keys.
 *
 * Also uses $planCacheStats to verify that the expected cache entries are created.
 */
function runTest(shape1, expectedResults1, shape2, expectedResults2, sameCacheKey) {
    // Flush the cache before starting the test to make sure we are starting from a clean slate.
    coll.getPlanCache().clear();

    for (let shape of [shape1, shape2]) {
        shape.collection = coll;
        shape.db = db;
    }

    const cacheKey1 = getPlanCacheKeyFromShape(shape1);
    const cacheKey2 = getPlanCacheKeyFromShape(shape2);
    if (sameCacheKey) {
        assert.eq(cacheKey1, cacheKey2, "expected SBE plan cache keys to be the same");
    } else {
        assert.neq(cacheKey1, cacheKey2, "expected SBE plan cache keys to be different");
    }

    // Run each query twice in order to make sure that each query still returns the same results
    // after the state of the cache has been altered.
    [...Array(2)].forEach(() => {
        const actualResults1 = runFindCommandFromShapeDoc(shape1);
        assert.sameMembers(actualResults1, expectedResults1, shape1);
        assertSbePlanCacheEntryExists(cacheKey1);

        const actualResults2 = runFindCommandFromShapeDoc(shape2);
        assert.sameMembers(actualResults2, expectedResults2, shape2);
        assertSbePlanCacheEntryExists(cacheKey2);
    });
}

// Test basic auto-parameterization of $eq.
runTest({query: {a: 1}},
        [{_id: 0, a: 1, c: "foo"}],
        {query: {a: 4}},
        [{_id: 3, a: 4, c: "foo"}, {_id: 4, a: 4, c: "foo"}, {_id: 5, a: [3, 4, 5, 6], c: "foo"}],
        true);

// Test that different projections result in different cache keys.
runTest({query: {a: 1}, projection: {_id: 0}},
        [{a: 1, c: "foo"}],
        {query: {a: 4}, projection: {_id: 0, c: 0}},
        [{a: 4}, {a: 4}, {a: [3, 4, 5, 6]}],
        false);

// Test that different sorts result in different cache keys.
runTest({query: {a: 1}, sort: {_id: -1}, projection: {c: 0}},
        [{_id: 0, a: 1}],
        {query: {a: 4}, sort: {_id: 1}, projection: {c: 0}},
        [{_id: 3, a: 4}, {_id: 4, a: 4}, {_id: 5, a: [3, 4, 5, 6]}],
        false);

// Queries on different paths should result in different cache keys.
runTest({query: {a: 1}},
        [{_id: 0, a: 1, c: "foo"}],
        {query: {"a.b": 6}},
        [{_id: 12, a: [{b: 5}, {b: 6}], c: "foo"}],
        false);

// Test $eq:null queries do not get auto-parameterized.
runTest({query: {a: 1}, projection: {c: 0}},
        [{_id: 0, a: 1}],
        {query: {a: null}, projection: {c: 0}},
        [{_id: 7}, {_id: 9, a: undefined}, {_id: 10, a: null}],
        false);

// Test basic auto-parameterization of $lt.
runTest({query: {a: {$lt: 3}}, projection: {c: 0}},
        [{_id: 0, a: 1}, {_id: 1, a: 2}],
        {query: {a: {$lt: 4}}, projection: {c: 0}},
        [
            {_id: 0, a: 1},
            {_id: 1, a: 2},
            {_id: 2, a: 3},
            {_id: 5, a: [3, 4, 5, 6]},
            {_id: 6, a: [3, 5, 8]}
        ],
        true);

// Test basic auto-parameterization of $lte.
runTest({query: {a: {$lte: 2}}, projection: {c: 0}},
        [{_id: 0, a: 1}, {_id: 1, a: 2}],
        {query: {a: {$lte: 3}}, projection: {c: 0}},
        [
            {_id: 0, a: 1},
            {_id: 1, a: 2},
            {_id: 2, a: 3},
            {_id: 5, a: [3, 4, 5, 6]},
            {_id: 6, a: [3, 5, 8]}
        ],
        true);

// Test basic auto-parameterization of $gt.
runTest({query: {a: {$gt: 5}}, projection: {c: 0}},
        [{_id: 5, a: [3, 4, 5, 6]}, {_id: 6, a: [3, 5, 8]}],
        {query: {a: {$gt: 6}}, projection: {c: 0}},
        [{_id: 6, a: [3, 5, 8]}],
        true);

// Test basic auto-parameterization of $gte.
runTest({query: {a: {$gte: 6}}, projection: {c: 0}},
        [{_id: 5, a: [3, 4, 5, 6]}, {_id: 6, a: [3, 5, 8]}],
        {query: {a: {$gte: 7}}, projection: {c: 0}},
        [{_id: 6, a: [3, 5, 8]}],
        true);

// Test basic auto-parameterization of $bitsAllClear.
runTest({query: {a: {$bitsAllClear: [0, 3]}}, projection: {_id: 1}},
        [{_id: 1}, {_id: 3}, {_id: 4}, {_id: 5}, {_id: 16}],
        {query: {a: {$bitsAllClear: [0, 2, 65]}}, projection: {_id: 1}},
        [{_id: 1}, {_id: 6}, {_id: 16}],
        true);

// Test basic auto-parameterization of $bitsAllSet.
runTest({query: {a: {$bitsAllSet: [0, 2]}}, projection: {_id: 1}},
        [{_id: 5}, {_id: 6}],
        {query: {a: {$bitsAllSet: [0, 1]}}, projection: {_id: 1}},
        [{_id: 2}, {_id: 5}, {_id: 6}],
        true);

// Test basic auto-parameterization of $bitsAnyClear.
runTest({query: {a: {$bitsAnyClear: 1}}, projection: {_id: 1}},
        [{_id: 1}, {_id: 3}, {_id: 4}, {_id: 5}, {_id: 6}, {_id: 16}],
        {query: {a: {$bitsAnyClear: 3}}, projection: {_id: 1}},
        [{_id: 0}, {_id: 1}, {_id: 3}, {_id: 4}, {_id: 5}, {_id: 6}, {_id: 16}],
        true);

// Test basic auto-parameterization of $bitsAnySet.
runTest({query: {a: {$bitsAnySet: 1}}, projection: {_id: 1}},
        [{_id: 0}, {_id: 2}, {_id: 5}, {_id: 6}],
        {query: {a: {$bitsAnySet: 3}}, projection: {_id: 1}},
        [{_id: 0}, {_id: 1}, {_id: 2}, {_id: 5}, {_id: 6}],
        true);

// Auto-parameterization of bit-test operators should work even if looking past 64 bits is required
// in order to match against binary data.
runTest({query: {a: {$bitsAllSet: [0, 94]}}, projection: {_id: 1}},
        [],
        {query: {a: {$bitsAllSet: [88, 89, 90, 91, 92, 93]}}, projection: {_id: 1}},
        [{_id: 16}],
        true);

// Test auto-parameterization of $elemMatch object.
runTest({query: {a: {$elemMatch: {b: {$gt: 3, $lt: 5}}}}, projection: {_id: 1}},
        [{_id: 11}],
        {query: {a: {$elemMatch: {b: {$gt: 4, $lt: 6}}}}, projection: {_id: 1}},
        [{_id: 12}],
        true);

// Test a conjunction with two auto-parameterized predicates.
runTest({query: {$and: [{a: 3}, {a: 6}]}, projection: {_id: 1}},
        [{_id: 5}],
        {query: {$and: [{a: 5}, {a: 8}]}, projection: {_id: 1}},
        [{_id: 6}],
        true);

// Test a disjunction with two auto-parameterized predicates.
runTest({query: {$or: [{a: 3}, {a: 6}]}, projection: {_id: 1}},
        [{_id: 2}, {_id: 5}, {_id: 6}],
        {query: {$or: [{a: 1}, {a: 4}]}, projection: {_id: 1}},
        [{_id: 0}, {_id: 3}, {_id: 4}, {_id: 5}],
        true);

// Test a $nor with three auto-parmeterized child predicates.
runTest({query: {$nor: [{a: 3}, {a: 6}], a: {$type: "number"}}, projection: {_id: 1}},
        [{_id: 0}, {_id: 1}, {_id: 3}, {_id: 4}],
        {query: {$nor: [{a: 1}, {a: 4}], a: {$type: "number"}}, projection: {_id: 1}},
        [{_id: 1}, {_id: 2}, {_id: 6}],
        true);

// Test an auto-parameterized $ne.
runTest({query: {$and: [{a: {$ne: 4}}, {a: {$type: "number"}}]}, projection: {_id: 1}},
        [{_id: 0}, {_id: 1}, {_id: 2}, {_id: 6}],
        {query: {$and: [{a: {$ne: 6}}, {a: {$type: "number"}}]}, projection: {_id: 1}},
        [{_id: 0}, {_id: 1}, {_id: 2}, {_id: 3}, {_id: 4}, {_id: 6}],
        true);

// Test an auto-parameterized $not-$lt.
runTest({query: {$and: [{a: {$not: {$lt: 4}}}, {a: {$type: "number"}}]}, projection: {_id: 1}},
        [{_id: 3}, {_id: 4}],
        {query: {$and: [{a: {$not: {$lt: 3}}}, {a: {$type: "number"}}]}, projection: {_id: 1}},
        [{_id: 2}, {_id: 3}, {_id: 4}, {_id: 5}, {_id: 6}],
        true);

// Verify that $exists queries are not auto-parameterized, meaning that $exists:true and
// $exists:false queries get different cache keys.
runTest({query: {a: {$exists: true}}, projection: {_id: 1}},
        [
            {_id: 0},
            {_id: 1},
            {_id: 2},
            {_id: 3},
            {_id: 4},
            {_id: 5},
            {_id: 6},
            {_id: 8},
            {_id: 9},
            {_id: 10},
            {_id: 11},
            {_id: 12},
            {_id: 13},
            {_id: 14},
            {_id: 15},
            {_id: 16},
        ],
        {query: {a: {$exists: false}}, projection: {_id: 1}},
        [{_id: 7}],
        false);

// Test that comparisons expressed as $expr are not auto-parameterized.
runTest({query: {$expr: {$eq: ["$a", 3]}}, projection: {_id: 1}},
        [{_id: 2}],
        {query: {$expr: {$eq: ["$a", 4]}}, projection: {_id: 1}},
        [{_id: 3}, {_id: 4}],
        false);
runTest({query: {$expr: {$lt: ["$a", 3]}, a: {$type: "number"}}, projection: {_id: 1}},
        [{_id: 0}, {_id: 1}],
        {query: {$expr: {$lt: ["$a", 4]}, a: {$type: "number"}}, projection: {_id: 1}},
        [{_id: 0}, {_id: 1}, {_id: 2}],
        false);
runTest({query: {$expr: {$lte: ["$a", 3]}, a: {$type: "number"}}, projection: {_id: 1}},
        [{_id: 0}, {_id: 1}, {_id: 2}],
        {query: {$expr: {$lte: ["$a", 4]}, a: {$type: "number"}}, projection: {_id: 1}},
        [{_id: 0}, {_id: 1}, {_id: 2}, {_id: 3}, {_id: 4}],
        false);
runTest({query: {$expr: {$gt: ["$a", 2]}, a: {$type: "number"}}, projection: {_id: 1}},
        [{_id: 2}, {_id: 3}, {_id: 4}, {_id: 5}, {_id: 6}],
        {query: {$expr: {$gt: ["$a", 3]}, a: {$type: "number"}}, projection: {_id: 1}},
        [{_id: 3}, {_id: 4}, {_id: 5}, {_id: 6}],
        false);
runTest({query: {$expr: {$gte: ["$a", 2]}, a: {$type: "number"}}, projection: {_id: 1}},
        [{_id: 1}, {_id: 2}, {_id: 3}, {_id: 4}, {_id: 5}, {_id: 6}],
        {query: {$expr: {$gte: ["$a", 3]}, a: {$type: "number"}}, projection: {_id: 1}},
        [{_id: 2}, {_id: 3}, {_id: 4}, {_id: 5}, {_id: 6}],
        false);

// Test that the entire list of $in values is treated as a parameter.
runTest({query: {a: {$in: [1, 2]}}, projection: {_id: 1}},
        [{_id: 0}, {_id: 1}],
        {query: {a: {$in: [1, 2, 3, 4]}}, projection: {_id: 1}},
        [{_id: 0}, {_id: 1}, {_id: 2}, {_id: 3}, {_id: 4}, {_id: 5}, {_id: 6}],
        true);

// Adding a null value to an $in inhibits auto-parameterization.
runTest({query: {a: {$in: [1, 2]}}, projection: {_id: 1}},
        [{_id: 0}, {_id: 1}],
        {query: {a: {$in: [1, 2, null]}}, projection: {_id: 1}},
        [{_id: 0}, {_id: 1}, {_id: 7}, {_id: 9}, {_id: 10}],
        false);

// Adding a regex to an $in inhibits auto-parameterization.
runTest({query: {a: {$in: [1, 2]}}, projection: {_id: 1}},
        [{_id: 0}, {_id: 1}],
        {query: {a: {$in: [1, 2, /foo/]}}, projection: {_id: 1}},
        [{_id: 0}, {_id: 1}, {_id: 13}, {_id: 14}],
        false);

// Adding a nested array to an $in inhibits auto-parameterization.
runTest({query: {a: {$in: [1, 2]}}, projection: {_id: 1}},
        [{_id: 0}, {_id: 1}],
        {query: {a: {$in: [1, 2, []]}}, projection: {_id: 1}},
        [{_id: 0}, {_id: 1}, {_id: 8}],
        false);

// Test auto-parameterization of $mod.
runTest({query: {a: {$mod: [2, 0]}}, projection: {_id: 1}},
        [{_id: 1}, {_id: 3}, {_id: 4}, {_id: 5}, {_id: 6}],
        {query: {a: {$mod: [3, 1]}}, projection: {_id: 1}},
        [{_id: 0}, {_id: 3}, {_id: 4}, {_id: 5}],
        true);

// Test auto-parameterization of $size.
runTest({query: {a: {$size: 4}}, projection: {_id: 1}},
        [{_id: 5}],
        {query: {a: {$size: 2}}, projection: {_id: 1}},
        [{_id: 11}, {_id: 12}],
        true);

// Test auto-parameterization of $where.
runTest({query: {$where: "this.a == 1;"}, projection: {_id: 1}},
        [{_id: 0}],
        {query: {$where: "this.a == 2;"}, projection: {_id: 1}},
        [{_id: 1}],
        true);
// $where queries use the same plan regardless of the exact JS code.
runTest({
    query: {
        $where: function() {
            const date = new Date();
            return this.c == 1;
        }
    },
    projection: {_id: 1}
},
        [],
        {query: {$where: "this.a == 2;"}, projection: {_id: 1}},
        [{_id: 1}],
        true);

// Test auto-parameterization of $regex.
runTest({query: {a: /foo/}, projection: {_id: 1}},
        [{_id: 13}, {_id: 14}],
        {query: {a: {$regex: "bar"}}, projection: {_id: 1}},
        [{_id: 15}],
        true);

// Test that $type is not auto-parameterized.
//
// TODO SERVER-64776: Re-enable auto-parameterization for $type predicates.
runTest({query: {a: {$type: "double"}}, projection: {_id: 1}},
        [{_id: 0}, {_id: 1}, {_id: 2}, {_id: 3}, {_id: 4}, {_id: 5}, {_id: 6}],
        {query: {a: {$type: ["string", "regex"]}}, projection: {_id: 1}},
        [{_id: 13}, {_id: 14}, {_id: 15}],
        false);

// Test that $type is not auto-parameterized when the type set includes "array".
runTest({query: {a: {$type: ["string", "regex"]}}, projection: {_id: 1}},
        [{_id: 13}, {_id: 14}, {_id: 15}],
        {query: {a: {$type: ["string", "array"]}}, projection: {_id: 1}},
        [{_id: 5}, {_id: 6}, {_id: 8}, {_id: 11}, {_id: 12}, {_id: 13}, {_id: 15}],
        false);
}());

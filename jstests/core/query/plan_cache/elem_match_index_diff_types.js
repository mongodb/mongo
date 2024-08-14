/**
 * Test that reusing a plan cache entry correctly distinguishes whether the entry's predicate is
 * compatible with the query. Specifically when using an index for a predicate inside an $elemMatch
 * when the entry and predicate have different data types which affect index compatibility.
 */

import {getPlanCacheKeyFromShape} from "jstests/libs/analyze_plan.js";

const coll = db.elem_match_index_diff_types;
const indexCollation = {
    collation: {locale: 'en'},
};

/**
 * Run the test scenario:
 * - Insert values in `documentList`.
 * - Create two indexes using `arrayFieldName` so have we more than one query solution candidate.
 *   Both indexes will be created with the 'indexCollation' collation.
 * - Run `notCachedQuery` as a sanity check. Expected it will produce two documents. This query will
 *   run with the specified 'queryCollation'.
 * - Run `cachedQuery` (which should use an index) twice to create an activated plan cache
 *   entry. This query will run with the specified 'queryCollation'.
 * - Run `notCachedQuery` again to verify that is still produces two documents.
 * - Verify via explain that the plan cache keys generated for `cachedQuery` and `notCachedQuery`
 *   differ.
 */
function runTest(documentList, arrayFieldName, cachedQuery, notCachedQuery, queryCollation) {
    coll.drop();
    assert.commandWorked(coll.insert(documentList));

    // Create two indexes so the plan will be cached. Note that the indexes both have a collation
    // which means that certain expressions/predicates will be considered incompatible if the
    // collation on the query does not match.
    assert.commandWorked(coll.createIndex({[arrayFieldName]: 1}, indexCollation));
    assert.commandWorked(coll.createIndex({[arrayFieldName]: -1}, indexCollation));

    const key1 =
        getPlanCacheKeyFromShape({query: cachedQuery, collection: coll, db: db, ...queryCollation});
    const key2 = getPlanCacheKeyFromShape(
        {query: notCachedQuery, collection: coll, db: db, ...queryCollation});
    assert.neq(key1, key2, "Plan cache keys should differ.");

    // Sanity check that the test query returns the correct results.
    assert.eq(2, coll.find(notCachedQuery).itcount());

    // Create the activated plan cache entry.
    for (let i = 0; i < 2; ++i) {
        coll.find(cachedQuery).itcount();
    }

    // Verify that we don't erroneously use the plan cache entry.
    assert.eq(2, coll.find(notCachedQuery).itcount());
}

// Test for the $elemMatch value expression.
{
    const documentList = [
        {_id: 1, arr: ["lowercase1"]},
        {_id: 2, arr: ["lowercase2"]},
    ];
    const cachedQuery = {arr: {$elemMatch: {$gte: NumberLong("78219")}}};
    const notCachedQuery = {arr: {$elemMatch: {$gte: "UPPERCASE"}}};
    const queryCollation = {};

    // Because the query and index collation differ, we should not re-use the cache entry for the
    // string comparison.
    runTest(documentList, "arr", cachedQuery, notCachedQuery, queryCollation);
}

// Test for the $elemMatch object expression.
{
    const documentList = [
        {_id: 1, arr: [{x: "lowercase1"}]},
        {_id: 2, arr: [{x: "lowercase2"}]},
    ];
    const cachedQuery = {arr: {$elemMatch: {x: {$gte: NumberLong("78219")}}}};
    const notCachedQuery = {arr: {$elemMatch: {x: {$gte: "UPPERCASE"}}}};
    const queryCollation = {};

    // Because the query and index collation differ, we should not re-use the cache entry for the
    // string comparison.
    runTest(documentList, "arr.x", cachedQuery, notCachedQuery, queryCollation);
}

// Test for $elemMatch value nested inside $elemMatch object expression.
{
    const documentList = [
        {_id: 1, arr: [{x: ["lowercase1"]}]},
        {_id: 2, arr: [{x: ["lowercase2"]}]},
    ];
    const cachedQuery = {arr: {$elemMatch: {x: {$elemMatch: {$gte: NumberLong("78219")}}}}};
    const notCachedQuery = {arr: {$elemMatch: {x: {$elemMatch: {$gte: "UPPERCASE"}}}}};
    const queryCollation = {};

    // Because the query and index collation differ, we should not re-use the cache entry for the
    // string comparison.
    runTest(documentList, "arr.x", cachedQuery, notCachedQuery, queryCollation);
}

// Test for $elemMatch object nested inside $elemMatch object expression.
{
    const documentList = [
        {_id: 1, arr: [{x: [{y: "lowercase1"}]}]},
        {_id: 2, arr: [{x: [{y: "lowercase2"}]}]},
    ];
    const cachedQuery = {arr: {$elemMatch: {x: {$elemMatch: {y: {$gte: NumberLong("78219")}}}}}};
    const notCachedQuery = {arr: {$elemMatch: {x: {$elemMatch: {y: {$gte: "UPPERCASE"}}}}}};
    const queryCollation = {};

    // Because the query and index collation differ, we should not re-use the cache entry for the
    // string comparison.
    runTest(documentList, "arr.x.y", cachedQuery, notCachedQuery, queryCollation);
}

// Test value $elemMatch with $in.
{
    const documentList = [
        {_id: 1, arr: [123, 0]},
        {_id: 2, arr: [123]},
        {_id: 3, arr: ["2", "0"]},
        {_id: 4, arr: ["10", "0"]},
    ];
    const cachedQuery = {arr: {$elemMatch: {$in: [123, 456, 789]}}};
    const notCachedQuery = {arr: {$elemMatch: {$in: ["2", "3", "10"]}}};
    const numericOrderCollation = {collation: {locale: 'en', strength: 1, numericOrdering: true}};

    // Because the query and index collation differ, we should not re-use the cache entry for the
    // string comparison.
    runTest(documentList, "arr", cachedQuery, notCachedQuery, numericOrderCollation);
}

// Test value $elemMatch with $not.
{
    const documentList = [
        {_id: 1, arr: [0, 2]},
        {_id: 2, arr: [2]},
    ];
    const queryCollation = indexCollation;

    // Set the activated plan cache entry with the $not query. Importantly, the predicate under the
    // $not is exact; this allows us to build an IXSCAN plan where the index bounds are built for
    // for the $not by inverting the $lte bounds.
    const cachedQuery = {arr: {$elemMatch: {$not: {$lte: 1}}}};
    const notCachedQuery = {arr: {$elemMatch: {$not: {$lte: []}}}};

    // We should not re-use the cache entry because the second query has an inexact comparison.
    // Using the cached plan would cause a tassert because we cannot invert inexact bounds.
    runTest(documentList, "arr", cachedQuery, notCachedQuery, queryCollation);
}

// Test object $elemMatch with $not.
{
    const documentList = [
        {_id: 1, arr: [{b: 5}]},
        {_id: 2, arr: [{b: 10}]},
    ];
    const queryCollation = indexCollation;

    // Set the activated plan cache entry with the $not query. Importantly, the predicate under the
    // $not is exact; this allows us to build an IXSCAN plan where the index bounds are built for
    // for the $not by inverting the $lte bounds.
    const cachedQuery = {arr: {$elemMatch: {b: {$not: {$lte: 1}}}}};
    const notCachedQuery = {arr: {$elemMatch: {b: {$not: {$lte: []}}}}};

    // We should not re-use the cache entry because the second query has an inexact comparison.
    // Using the cached plan would cause a tassert because we cannot invert inexact bounds.
    runTest(documentList, "arr.b", cachedQuery, notCachedQuery, queryCollation);
}

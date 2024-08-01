/**
 * Test that reusing a plan cache entry correctly distinguishes
 * whether the entry's predicate is compatible with the
 * query. Specifically when using an index for a predicate inside an
 * $elemMatch when the entry and predicate have different data types
 * which affect index compatibility.
 */

import {getPlanCacheKeyFromShape} from "jstests/libs/analyze_plan.js";

(function() {
"use strict";

/**
 * Run the test scenario:
 * - Insert values in `documentList`.
 * - Create two indexes using `arrayFieldName` so have we more than one query solution candidate
 * - Run `indexIncompatibleMatch` (which should not use an index) as a sanity check. Expected it
 *   will produce two documents
 * - Run `indexCompatibleMatch` (which should use an index) twice to create an activated plan cache
 *   entry
 * - Run `indexIncompatibleMatch` again to verify that is still produces two documents
 * - Verify via explain that the plan cache keys generated for `indexCompatibleMatch` and
 *   `indexIncompatibleMatch` differ
 */
function runTest(documentList, arrayFieldName, indexCompatibleMatch, indexIncompatibleMatch) {
    let coll = db.elem_match_index_diff_types;
    coll.drop();
    assert.commandWorked(coll.insert(documentList));

    // Create two indexes so the plan will be cached. Note that the indexes both have a collation
    // which means that certain expressions/predicates will be considered incompatible if the
    // collation on the query does not match.
    let indexSpec = {};

    indexSpec[arrayFieldName] = 1;
    assert.commandWorked(coll.createIndex(indexSpec, {
        collation: {locale: 'en'},
    }));

    // Add an additional field.
    indexSpec.unused = 1;
    assert.commandWorked(coll.createIndex(indexSpec, {
        collation: {locale: 'en'},
    }));

    const key1 = getPlanCacheKeyFromShape({query: indexCompatibleMatch, collection: coll, db: db});
    const key2 =
        getPlanCacheKeyFromShape({query: indexIncompatibleMatch, collection: coll, db: db});
    assert.neq(key1, key2, "Plan cache keys should differ.");

    // Sanity check that the $elemMatch with the default collation returns the
    // correct results.
    assert.eq(2, coll.find(indexIncompatibleMatch).itcount());

    // Create the activated plan cache entry.
    for (let i = 0; i < 2; ++i) {
        coll.find(indexCompatibleMatch).itcount();
    }

    // Verify that we don't erroneously use the plan cache entry.
    assert.eq(2, coll.find(indexIncompatibleMatch).itcount());
}

// Test for the $elemMatch value expression.
{
    const documentList = [
        {_id: 1, arr: ["lowercase1"]},
        {_id: 2, arr: ["lowercase2"]},
    ];
    const indexCompatibleMatch = {arr: {$elemMatch: {$gte: NumberLong("78219")}}};
    const indexIncompatibleMatch = {arr: {$elemMatch: {$gte: "UPPERCASE"}}};
    runTest(documentList, "arr", indexCompatibleMatch, indexIncompatibleMatch);
}

// Test for the $elemMatch object expression.
{
    const documentList = [
        {_id: 1, arr: [{x: "lowercase1"}]},
        {_id: 2, arr: [{x: "lowercase2"}]},
    ];
    const indexCompatibleMatch = {arr: {$elemMatch: {x: {$gte: NumberLong("78219")}}}};
    const indexIncompatibleMatch = {arr: {$elemMatch: {x: {$gte: "UPPERCASE"}}}};
    runTest(documentList, "arr.x", indexCompatibleMatch, indexIncompatibleMatch);
}

// Test for $elemMatch value nested inside $elemMatch object expression.
{
    const documentList = [
        {_id: 1, arr: [{x: ["lowercase1"]}]},
        {_id: 2, arr: [{x: ["lowercase2"]}]},
    ];
    const indexCompatibleMatch = {
        arr: {$elemMatch: {x: {$elemMatch: {$gte: NumberLong("78219")}}}}
    };
    const indexIncompatibleMatch = {arr: {$elemMatch: {x: {$elemMatch: {$gte: "UPPERCASE"}}}}};
    runTest(documentList, "arr.x", indexCompatibleMatch, indexIncompatibleMatch);
}

// Test for $elemMatch object nested inside $elemMatch object expression.
{
    const documentList = [
        {_id: 1, arr: [{x: [{y: "lowercase1"}]}]},
        {_id: 2, arr: [{x: [{y: "lowercase2"}]}]},
    ];
    const indexCompatibleMatch = {
        arr: {$elemMatch: {x: {$elemMatch: {y: {$gte: NumberLong("78219")}}}}}
    };
    const indexIncompatibleMatch = {arr: {$elemMatch: {x: {$elemMatch: {y: {$gte: "UPPERCASE"}}}}}};
    runTest(documentList, "arr.x.y", indexCompatibleMatch, indexIncompatibleMatch);
}
})();

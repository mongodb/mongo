/**
 * Tests that queries with a negation of a comparison to 'null' with '$eq', '$gte', and 'lte' do
 * not incorrectly re-use query plans in cache that may be cause incorrect behavior due to null
 * semantics. This test was initially designed to reproduce incorrect plan cache behavior bug in
 * SERVER-56468.
 * @tags: [
 *   does_not_support_stepdowns
 * ]
 */

(function() {
"use strict";

const coll = db.neq_null_correctly_cache;
coll.drop();

assert.commandWorked(coll.createIndex({val: 1}));
assert.commandWorked(coll.insertMany([{val: []}, {val: true}, {val: true}]));

function runTest(testQueryPred, cachedQueryPred) {
    coll.getPlanCache().clear();

    // The 'testQueryPred' is always a null equality query of some form. All three of the documents
    // in the collection should match this not-equal-to-null predicate.
    assert.eq(3,
              coll.find({val: {$not: testQueryPred}}, {_id: 0, val: 1}).sort({val: 1}).itcount());

    // Run a query twice to create an active plan cache entry.
    for (let i = 0; i < 2; ++i) {
        assert.eq(
            1,
            coll.find({val: {$not: cachedQueryPred}}, {_id: 0, val: 1}).sort({val: 1}).itcount());
    }
    // The results from the original query should not have changed, despite any possible changes in
    // the state of the plan cache from the previous query.
    assert.eq(3,
              coll.find({val: {$not: testQueryPred}}, {_id: 0, val: 1}).sort({val: 1}).itcount());
}

runTest({$eq: null}, {$eq: true});
runTest({$gte: null}, {$gte: true});
runTest({$lte: null}, {$lte: true});
}());

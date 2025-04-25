/**
 * Checks plan cache and plan re-use behavior for queries affected by the
 * internalQueryPlannerDisableDottedPathIsSubsetOfExistsTrue query knob. We test that the same query
 * does not re-use a cached plan when the knob is changed from disabled to enabled. This behavior is
 * desired as enabling/disabling the knob will change the query results and thus, we do not want to
 * re-use a plan that was cached under a different knob value.
 *
 * The plan is not re-used when the knob is changed because the knob changes the return value of
 * 'isSubsetOf()' which is used as a plan cache index discriminator and thus, the plan cache key for
 * the exact same query will be different depending on the value of the knob.
 *
 * See SERVER-36635 for more details on the interaction between the query and partial index used in
 * this test.
 */

import {
    getPlanCacheKeyFromExplain,
    getWinningPlanFromExplain,
    isCollscan,
    isIxscan
} from "jstests/libs/query/analyze_plan.js";

const conn = MongoRunner.runMongod();
const dbName = jsTestName();
const db = conn.getDB(dbName);
assert.commandWorked(db.dropDatabase());
const coll = db.getCollection("test");

// This document matches the filter {"a.b": {$ne: null}} but not {"a.b": {$exists: true}}.
const docAffected = {
    _id: 0,
    a: [1],
    x: 1
};

// These documents match the filters {"a.b": {$ne: null}} AND {"a.b": {$exists: true}}.
const docsUnaffected = [
    {_id: 1, a: {b: 1}, x: 1},
    {_id: 2, a: {b: 1}, x: 1},
];
const query = {
    "a.b": {$ne: null},
    x: 1,
};

// Create 2 indexes so we have competing plans and thus, can cache the winning plan.
// The partial index on "a.b" will contain only the documents in docsUnaffected, i.e. 2 docs.
// The index on "x" will contain docAffected and the documents in docsUnaffected, i.e. 3 docs.
// Thus, the partial index is more selective and we expect the query to use the partial index.
coll.createIndex({"a.b": 1}, {partialFilterExpression: {"a.b": {$exists: true}}});
coll.createIndex({x: 1});
assert.commandWorked(coll.insertMany([docAffected, ...docsUnaffected]));

// Start with the knob disabled.
assert.commandWorked(db.adminCommand(
    {setParameter: 1, internalQueryPlannerDisableDottedPathIsSubsetOfExistsTrue: false}));

// Run the query twice to ensure it is cached.
for (let i = 0; i < 2; i++) {
    const res = coll.find(query).toArray();
    // docAffected is incorrectly not returned with the knob disabled.
    assert.sameMembers(res, docsUnaffected);
}

const explainWithKnobDisabled = coll.find(query).explain();
assert(isIxscan(db, explainWithKnobDisabled));
// Validate we are using the more selective partial index on "a.b" as expected.
assert.eq(getWinningPlanFromExplain(explainWithKnobDisabled).inputStage.indexName, "a.b_1");

// Plan is cached with the knob disabled.
assert.eq(explainWithKnobDisabled.queryPlanner.winningPlan.isCached, true);

// Enable the knob.
assert.commandWorked(db.adminCommand(
    {setParameter: 1, internalQueryPlannerDisableDottedPathIsSubsetOfExistsTrue: true}));

let explainWithKnobEnabled = coll.find(query).explain();
// Plan cache key is different for the same query.
assert.neq(getPlanCacheKeyFromExplain(explainWithKnobDisabled),
           getPlanCacheKeyFromExplain(explainWithKnobEnabled));

// Plan for same query is no longer cached.
assert.eq(explainWithKnobEnabled.queryPlanner.winningPlan.isCached, false);
assert(isIxscan(db, explainWithKnobEnabled));
// The knob disables usage of the partial index on "a.b" for this query and thus, we use the less
// selective index on x.
assert.eq(getWinningPlanFromExplain(explainWithKnobEnabled).inputStage.indexName, "x_1");

for (let i = 0; i < 2; i++) {
    const res = coll.find(query).toArray();
    // All docs are correctly returned with the knob enabled.
    assert.sameMembers(res, [docAffected, ...docsUnaffected]);
}

// Explain after running queries twice.
explainWithKnobEnabled = coll.find(query).explain();
// Since there's now only one plan that needs to be multiplanned, we no longer generate a cache
// entry.
assert.eq(explainWithKnobEnabled.queryPlanner.winningPlan.isCached, false);

MongoRunner.stopMongod(conn);

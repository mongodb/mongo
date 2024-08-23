/**
 * Ensure the indexes excluded from API version 1 cannot be used for query planning with
 * "APIStrict: true". Currently, "geoHaystack", "text", "columnstore", and sparse indexes are
 * excluded from API version 1. Note "geoHaystack" index has been deprecated after 4.9.
 *
 * @tags: [
 *   uses_api_parameters,
 *   assumes_read_concern_local,
 *   not_allowed_with_signed_security_token
 * ]
 */

import {getQueryPlanner, getWinningPlan, planHasStage} from "jstests/libs/analyze_plan.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

const testDb = db.getSiblingDB(jsTestName());
const collName = "api_verision_unstable_indexes";
const coll = testDb[collName];
coll.drop();

assert.commandWorked(coll.insert([
    {_id: 1, subject: "coffee", author: "xyz", views: 50},
    {_id: 2, subject: "Coffee Shopping", author: "efg", views: 5},
    {_id: 3, subject: "Baking a cake", author: "abc", views: 90},
    {_id: 4, subject: "baking", author: "xyz", views: 100},
]));

assert.commandWorked(coll.createIndex({subject: "text"}));
assert.commandWorked(coll.createIndex({"views": 1}, {sparse: true}));

// The "text" index, "subject_text", can be used normally.
if (!FixtureHelpers.isMongos(testDb) && !TestData.testingReplicaSetEndpoint) {
    const explainRes = assert.commandWorked(
        testDb.runCommand({explain: {"find": collName, "filter": {$text: {$search: "coffee"}}}}));
    assert.eq(getWinningPlan(getQueryPlanner(explainRes)).indexName, "subject_text", explainRes);
}

// No "text" index can be used for $text search as the "text" index is excluded from API version 1.
assert.commandFailedWithCode(testDb.runCommand({
    explain: {"find": collName, "filter": {$text: {$search: "coffee"}}},
    apiVersion: "1",
    apiStrict: true
}),
                             ErrorCodes.NoQueryExecutionPlans);

// Can not hint a sparse index which is excluded from API version 1 with 'apiStrict: true'.
assert.commandFailedWithCode(testDb.runCommand({
    "find": collName,
    "filter": {views: 50},
    "hint": {views: 1},
    apiVersion: "1",
    apiStrict: true
}),
                             ErrorCodes.BadValue);

if (!FixtureHelpers.isMongos(testDb) && !TestData.testingReplicaSetEndpoint) {
    const explainRes = assert.commandWorked(testDb.runCommand(
        {explain: {"find": collName, "filter": {views: 50}, "hint": {views: 1}}}));
    assert.eq(
        getWinningPlan(getQueryPlanner(explainRes)).inputStage.indexName, "views_1", explainRes);
}
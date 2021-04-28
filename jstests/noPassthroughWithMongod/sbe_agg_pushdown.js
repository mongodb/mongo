// Tests aggregation queries containing a mix of SBE supported and unspported expressions execute
// the pushed down query with SBE.
// TODO: Remove this file when all agg expressions are supported by SBE.

(function() {
"use strict";

load("jstests/libs/sbe_util.js");  // For checkSBEEnabled.

const isSBEEnabled = checkSBEEnabled(db);
if (!isSBEEnabled) {
    jsTestLog("Skipping test because the SBE feature flag is disabled");
    return;
}

const coll = db.jstests_sbe_pushdown;
coll.drop();

// Helper that runs a 'pipeline' with explain mode to determine if SBE was used to execute the
// pushed down query or not. It does this by checking the 'explainVersion' of the plan against the
// 'expectedExplainVersion' input. If the plan has an 'explainVersion' of "2", then SBE was used to
// execute the pushed down query. Otherwise, an 'explainVersion' of "1" means the classic engine was
// used to execute the pushed down query.
function assertPushdownQueryExecMode(pipeline, expectedExplainVersion) {
    assert(expectedExplainVersion === "1" || expectedExplainVersion === "2");
    const actualExplainVersion = coll.explain().aggregate(pipeline).explainVersion;
    assert.eq(actualExplainVersion, expectedExplainVersion);
}

assert.commandWorked(coll.insertMany([
    {_id: 0, a: 1, b: ISODate("2021-04-28T00:00:00Z")},
    {_id: 1, a: 2, b: ISODate("2021-04-28T10:00:00Z")},
    {_id: 2, a: 3, b: ISODate("2021-04-28T20:00:00Z")},
]));

// Test query with no supported expressions is executed with the classic engine.
assertPushdownQueryExecMode([{$project: {_id: 0, c: {$dateToString: {date: "$b"}}}}], "1");

// Test query that contains an expression unsupported by SBE that isn't pushed down. In this case,
// we still expect SBE to be used if the unsupported expression isn't pushed down.
assertPushdownQueryExecMode(
    [
        {$match: {a: 2}},
        {$_internalInhibitOptimization: {}},
        {$project: {_id: 0, c: {$dateToString: {date: "$b"}}}}
    ],
    "2");

// Test query with an unsupported expression in a $project stage that's pushed down executes with
// the classic engine.
assertPushdownQueryExecMode(
    [{$match: {a: 2}}, {$project: {_id: 0, c: {$dateToString: {date: "$b"}}}}], "1");

// Test query with fully supported expressions are executed with SBE when pushed down.
assertPushdownQueryExecMode(
    [{$match: {$expr: {$eq: ["$b", {$dateFromParts: {year: 2021, month: 4, day: 28}}]}}}], "2");
}());

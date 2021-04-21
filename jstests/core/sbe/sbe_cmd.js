// Tests internal 'sbe' command.
// @tags: [
//   assumes_against_mongod_not_mongos,
//   does_not_support_stepdowns,
//   uses_testing_only_commands,
// ]

(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");  // For assertArrayEq.
load("jstests/libs/sbe_util.js");             // For checkSBEEnabled.

const isSBEEnabled = checkSBEEnabled(db);
if (!isSBEEnabled) {
    jsTestLog("Skipping test because the SBE feature flag is disabled");
    return;
}

const coll = db.jstests_sbe_cmd;
coll.drop();

// Helper that executes a given 'query', gets the generated 'slotBasedPlan' from explain output,
// and runs that SBE plan through the internal 'sbe' command which executes the plan string
// directly to verify that the command works.
function assertSbeCommandWorked({query, projection = {}} = {}) {
    const queryResult = coll.find(query, projection);
    const expl = queryResult.explain();

    assert(expl.queryPlanner.winningPlan.hasOwnProperty("slotBasedPlan"));
    const slotBasedPlan = expl.queryPlanner.winningPlan.slotBasedPlan.stages;

    // Verify that the sbe command works and that the SBE plan string is parsed successfully.
    assert(db._sbe(slotBasedPlan));
}

assert.commandWorked(coll.insertMany([
    {_id: 0, a: 1, b: 1, c: 1},
    {_id: 1, a: 1, b: 1, c: 2},
    {_id: 2, a: 1, b: 1, c: 3},
    {_id: 3, a: 1, b: 2, c: 3}
]));

// Test query: {}.
assertSbeCommandWorked({query: {}});
// Test query {b: 1}.
assertSbeCommandWorked({query: {b: 1}});
// Test query: {a: 1, c: 3}.
assertSbeCommandWorked({query: {a: 1, c: 3}});
// Test query: {a: 1, c: 3} with projection {_id: 0}.
assertSbeCommandWorked({query: {a: 1, c: 3}, projection: {_id: 0}});
}());

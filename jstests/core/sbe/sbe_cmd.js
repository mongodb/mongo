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

// The sbe command requires lock-free reads, so determine if they're enabled before proceeding:
// (1) ephemeralForTest automatically uses enableMajorityReadConcern=false, which disables lock-free
// reads.
// (2) lock-free reads are only supported in server versions 4.9+.
const maxWireVersion = assert.commandWorked(db.runCommand({isMaster: 1})).maxWireVersion;
const isLockFreeReadsEnabled = jsTest.options().storageEngine !== "ephemeralForTest" &&
    maxWireVersion >= 12 /* WIRE_VERSION_49 */;
if (!isLockFreeReadsEnabled) {
    jsTestLog("Skipping test because lock-free reads are not enabled.");
    return;
}

const coll = db.jstests_sbe_cmd;
coll.drop();
const basicFind = coll.find().explain();
if (!basicFind.queryPlanner.winningPlan.hasOwnProperty("slotBasedPlan")) {
    jsTestLog("Skipping test because the SBE feature flag is disabled");
    return;
}

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

// Verify that the sbe command can detect if a collection has been dropped.
const explain = coll.find().explain();
assert(explain.queryPlanner.winningPlan.hasOwnProperty("slotBasedPlan"), explain);
const slotBasedPlan = explain.queryPlanner.winningPlan.slotBasedPlan.stages;

// The command response should be OK as long as the collection exists.
assert(db._sbe(slotBasedPlan));

// After the collection is dropped, the parser should detect that the collection doesn't exist.
assert(coll.drop());
assert.throwsWithCode(() => db._sbe(slotBasedPlan), 6056700);
}());

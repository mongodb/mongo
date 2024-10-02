// Tests that profiled $lookups contain the correct namespace and that Top is updated accordingly.
// @tags: [
//  does_not_support_stepdowns,
//  requires_profiling,
//  not_allowed_with_signed_security_token,
// ]

import {getAggPlanStages} from "jstests/libs/query/analyze_plan.js";

const localColl = db.local;
const foreignColl = db.foreign;
localColl.drop();
foreignColl.drop();

assert.commandWorked(localColl.insert([{a: 1}, {b: 1}, {a: 2}]));
assert.commandWorked(foreignColl.insert({a: 1}));

db.setProfilingLevel(0);
db.system.profile.drop();
// Don't profile the setFCV command, which could be run during this test in the
// fcv_upgrade_downgrade_replica_sets_jscore_passthrough suite.
assert.commandWorked(db.setProfilingLevel(
    1, {filter: {'command.setFeatureCompatibilityVersion': {'$exists': false}}}));

let oldTop = db.adminCommand("top");
const pipeline =
    [{$lookup: {from: foreignColl.getName(), as: "res", localField: "a", foreignField: "a"}}];
localColl.aggregate(pipeline);

db.setProfilingLevel(0);

// Confirm that namespace is the local rather than foreign collection.
let profileDoc = db.system.profile.findOne();
assert.eq("test.local", profileDoc.ns);

// Confirm that the local collection had one command added to Top.
let newTop = db.adminCommand("top");
assert.eq(1,
          newTop.totals[localColl.getFullName()].commands.count -
              oldTop.totals[localColl.getFullName()].commands.count);

const actualCount = newTop.totals[foreignColl.getFullName()].commands.count -
    oldTop.totals[foreignColl.getFullName()].commands.count;

// Compute the expected count as follows:
// 1) We expect the count to be at least one. This is because we will take a lock over 'foreignColl'
// and, even if we don't push down $lookup into SBE, this will still increment the top counter for
// 'foreignColl' by one.
// 2) If $lookup is NOT pushed down into SBE, then we increment the count by three. This is because
// when executing $lookup in the classic engine, we will add one entry to top for the foreign
// collection for each document in the local collection (of which there are three).
let expectedCount = 1;
const eqLookupNodes = getAggPlanStages(localColl.explain().aggregate(pipeline), "EQ_LOOKUP");
if (eqLookupNodes.length === 0) {
    expectedCount += 3;
}
assert.eq(expectedCount, actualCount);

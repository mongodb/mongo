// Tests whether pipeline stages that are pushed down to SBE can report the 'replanReason' properly
// under profiling.
//
// @tags: [
//   # Wrapping the pipeline in a $facet will prevent its $match from getting absorbed by the query
//   # system, which prevents $match from being pushed down to SBE.
//   do_not_wrap_aggregations_in_facets,
//   does_not_support_stepdowns,
//   requires_profiling,
//   not_allowed_with_signed_security_token,
// ]

import {getLatestProfilerEntry} from "jstests/libs/profiler.js";

const testDB = db.getSiblingDB(jsTestName());
assert.commandWorked(testDB.dropDatabase());

const lcoll = testDB.left;
const fcoll = testDB.right;

// Creates multiple indexes so that multiplanning can happen.
assert.commandWorked(lcoll.createIndex({a: 1}));
assert.commandWorked(lcoll.createIndex({b: 1}));
// Inserts enough data so that different $match stage may trigger replanning.
for (let i = 0; i < 20; ++i) {
    assert.commandWorked(lcoll.insert({a: 5, b: i}));
    assert.commandWorked(lcoll.insert({a: i, b: 10}));
}

for (let i = 0; i < 20; ++i) {
    assert.commandWorked(fcoll.insert({_id: i, c: i}));
}

function testPushedDownSBEPlanReplanning(match1, match2, pushedDownStage) {
    // Runs this pipeline twice so that the pushed down plan can be cached. The 'match1' and
    // 'pushedDownStage' are pushed down to SBE together.
    const pushedDownPipeline = [match1, pushedDownStage];
    lcoll.aggregate(pushedDownPipeline).itcount();
    lcoll.aggregate(pushedDownPipeline).itcount();

    // Don't profile the setFCV command, which could be run during this test in the
    // fcv_upgrade_downgrade_replica_sets_jscore_passthrough suite.
    assert.commandWorked(testDB.setProfilingLevel(
        1, {filter: {'command.setFeatureCompatibilityVersion': {'$exists': false}}}));
    // The cached plan is not efficient for the 'match2' stage and replanning will happen.
    const pipelineTriggeringReplanning = [match2, pushedDownStage];
    lcoll.aggregate(pipelineTriggeringReplanning).itcount();
    assert.commandWorked(testDB.setProfilingLevel(0));

    const replanReasonRegex = /cached plan was less efficient than expected:/;
    const profileEntryFilter =
        {op: "command", ns: lcoll.getFullName(), replanned: true, replanReason: replanReasonRegex};

    const profileObj = getLatestProfilerEntry(testDB, profileEntryFilter);
    assert(profileObj);
    assert(profileObj.replanned, profileObj);
    assert(profileObj.replanReason.match(replanReasonRegex), profileObj);

    assert(testDB.system.profile.drop());
}

(function testPushedDownLookupReplanning() {
    testPushedDownSBEPlanReplanning({$match: {a: 5, b: 15}}, {$match: {a: 15, b: 10}}, {
        $lookup: {from: fcoll.getName(), as: "as", localField: "a", foreignField: "c"}
    });
})();

(function testPushedDownGroupReplanning() {
    testPushedDownSBEPlanReplanning(
        {$match: {a: 5, b: 15}}, {$match: {a: 15, b: 10}}, {$group: {_id: "$a"}});
})();

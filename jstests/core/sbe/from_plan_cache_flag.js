// @tags: [
//   # The test runs commands that are not allowed with security token: setProfilingLevel.
//   not_allowed_with_signed_security_token,
//   requires_profiling,
//   does_not_support_stepdowns,
//   # The SBE plan cache was first enabled in 6.3.
//   requires_fcv_63,
//   featureFlagSbeFull,
// ]
import {runWithFastPathsDisabled} from "jstests/libs/optimizer_utils.js";
import {getLatestProfilerEntry} from "jstests/libs/profiler.js";

// SERVER-85146: Disable CQF fast path for queries testing the plan cache. Fast path queries do not
// get cached and may break tests that expect cache entries for fast path-eligible queries.
runWithFastPathsDisabled(() => {
    const testDB = db.getSiblingDB("from_plan_cache_flag");
    assert.commandWorked(testDB.dropDatabase());
    const collName = jsTestName();
    const coll = testDB.getCollection(collName);
    // Don't profile the setFCV command, which could be run during this test in the
    // fcv_upgrade_downgrade_replica_sets_jscore_passthrough suite.
    assert.commandWorked(testDB.setProfilingLevel(
        1, {filter: {'command.setFeatureCompatibilityVersion': {'$exists': false}}}));
    coll.drop();
    coll.getPlanCache().clear();

    assert.commandWorked(coll.insert({a: 1}));
    assert.commandWorked(coll.insert({a: 2}));
    assert.commandWorked(coll.insert({a: 3}));

    const comment = "from_plan_cache_flag";
    coll.aggregate([{$match: {a: 1}}], {comment}).toArray();
    let profileObj = getLatestProfilerEntry(testDB, {"command.comment": comment});
    assert.eq(!!profileObj.fromPlanCache, false, profileObj);

    coll.aggregate([{$match: {a: 2}}], {comment}).toArray();
    profileObj = getLatestProfilerEntry(testDB, {"command.comment": comment});
    assert.eq(!!profileObj.fromPlanCache, true, profileObj);

    coll.aggregate([{$match: {a: 3}}], {comment}).toArray();
    profileObj = getLatestProfilerEntry(testDB, {"command.comment": comment});
    assert.eq(!!profileObj.fromPlanCache, true, profileObj);
});

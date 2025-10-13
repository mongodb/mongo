// @tags: [
//   # The test runs commands that are not allowed with security token: setProfilingLevel.
//   not_allowed_with_signed_security_token,
//   requires_profiling,
//   does_not_support_stepdowns,
//   # The SBE plan cache was first enabled in 6.3.
//   requires_fcv_63,
//   featureFlagSbeFull,
// ]
import {getLatestProfilerEntry} from "jstests/libs/profiler.js";

const testDB = db.getSiblingDB("from_plan_cache_flag");
assert.commandWorked(testDB.dropDatabase());
const collName = jsTestName();
const coll = testDB.getCollection(collName);
// Don't profile the setFCV command, which could be run during this test in the
// fcv_upgrade_downgrade_replica_sets_jscore_passthrough suite.
assert.commandWorked(
    testDB.setProfilingLevel(1, {filter: {"command.setFeatureCompatibilityVersion": {"$exists": false}}}),
);
coll.drop();
coll.getPlanCache().clear();

assert.commandWorked(coll.insert({a: 1}));
assert.commandWorked(coll.insert({a: 2}));
assert.commandWorked(coll.insert({a: 3}));

const comment = "from_plan_cache_flag";
coll.aggregate([{$match: {a: 1}}], {comment}).toArray();
let profileObj = getLatestProfilerEntry(testDB, {"command.comment": comment});
assert.eq(!!profileObj.fromPlanCache, false, profileObj);

// Run the query two more times, for different values of 'a'. These runs should use the cached query plan.
[2, 3].forEach((a) => {
    // Using 'assert.soon()' here to skip over transient situations in which a query
    // plan cannot be added to the plan cache.
    assert.soon(() => {
        coll.aggregate([{$match: {a}}], {comment}).toArray();
        profileObj = getLatestProfilerEntry(testDB, {"command.comment": comment});
        return !!profileObj.fromPlanCache;
    });
    assert.eq(!!profileObj.fromPlanCache, true, () => {
        const planCacheEntries = coll.getPlanCache().list();
        return (
            `Query not served from plan cache.\nProfile: ${tojson(profileObj)}\n` +
            `Plan cache: ${tojson(planCacheEntries)}`
        );
    });
});

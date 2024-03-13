// Test that, when running a trial of a cached plan that has blocking stages, the planner does not
// invalidate the plan (and discard its results) at the end of the trial unless replanning is
// needed.
//
// @tags: [
//   # The test runs commands that are not allowed with security token: setProfilingLevel.
//   not_allowed_with_signed_security_token,
//   # This test attempts to perform queries and introspect the server's plan cache entries. The
//   # former operation may be routed to a secondary in the replica set, whereas the latter must be
//   # routed to the primary.
//   assumes_read_concern_unchanged,
//   assumes_read_preference_unchanged,
//   assumes_unsharded_collection,
//   does_not_support_stepdowns,
//   # The SBE plan cache was first enabled in 6.3.
//   requires_fcv_63,
//   requires_profiling,
//   # Plan cache state is node-local and will not get migrated alongside user data.
//   tenant_migration_incompatible,
//   assumes_balancer_off,
//   # TODO SERVER-85728: Enable Bonsai plan cache tests involving indices.
//   cqf_experimental_incompatible,
//   featureFlagSbeFull,
// ]
import {getLatestProfilerEntry} from "jstests/libs/profiler.js";

const testDb = db.getSiblingDB(jsTestName());
assert.commandWorked(testDb.dropDatabase());

const coll = testDb.getCollection('test');

const queryPlanEvaluationMaxResults = (() => {
    const getParamRes = assert.commandWorked(
        testDb.adminCommand({getParameter: 1, internalQueryPlanEvaluationMaxResults: 1}));
    return getParamRes["internalQueryPlanEvaluationMaxResults"];
})();

const queryCacheEvictionRatio = (() => {
    const getParamRes = assert.commandWorked(
        testDb.adminCommand({getParameter: 1, internalQueryCacheEvictionRatio: 1}));
    return getParamRes["internalQueryCacheEvictionRatio"];
})();

assert.commandWorked(coll.createIndex({a: 1}));
assert.commandWorked(coll.createIndex({b: 1, d: 1}));

// Add enough documents to the collection to ensure that the test query will always run through its
// "trial period" when using the cached plan.
const numMatchingDocs = 2 * queryPlanEvaluationMaxResults;
let bulk = coll.initializeUnorderedBulkOp();
for (let i = 0; i < 100; i++) {
    // Add documents that will not match the test query but will favor the {a: 1} index.
    bulk.insert({a: 0, b: 1, c: i, d: i % 2});
}
for (let i = 100; i < 100 + numMatchingDocs; i++) {
    // Add documents that will match the test query.
    bulk.insert({a: 1, b: 1, c: i, d: i % 2});
}
assert.commandWorked(bulk.execute());

// We enable profiling and run the test query three times. The first two times, it will go through
// multiplanning.
function runTestQuery(comment) {
    return coll.find({a: 1, b: 1})
        .sort({c: 1})
        .batchSize(numMatchingDocs + 1)
        .comment(comment)
        .itcount();
}

// Don't profile the setFCV command, which could be run during this test in the
// fcv_upgrade_downgrade_replica_sets_jscore_passthrough suite.
assert.commandWorked(testDb.setProfilingLevel(
    1, {filter: {'command.setFeatureCompatibilityVersion': {'$exists': false}}}));

let lastComment;
for (let i = 0; i < 3; i++) {
    lastComment = `test query: ${i}`;
    const numResults = runTestQuery(lastComment);
    assert.eq(numResults, numMatchingDocs);
}

// Get the profile entry for the third execution, which should have bypassed the multiplanner and
// used a cached plan.
const profileEntry = getLatestProfilerEntry(
    testDb, {'command.find': coll.getName(), 'command.comment': lastComment});
assert(!profileEntry.fromMultiPlanner, profileEntry);
assert('planCacheKey' in profileEntry, profileEntry);

// We expect the cached plan to run through its "trial period," but the planner should determine
// that the cached plan is still good and does _not_ need replanning. Previously, the planner would
// still need to close the execution tree in this scenario, discarding all the work it had already
// done. This test ensures that behavior is corrected: the execution tree should only need to be
// opened 1 time.
assert.eq(profileEntry.execStats.opens, 1, profileEntry);

const planCacheEntry = (() => {
    const planCache =
        coll.getPlanCache().list([{$match: {planCacheKey: profileEntry.planCacheKey}}]);
    assert.eq(planCache.length, 1, planCache);
    return planCache[0];
})();

// Modify the test data so that it will force a replan. We remove all the documents that will match
// the test query and add non-matching documents that will get examined by the index scan (for
// either index). The planner's criterion for when a cached index scan has done too much work (and
// should be replanned) is based on the "works" value in the plan cache entry and the
// "internalQueryCacheEvictionRatio" server parameter, so we use those values to determine how many
// documents to add.
//
// This portion of the test validates that replanning still works as desired even after the query
// planner changes to allow "trial periods" that do not discard results when replanning is not
// necessary.

assert.commandWorked(coll.remove({a: 1, b: 1}));
bulk = coll.initializeUnorderedBulkOp();
for (let i = 0; i < queryCacheEvictionRatio * planCacheEntry.works + 1; i++) {
    bulk.insert({a: 1, b: 0, c: i});
    bulk.insert({a: 0, b: 1, c: i});
}
assert.commandWorked(bulk.execute());

// Run the query one last time, and get its profile entry to enure it triggered replanning.
lastComment = "test query expected to trigger replanning";
const numResults = runTestQuery(lastComment);
assert.eq(numResults, 0);

const replanProfileEntry = getLatestProfilerEntry(
    testDb, {'command.find': coll.getName(), 'command.comment': lastComment});
assert(replanProfileEntry.replanned, replanProfileEntry);

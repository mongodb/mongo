/**
 * Tests for the $$NOW and $$CLUSTER_TIME system variable.
 */
(function() {
"use strict";

load("jstests/libs/sbe_util.js");                   // For checkSBEEnabled.
load("jstests/libs/sbe_assert_error_override.js");  // Override error-code-checking APIs.

const coll = db[jsTest.name()];
const otherColl = db[coll.getName() + "_other"];
otherColl.drop();
coll.drop();
db["viewWithNow"].drop();
db["viewWithClusterTime"].drop();

// Insert simple documents into the main test collection. Aggregation and view pipelines will
// augment these docs with time-based fields.
const numdocs = 1000;
let bulk = coll.initializeUnorderedBulkOp();
for (let i = 0; i < numdocs; ++i) {
    bulk.insert({_id: i});
}
assert.commandWorked(bulk.execute());

// Insert into another collection with pre-made fields for testing the find() command.
bulk = otherColl.initializeUnorderedBulkOp();
const timeFieldValue = new Date();
for (let i = 0; i < numdocs; ++i) {
    bulk.insert({_id: i, timeField: timeFieldValue, clusterTimeField: new Timestamp(0, 1)});
}
assert.commandWorked(bulk.execute());

assert.commandWorked(
    db.createView("viewWithNow", coll.getName(), [{$addFields: {timeField: "$$NOW"}}]));
const viewWithNow = db["viewWithNow"];

assert.commandWorked(db.createView(
    "viewWithClusterTime", coll.getName(), [{$addFields: {timeField: "$$CLUSTER_TIME"}}]));
const viewWithClusterTime = db["viewWithClusterTime"];

function runTests(query) {
    const runAndCompare = function() {
        const results = query().toArray();
        assert.eq(results.length, numdocs);

        // Make sure the values are the same for all documents
        for (let i = 0; i < numdocs; ++i) {
            assert.eq(results[0].timeField, results[i].timeField);
        }
        return results;
    };

    const results = runAndCompare();

    // Sleep for a while and then rerun.
    sleep(3000);

    const resultsLater = runAndCompare();

    // Later results should be later in time.
    assert.lt(results[0].timeField, resultsLater[0].timeField);

    // Sleep for a while and then run for the third time.
    //
    // Test when the query is cached (it can take two executions of a query for a plan to get
    // cached).
    sleep(3000);

    const resultsLast = runAndCompare();

    // 'resultsLast' should be later in time than 'resultsLater'.
    assert.lt(resultsLater[0].timeField, resultsLast[0].timeField);
}

function runTestsExpectFailure(query) {
    const results = query();
    // Expect to see "Builtin variable '$$CLUSTER_TIME' is not available" error.
    assert.commandFailedWithCode(results, 51144);
}

function baseCollectionNowFind() {
    return otherColl.find({$expr: {$lt: ["$timeField", "$$NOW"]}});
}

function baseCollectionClusterTimeFind() {
    return db.runCommand({
        find: otherColl.getName(),
        filter: {$expr: {$lt: ["$clusterTimeField", "$$CLUSTER_TIME"]}}
    });
}

function baseCollectionNowAgg() {
    return coll.aggregate([{$addFields: {timeField: "$$NOW"}}]);
}

function baseCollectionClusterTimeAgg() {
    return db.runCommand({
        aggregate: coll.getName(),
        pipeline: [{$addFields: {timeField: "$$CLUSTER_TIME"}}],
        cursor: {}
    });
}

function fromViewWithNow() {
    return viewWithNow.find();
}

function fromViewWithClusterTime() {
    return db.runCommand({find: viewWithClusterTime.getName()});
}

function withExprNow() {
    return viewWithNow.find({$expr: {$eq: ["$timeField", "$$NOW"]}});
}

function projWithNow() {
    return coll.find({}, {timeField: "$$NOW"});
}

function aggWithNow() {
    return coll.aggregate([{$project: {timeField: "$$NOW"}}]);
}

function aggWithNowNotPushedDown() {
    return coll.aggregate([{$_internalInhibitOptimization: {}}, {$project: {timeField: "$$NOW"}}]);
}

function withExprClusterTime() {
    return db.runCommand({
        find: viewWithClusterTime.getName(),
        filter: {$expr: {$eq: ["$timeField", "$$CLUSTER_TIME"]}}
    });
}

// Test that $$NOW is usable in all contexts.
//
// $$NOW used at insertion time, it is expected values are not changed.
assert.eq(baseCollectionNowFind().toArray().length, numdocs);
// $$NOW used in the query, it is expected to update its time value for different runs.
runTests(fromViewWithNow);
runTests(withExprNow);
runTests(baseCollectionNowAgg);
runTests(projWithNow);
runTests(aggWithNow);
runTests(aggWithNowNotPushedDown);

// Test that $$NOW can be used in explain for both find and aggregate.
assert.commandWorked(coll.explain().find({$expr: {$lte: ["$timeField", "$$NOW"]}}).finish());
assert.commandWorked(viewWithNow.explain().find({$expr: {$eq: ["$timeField", "$$NOW"]}}).finish());
assert.commandWorked(coll.explain().aggregate([{$addFields: {timeField: "$$NOW"}}]));

// $$CLUSTER_TIME is not available on a standalone mongod.
runTestsExpectFailure(baseCollectionClusterTimeFind);
runTestsExpectFailure(baseCollectionClusterTimeAgg);
runTestsExpectFailure(fromViewWithClusterTime);
runTestsExpectFailure(withExprClusterTime);

if (checkSBEEnabled(db, ["featureFlagSbeFull"])) {
    function verifyPlanCacheSize(query) {
        coll.getPlanCache().clear();

        query().toArray();
        // It can take two executions of a query for a plan to get cached.
        query().toArray();

        const caches = coll.getPlanCache().list();
        assert.eq(caches.length, 1, caches);
        assert.eq(caches[0].cachedPlan.stages.includes("Date"), false, caches);
    }

    // Query with $$NOW will be cached.
    verifyPlanCacheSize(projWithNow);
    verifyPlanCacheSize(aggWithNow);
    // $$NOW is not in SBE query.
    verifyPlanCacheSize(fromViewWithNow);
    verifyPlanCacheSize(withExprNow);
    // $$NOW could not be pushed down into SBE.
    verifyPlanCacheSize(baseCollectionNowAgg);
    verifyPlanCacheSize(aggWithNowNotPushedDown);
}

// Insert an doc with a future time.
const futureColl = db[coll.getName() + "_future"];
futureColl.drop();
const time = new Date();
time.setSeconds(time.getSeconds() + 3);
assert.commandWorked(futureColl.insert({timeField: time}));

// The 'timeField' value is later than '$$NOW' in '$expr'.
assert.eq(0, futureColl.find({$expr: {$lt: ["$timeField", "$$NOW"]}}).itcount());
// The '$$NOW' in '$expr' should advance its value after sleeping for 3 second, the 'timeField'
// value should be earlier than it now.
assert.soon(() => {
    return futureColl.find({$expr: {$lt: ["$timeField", "$$NOW"]}}).itcount() == 1;
}, "$$NOW should catch up after 3 seconds");
}());

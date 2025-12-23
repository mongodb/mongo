/**
 * Tests that we are not able to write to a collection during an active primary driven index build.
 * @tags: [
 *   requires_replication,
 * ]
 * TODO SERVER-111867: Remove this test once primary-driven index builds support side writes.
 */
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {IndexBuildTest} from "jstests/noPassthrough/libs/index_builds/index_build.js";
import {isIxscan, getWinningPlanFromExplain} from "jstests/libs/query/analyze_plan.js";

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const dbName = "test";
const collName = jsTestName();
const db = primary.getDB(dbName);
const coll = db.getCollection(collName);

if (!FeatureFlagUtil.isPresentAndEnabled(primary, "PrimaryDrivenIndexBuilds")) {
    jsTestLog("Skipping writes_during_primary_driven_index.js because featureFlagPrimaryDrivenIndexBuilds is disabled");
    MongoRunner.stopMongod(primary);
    quit();
}

assert.commandWorked(coll.insert({a: 1}));

// Prevent the index build from completing.
IndexBuildTest.pauseIndexBuilds(primary);

// Start the index build and wait for it to start.
const awaitIndex = IndexBuildTest.startIndexBuild(primary, coll.getFullName(), {a: 1});
IndexBuildTest.waitForIndexBuildToStart(db, collName, "a_1");

// Perform writes that are not allowed while the index build is in progress.
assert.commandFailedWithCode(coll.update({a: 1}, {a: -1}), ErrorCodes.ConflictingOperationInProgress);
assert.commandFailedWithCode(coll.remove({a: 1}), ErrorCodes.ConflictingOperationInProgress);
assert.commandFailedWithCode(
    assert.throws(function () {
        coll.findAndModify({query: {a: 1}, update: {$set: {a: 2}}});
    }),
    ErrorCodes.ConflictingOperationInProgress,
);

// Bulk writes partially succeeds for writes that are allowed and errors for disallowed writes.
const result = assert.throws(function () {
    coll.bulkWrite([
        {insertOne: {document: {a: 2}}},
        {updateOne: {filter: {a: 1}, update: {$set: {a: -1}}}},
        {deleteOne: {filter: {a: 1}}},
    ]);
});
assert.commandFailedWithCode(result, ErrorCodes.ConflictingOperationInProgress);
assert.eq(result.nInserted, 1);

// Perform writes that are allowed while index is building.
assert.commandWorked(coll.insert({a: 3}));

// Verify write to the collection.
assert.eq(coll.find({a: 3}).count(), 1);

// Allow the index build to complete.
IndexBuildTest.resumeIndexBuilds(primary);
awaitIndex();

// Verify that query for successful concurrent writes uses the index.
assert(isIxscan(db, getWinningPlanFromExplain(coll.find({a: 2}).explain())));

rst.stopSet();

/**
 * Tests index usage with durable history across restarts.
 *
 * @tags: [
 *     requires_persistence,
 *     requires_replication,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/noPassthrough/libs/index_build.js");
load("jstests/libs/feature_flag_util.js");

const replTest = new ReplSetTest({
    nodes: 1,
    nodeOptions: {
        setParameter: {
            // To control durable history more predictably, disable the checkpoint thread.
            syncdelay: 0
        }
    }
});
replTest.startSet();
replTest.initiate();

const indexSpec = {
    a: 1
};

const primary = function() {
    return replTest.getPrimary();
};

const testDB = function() {
    return primary().getDB("test");
};

const coll = function() {
    return testDB()[jsTestName()];
};

const insert = function(document) {
    // The write concern guarantees the stable and oldest timestamp are bumped.
    return assert
        .commandWorked(testDB().runCommand(
            {insert: coll().getName(), documents: [document], writeConcern: {w: "majority"}}))
        .operationTime;
};

const findWithIndex = function(atClusterTime, expectedErrCode) {
    let res = {};
    if (atClusterTime == undefined) {
        res = testDB().runCommand({find: jsTestName(), hint: indexSpec});
    } else {
        res = testDB().runCommand({
            find: jsTestName(),
            hint: indexSpec,
            readConcern: {level: "snapshot", atClusterTime: atClusterTime}
        });
    }

    if (expectedErrCode) {
        assert.commandFailedWithCode(res, expectedErrCode);
    } else {
        return assert.commandWorked(res);
    }
};

const oldestTS = insert({a: 0});
jsTestLog("Oldest timestamp: " + tojson(oldestTS));

// The index does not exist yet.
findWithIndex(undefined, ErrorCodes.BadValue);
findWithIndex(oldestTS, ErrorCodes.BadValue);

const createIndexTS = assert.commandWorked(coll().createIndex(indexSpec)).operationTime;
jsTestLog("Initial index creation timestamp: " + tojson(createIndexTS));

// The index is only available for use as of the 'createIndexTS'.
findWithIndex(oldestTS, ErrorCodes.BadValue);
assert.eq(1, findWithIndex(createIndexTS)["cursor"]["firstBatch"].length);
assert.eq(1, findWithIndex(undefined)["cursor"]["firstBatch"].length);

// Rebuild the index without finishing it.
assert.commandWorked(coll().dropIndex(indexSpec));
const fp = configureFailPoint(primary(), "hangAfterSettingUpIndexBuildUnlocked");
const awaitCreateIndex = IndexBuildTest.startIndexBuild(primary(), coll().getFullName(), indexSpec);
fp.wait();

// Get a timestamp after starting the index build but before the commit timestamp of the index
// build.
const preIndexCommitTS = insert({a: 1});

assert.commandWorked(testDB().adminCommand({fsync: 1}));

replTest.stop(0, 9, {allowedExitCode: MongoRunner.EXIT_SIGKILL}, {forRestart: true});
awaitCreateIndex({checkExitSuccess: false});

replTest.start(
    0,
    {
        setParameter: {
            "failpoint.hangAfterSettingUpIndexBuildUnlocked": tojson({mode: "alwaysOn"}),
            // To control durable history more predictably, disable the checkpoint thread.
            syncdelay: 0
        }
    },
    true /* restart */);

const checkLogs = function() {
    // Found index from unfinished build.
    checkLog.containsJson(primary(), 22253, {
        index: "a_1",
        namespace: coll().getFullName(),
    });

    // Resetting unfinished index.
    checkLog.containsJson(primary(), 6987700, {namespace: coll().getFullName(), index: "a_1"});

    // Index build restarting.
    checkLog.containsJson(primary(), 20660);
};

checkLogs();

// The index is being re-created.

// It's possible to read prior to the most recent DDL operation for the collection.
//
// At oldestTs, the index did not exist, so queries for the index at that timestamp will return
// BadValue.
//
// At createIndexTS, the index did exist, so queries for the index at that timestamp will be
// successful.
//
// At preIndexCommitTS, the new index has not finished being created yet, so queries for the index
// at that timestamp will return BadValue.
//
// Etc.
//
// Find queries should all return the result one would expect based on the state of the catalog at
// that point in time. When the feature flag is disabled, these find queries will instead return
// SnapshotUnavailable.

findWithIndex(oldestTS, ErrorCodes.BadValue);
findWithIndex(createIndexTS, null);
findWithIndex(preIndexCommitTS, ErrorCodes.BadValue);
findWithIndex(undefined, ErrorCodes.BadValue);

const restartInsertTS = insert({a: 2});

// Take a checkpoint to persist the new catalog entry of the index being rebuilt.
assert.commandWorked(testDB().adminCommand({fsync: 1}));

replTest.stop(0, 9, {allowedExitCode: MongoRunner.EXIT_SIGKILL}, {forRestart: true});
replTest.start(
    0,
    {
        setParameter: {
            // To control durable history more predictably, disable the checkpoint thread.
            syncdelay: 0
        }
    },
    true /* restart */);

checkLogs();

// Startup recovery will rebuild the index in the background. Wait for the index build to finish.
checkLog.containsJson(primary(), 20663, {
    namespace: coll().getFullName(),
    indexesBuilt: ["a_1"],
    numIndexesAfter: 2,
});
IndexBuildTest.assertIndexes(coll(), 2, ["_id_", "a_1"]);

findWithIndex(oldestTS, ErrorCodes.BadValue);
findWithIndex(createIndexTS, null);
findWithIndex(preIndexCommitTS, ErrorCodes.BadValue);
findWithIndex(restartInsertTS, ErrorCodes.BadValue);

assert.eq(3, findWithIndex(undefined)["cursor"]["firstBatch"].length);

const insertAfterIndexBuildTS = insert({a: 3});
assert.eq(4, findWithIndex(insertAfterIndexBuildTS)["cursor"]["firstBatch"].length);
assert.eq(4, findWithIndex(undefined)["cursor"]["firstBatch"].length);

// Demonstrate that durable history can be used across a restart with a finished index.
assert.commandWorked(testDB().adminCommand({fsync: 1}));
replTest.restart(primary());

assert.eq(4, findWithIndex(undefined)["cursor"]["firstBatch"].length);
const insertAfterRestartAfterIndexBuild = insert({a: 4});
assert.eq(5, findWithIndex(insertAfterRestartAfterIndexBuild)["cursor"]["firstBatch"].length);
assert.eq(5, findWithIndex(undefined)["cursor"]["firstBatch"].length);

findWithIndex(oldestTS, ErrorCodes.BadValue);
findWithIndex(createIndexTS, null);
findWithIndex(preIndexCommitTS, ErrorCodes.BadValue);
findWithIndex(restartInsertTS, ErrorCodes.BadValue);

assert.eq(4, findWithIndex(insertAfterIndexBuildTS)["cursor"]["firstBatch"].length);

// Drop the index and demonstrate the durable history can be used across a restart for reads with
// times prior to the drop.
const dropIndexTS = assert.commandWorked(coll().dropIndex(indexSpec)).operationTime;
jsTestLog("Index drop timestamp: " + tojson(dropIndexTS));

// Take a checkpoint to persist the new catalog entry of the index being rebuilt.
assert.commandWorked(testDB().adminCommand({fsync: 1}));

replTest.stop(0, 9, {allowedExitCode: MongoRunner.EXIT_SIGKILL}, {forRestart: true});
replTest.start(
    0,
    {
        setParameter: {
            // To control durable history more predictably, disable the checkpoint thread.
            syncdelay: 0
        }
    },
    true /* restart */);

// Test that we can read using the dropped index on timestamps before the drop
assert.eq(4, findWithIndex(insertAfterIndexBuildTS)["cursor"]["firstBatch"].length);
assert.eq(5, findWithIndex(insertAfterRestartAfterIndexBuild)["cursor"]["firstBatch"].length);

replTest.stopSet();
})();

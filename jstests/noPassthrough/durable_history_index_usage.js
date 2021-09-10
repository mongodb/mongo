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

const findWithIndex = function(atClusterTime, shouldSucceed) {
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

    if (shouldSucceed) {
        return assert.commandWorked(res);
    }

    assert.commandFailedWithCode(res, [ErrorCodes.BadValue, ErrorCodes.SnapshotUnavailable]);
};

const oldestTS = insert({a: 0});
jsTestLog("Oldest timestamp: " + tojson(oldestTS));

// The index does not exist yet.
findWithIndex(undefined, /*shouldSucceed=*/false);
findWithIndex(oldestTS, /*shouldSucceed=*/false);

const createIndexTS = assert.commandWorked(coll().createIndex(indexSpec)).operationTime;
jsTestLog("Initial index creation timestamp: " + tojson(createIndexTS));

// The index is only available for use as of the 'createIndexTS'.
findWithIndex(oldestTS, /*shouldSucceed=*/false);
assert.eq(1, findWithIndex(createIndexTS, /*shouldSucceed=*/true)["cursor"]["firstBatch"].length);
assert.eq(1, findWithIndex(undefined, /*shouldSucceed=*/true)["cursor"]["firstBatch"].length);

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
    // The index build was not yet completed at the recovery timestamp, it will be dropped and
    // rebuilt.
    checkLog.containsJson(primary(), 22206, {
        index: "a_1",
        namespace: coll().getFullName(),
        commitTimestamp: {$timestamp: {t: 0, i: 0}},
    });

    // Index build restarting.
    checkLog.containsJson(primary(), 20660);
};

checkLogs();

// The index is being re-created.
findWithIndex(oldestTS, /*shouldSucceed=*/false);
findWithIndex(createIndexTS, /*shouldSucceed=*/false);
findWithIndex(preIndexCommitTS, /*shouldSucceed=*/false);
findWithIndex(undefined, /*shouldSucceed=*/false);

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

// We can't use the index at earlier times than the index builds commit timestamp.
findWithIndex(oldestTS, /*shouldSucceed=*/false);
findWithIndex(createIndexTS, /*shouldSucceed=*/false);
findWithIndex(preIndexCommitTS, /*shouldSucceed=*/false);
findWithIndex(restartInsertTS, /*shouldSucceed=*/false);

assert.eq(3, findWithIndex(undefined, /*shouldSucceed=*/true)["cursor"]["firstBatch"].length);

const finalInsertTS = insert({a: 3});
assert.eq(4, findWithIndex(finalInsertTS, /*shouldSucceed=*/true)["cursor"]["firstBatch"].length);
assert.eq(4, findWithIndex(undefined, /*shouldSucceed=*/true)["cursor"]["firstBatch"].length);

// Demonstrate that durable history can be used across a restart with a finished index.
assert.commandWorked(testDB().adminCommand({fsync: 1}));
replTest.restart(primary());

assert.eq(4, findWithIndex(undefined, /*shouldSucceed=*/true)["cursor"]["firstBatch"].length);
assert.eq(5, findWithIndex(insert({a: 4}), /*shouldSucceed=*/true)["cursor"]["firstBatch"].length);
assert.eq(5, findWithIndex(undefined, /*shouldSucceed=*/true)["cursor"]["firstBatch"].length);

findWithIndex(oldestTS, /*shouldSucceed=*/false);
findWithIndex(createIndexTS, /*shouldSucceed=*/false);
findWithIndex(preIndexCommitTS, /*shouldSucceed=*/false);
findWithIndex(restartInsertTS, /*shouldSucceed=*/false);

assert.eq(4, findWithIndex(finalInsertTS, /*shouldSucceed=*/true)["cursor"]["firstBatch"].length);

replTest.stopSet();
})();

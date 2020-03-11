/**
 * This test shuts down a replica set during a two-phase index build. The test corrupts a WiredTiger
 * collection file and expects that --repair salvages the data and drops the unfinished index.
 *
 * @tags: [requires_wiredtiger, requires_replication]
 */

(function() {

load('jstests/disk/libs/wt_file_helper.js');
load('jstests/noPassthrough/libs/index_build.js');

const dbName = "repair_unfinished_indexes";
const collName = "test";

const replSet = new ReplSetTest({nodes: 2});
replSet.startSet();
replSet.initiate();

const primary = replSet.getPrimary();
const primaryDB = primary.getDB(dbName);

if (!IndexBuildTest.supportsTwoPhaseIndexBuild(primary)) {
    jsTestLog('Two phase index builds not supported, skipping test.');
    rst.stopSet();
    return;
}

const secondary = replSet.getSecondary();
const secondaryDB = secondary.getDB(dbName);
const secondaryPort = secondary.port;
const secondaryDbpath = secondary.dbpath;

const primaryColl = primaryDB.getCollection(collName);

assert.commandWorked(primaryColl.insert({_id: 0, a: 1}));

jsTestLog("Starting index build on primary and pausing before completion");
IndexBuildTest.pauseIndexBuilds(primary);
const createIdx = IndexBuildTest.startIndexBuild(primary, primaryColl.getFullName(), {a: 1});

jsTestLog("Waiting for secondary to start the index build");
IndexBuildTest.waitForIndexBuildToStart(secondaryDB);

const secondaryCollUri = getUriForColl(secondaryDB[collName]);
replSet.stop(secondary);

// Confirm that the secondary node leaves the index as unfinished.
(function startAsStandalone() {
    jsTestLog("Starting secondary as standalone");
    const mongod = startMongodOnExistingPath(
        secondaryDbpath,
        // This parameter ensures that when the standalone starts up, it applies all unapplied oplog
        // entries since the last shutdown. This "smooths out" a race condition in this test where
        // the secondary can shut down without fully applying the 'startIndexBuild' oplog entry, and
        // not advancing the stable timestamp to the top of the oplog.
        {setParameter: 'recoverFromOplogAsStandalone=true'});
    IndexBuildTest.assertIndexes(mongod.getDB(dbName).getCollection(collName),
                                 2,
                                 ["_id_"],
                                 ["a_1"],
                                 {includeBuildUUIDs: true});
    MongoRunner.stopMongod(mongod);
})();

const exitCode = createIdx({checkExitSuccess: false});
assert.neq(0, exitCode, 'expected shell to exit abnormally due to shutdown');

const secondaryCollFile = secondaryDbpath + "/" + secondaryCollUri + ".wt";
jsTestLog("Corrupting secondary collection file: " + secondaryCollFile);
corruptFile(secondaryCollFile);
assertRepairSucceeds(secondaryDbpath, secondaryPort);

// Importantly, confirm that the secondary node dropped the unfinished index.
(function startAsStandaloneAfterRepair() {
    jsTestLog("Starting secondary as standalone after repair");
    const mongod = startMongodOnExistingPath(secondaryDbpath);
    IndexBuildTest.assertIndexes(
        mongod.getDB(dbName).getCollection(collName), 1, ["_id_"], [], {includeBuildUUIDs: true});
    MongoRunner.stopMongod(mongod);
})();

// The secondary may not be reintroduced because data was modified.
assertErrorOnStartupWhenStartingAsReplSet(
    secondaryDbpath, secondaryPort, replSet.getReplSetConfig()._id);

(function reSyncSecondary() {
    jsTestLog("Wiping dbpath and re-syncing secondary");
    const newSecondary = assertStartInReplSet(
        replSet, secondary, true /* cleanData */, true /* expectResync */, function(node) {});

    IndexBuildTest.resumeIndexBuilds(primary);
    IndexBuildTest.waitForIndexBuildToStop(primaryDB);
    replSet.awaitReplication();
    IndexBuildTest.assertIndexes(primaryColl, 2, ["_id_", "a_1"]);
    IndexBuildTest.assertIndexes(
        newSecondary.getDB(dbName).getCollection(collName), 2, ["_id_", "a_1"]);
})();

replSet.stopSet();
})();

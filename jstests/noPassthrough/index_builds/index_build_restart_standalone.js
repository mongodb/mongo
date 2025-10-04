/**
 * Restarts replica set members in standalone mode after a shutdown during an in-progress two-phase
 * index build.
 *
 * @tags: [
 *   requires_persistence,
 *   requires_replication,
 * ]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {IndexBuildTest} from "jstests/noPassthrough/libs/index_builds/index_build.js";

const rst = new ReplSetTest({
    nodes: [
        {},
        {
            // Disallow elections on secondary.
            rsConfig: {
                priority: 0,
                votes: 0,
            },
        },
    ],
});

const nodes = rst.startSet();
rst.initiate();

const dbName = "test";
const collName = "testColl";

const primary = rst.getPrimary();
const primaryDB = primary.getDB(dbName);
const primaryColl = primaryDB.getCollection(collName);
const secondary = rst.getSecondary();
const secondaryDB = secondary.getDB(dbName);

assert.commandWorked(primaryColl.insert({a: 1}));

jsTest.log("Starting an index build on the primary and waiting for the secondary.");
IndexBuildTest.pauseIndexBuilds(primary);
const indexSpec = {
    a: 1,
};
const indexName = "a_1";
const createIndexCmd = IndexBuildTest.startIndexBuild(primary, primaryColl.getFullName(), indexSpec, {}, [
    ErrorCodes.InterruptedDueToReplStateChange,
]);
IndexBuildTest.waitForIndexBuildToStart(secondaryDB, collName, indexName);

// Wait for the stable timestamps on each node to advance, so that the  write is included in the
// stable checkpoint taken on shutdown.
jsTest.log("Wait for stable timestamps to advance.");
rst.awaitLastStableRecoveryTimestamp();

TestData.skipCheckDBHashes = true;
rst.stopSet(/*signal=*/ null, /*forRestart=*/ true);
TestData.skipCheckDBHashes = false;

function restartStandalone(node) {
    // Startup a mongod process on the nodes data files with recoverFromOplogAsStandalone=true. This
    // parameter ensures that when the standalone starts up, it applies all unapplied oplog entries
    // since the last shutdown.
    const recoveryMongod = MongoRunner.runMongod({
        dbpath: node.dbpath,
        noReplSet: true,
        noCleanData: true,
        setParameter: "recoverFromOplogAsStandalone=true",
    });

    // We need to shutdown this instance of mongod as using the recoverFromOplogAsStandalone=true
    // parameter puts the server into read-only mode, but we need to be able to perform writes for
    // this test.
    MongoRunner.stopMongod(recoveryMongod);

    return MongoRunner.runMongod({dbpath: node.dbpath, noReplSet: true, noCleanData: true});
}

(function restartPrimaryAsStandaloneAndCreate() {
    jsTest.log("Restarting primary in standalone mode.");
    const mongod = restartStandalone(primary);
    const db = mongod.getDB(dbName);
    const coll = db.getCollection(collName);
    IndexBuildTest.assertIndexes(coll, 2, ["_id_"], [indexName], {includeBuildUUIDs: true});

    // As a standalone, this should fail because of the unfinished index.
    assert.commandFailedWithCode(coll.createIndex(indexSpec), ErrorCodes.CannotCreateIndex);

    // Drop the index, then recreate it successfully.
    assert.commandWorked(coll.dropIndex(indexSpec));
    assert.commandWorked(coll.createIndex(indexSpec));
    IndexBuildTest.assertIndexes(coll, 2, ["_id_", indexName]);
    MongoRunner.stopMongod(mongod);
})();

(function restartSecondaryAsStandaloneAndCreate() {
    jsTest.log("Restarting secondary in standalone mode.");
    const mongod = restartStandalone(secondary);
    const db = mongod.getDB(dbName);
    const coll = db.getCollection(collName);
    IndexBuildTest.assertIndexes(coll, 2, ["_id_"], [indexName], {includeBuildUUIDs: true});

    // As a standalone, this should fail because of the unfinished index.
    assert.commandFailedWithCode(coll.createIndex(indexSpec), ErrorCodes.CannotCreateIndex);

    // Drop the index, then recreate it successfully.
    assert.commandWorked(coll.dropIndex(indexSpec));
    assert.commandWorked(coll.createIndex(indexSpec));
    IndexBuildTest.assertIndexes(coll, 2, ["_id_", indexName]);
    MongoRunner.stopMongod(mongod);
})();

createIndexCmd();

/**
 * A file used to perform a magic restore against the current primary node. Requires that a backup
 * cursor has already been taken by magic_restore_backup.js.
 */

import {
    _copyFileHelper,
    _runMagicRestoreNode,
    _writeObjsToMagicRestorePipe
} from "jstests/libs/backup_utils.js";

// Starts up a new node on dbpath where a backup cursor has already been written from sourceConn.
// sourceConn must also contain a timestamp in `test.magic_restore_checkpointTimestamp` of when the
// backup was taken.
function performRestore(sourceConn, dbpath) {
    // Read checkpointTimestamp from source cluster.
    const checkpointTimestamp = sourceConn.getDB("magic_restore_metadata")
                                    .getCollection("magic_restore_checkpointTimestamp")
                                    .findOne()
                                    .ts;
    const objs = [{
        "nodeType": "replicaSet",
        "replicaSetConfig": expectedConfig,
        "maxCheckpointTs": checkpointTimestamp,
    }];

    _writeObjsToMagicRestorePipe(objs, MongoRunner.dataDir);
    _runMagicRestoreNode(dbpath, MongoRunner.dataDir);
}

// Performs a data consistency check between two nodes. The `local` database is ignored due to
// containing different contents on the source and restore node. The collection
// `test.magic_restore_checkpointTimestamp` is ignored on the source node for comparisons.
function dataConsistencyCheck(sourceNode, restoreNode) {
    // Grab the list of databases from both nodes.
    // Need to filter out the metadata database from the source.
    const sourceDatabases = sourceNode.adminCommand(
        {listDatabases: 1, nameOnly: true, filter: {name: {$ne: "magic_restore_metadata"}}});
    const restoreDatabases = restoreNode.adminCommand({listDatabases: 1, nameOnly: true});

    const srcDbs = sourceDatabases.databases;
    const restoreDbs = restoreDatabases.databases;

    // Make sure the lists contain the same elements.
    if (srcDbs.length === restoreDbs.length &&
        srcDbs.every((element, index) => element === restoreDbs[index])) {
        throw new Error("Source and restore databases do not match");
    }

    srcDbs.forEach((element) => {
        const dbName = element["name"];

        // Ignore the `local` db.
        if (dbName === "local") {
            return;
        }

        let sourceDb = sourceNode.getDB(dbName);
        let restoreDb = restoreNode.getDB(dbName);

        let sourceCollectionInfos =
            new DBCommandCursor(sourceDb, assert.commandWorked(sourceDb.runCommand({
                listCollections: 1
            }))).toArray();
        sourceCollectionInfos.sort((a, b) => a.name.localeCompare(b.name));

        let restoreCollectionInfos =
            new DBCommandCursor(
                restoreDb,
                assert.commandWorked(restoreDb.runCommand({listCollections: 1, nameOnly: true})))
                .toArray();
        restoreCollectionInfos.sort((a, b) => a.name.localeCompare(b.name));

        let idx = 0;

        sourceCollectionInfos.forEach((sourceColl) => {
            const sourceCollName = sourceColl.name;

            // Skip the collection if it is temporary since it will not have been migrated in
            // restore.
            if (sourceColl.options.temp == true) {
                jsTestLog("Magic Restore: Skipping consistency check for temporary namespace " +
                          dbName + "." + sourceCollName + ".");
                return;
            }

            // If we have finished iterating restoreCollectionInfos then we are missing a
            // collection.
            assert(idx < restoreCollectionInfos.length,
                   "restore node is missing the " + dbName + "." + sourceCollName + " namespace.");

            const restoreCollName = restoreCollectionInfos[idx++].name;

            // Make sure we compare the same collections (if they don't match one is missing from
            // restore node).
            assert(sourceCollName === restoreCollName,
                   "restore node is missing the " + dbName + "." + sourceCollName + " namespace.");

            let sourceCursor = sourceDb.getCollection(sourceCollName).find().sort({_id: 1});
            let restoreCursor = restoreDb.getCollection(restoreCollName).find().sort({_id: 1});

            let diff = DataConsistencyChecker.getDiff(sourceCursor, restoreCursor);

            assert.eq(
                diff,
                {
                    docsWithDifferentContents: [],
                    docsMissingOnFirst: [],
                    docsMissingOnSecond: [],
                },
                `Magic Restore: The magic restore node and source do not match for namespace ${
                    dbName + "." + sourceCollName}`);
        });
        // Source cursor has been exhausted, the restore node should be too.
        assert(idx == restoreCollectionInfos.length,
               "restore node contains more collections than its source for the " + dbName +
                   " database.");
    });
}

const conn = db.getMongo();
const backupDbPath = MongoRunner.dataPath + '../magicRestore';

jsTestLog("Magic Restore: Beginning magic restore.");

let rst = new ReplSetTest({nodes: 1});

rst.startSet();
rst.initiateWithHighElectionTimeout();

jsTestLog("Magic Restore: Getting config.");

let expectedConfig =
    assert.commandWorked(rst.getPrimary().adminCommand({replSetGetConfig: 1})).config;

jsTestLog("Magic Restore: Stopping cluster.");

rst.stopSet(null /*signal*/, true /*forRestart*/);

jsTestLog("Magic Restore: Restarting with magic restore options.");

performRestore(conn, backupDbPath);

jsTestLog("Magic Restore: Starting restore cluster for data consistency check.");

rst.startSet({restart: true, dbpath: backupDbPath});

try {
    dataConsistencyCheck(conn, rst.getPrimary());
} finally {
    jsTestLog("Magic Restore: Stopping magic restore cluster and cleaning up restore dbpath.");

    // ReplSetTest clears the dbpath when it is stopped.
    rst.stopSet();
}

jsTestLog("Magic Restore: Magic restore complete.");

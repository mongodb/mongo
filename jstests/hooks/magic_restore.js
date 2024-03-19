/**
 * A file used to perform a magic restore against the current primary node. Requires that a backup
 * cursor has already been taken by magic_restore_backup.js.
 */

import {
    _copyFileHelper,
    _runMagicRestoreNode,
    _writeObjsToMagicRestorePipe
} from "jstests/libs/backup_utils.js";
import {DiscoverTopology, Topology} from "jstests/libs/discover_topology.js";

// Starts up a new node on dbpath where a backup cursor has already been written from sourceConn.
// sourceConn must also contain a timestamp in `test.magic_restore_checkpointTimestamp` of when the
// backup was taken.
function performRestore(sourceConn, expectedConfig, dbpath, options) {
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
    _runMagicRestoreNode(dbpath, MongoRunner.dataDir, options);
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

            // When we restore a sharded cluster we are running the individual shards individually
            // as replica sets. This causes the system.keys collection to be populated differently
            // than it is in a complete sharded cluster with configsvr. The `config.mongos`
            // collection is expected to be different here since shard names and last known ping
            // times will be different from the source node.
            if (sourceCollName === "system.keys" || sourceCollName === "mongos") {
                return;
            }

            // If we have finished iterating restoreCollectionInfos then we are missing a
            // collection.
            assert(idx < restoreCollectionInfos.length,
                   "restore node is missing the " + dbName + "." + sourceCollName + " namespace.");

            let restoreCollName = restoreCollectionInfos[idx++].name;

            // When we restore a sharded cluster we are running the individual shards individually
            // as replica sets. This causes the system.keys collection to be populated differently
            // than it is in a complete sharded cluster with configsvr. The `config.mongos`
            // collection is expected to be different here since shard names and last known ping
            // times will be different from the source node.
            if (restoreCollName === "system.keys" || restoreCollName === "mongos") {
                restoreCollName = restoreCollectionInfos[idx++].name;
            }

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

function performMagicRestore(sourceNode, dbPath, options) {
    jsTestLog("Magic Restore: Beginning magic restore for node " + sourceNode + ".");

    let rst = new ReplSetTest({nodes: 1});

    rst.startSet();
    rst.initiateWithHighElectionTimeout();

    let expectedConfig =
        assert.commandWorked(rst.getPrimary().adminCommand({replSetGetConfig: 1})).config;

    jsTestLog("Magic Restore: Stopping cluster.");

    rst.stopSet(null /*signal*/, true /*forRestart*/);

    jsTestLog("Magic Restore: Restarting with magic restore options.");

    performRestore(sourceNode, expectedConfig, dbPath);

    jsTestLog("Magic Restore: Starting restore cluster for data consistency check.");

    rst.startSet({restart: true, dbpath: dbPath});

    try {
        dataConsistencyCheck(sourceNode, rst.getPrimary());
    } finally {
        jsTestLog("Magic Restore: Stopping magic restore cluster and cleaning up restore dbpath.");

        // ReplSetTest clears the dbpath when it is stopped.
        rst.stopSet();
    }

    jsTestLog("Magic Restore: Magic restore complete.");
}

const topology = DiscoverTopology.findConnectedNodes(db);

if (topology.type == Topology.kShardedCluster) {
    // Perform restore for the config server.
    const path = MongoRunner.dataPath + '../magicRestore/configsvr/node0'
    let configMongo = new Mongo(topology.configsvr.nodes[0]);
    performMagicRestore(configMongo, path, {"configsvr": ''});

    // Need to iterate over the shards and do one restore per shard.
    for (const [shardName, shard] of Object.entries(topology.shards)) {
        const dbPathPrefix = MongoRunner.dataPath + '../magicRestore/' + shardName + '/node0';
        let nodeMongo = new Mongo(shard.nodes[0]);
        performMagicRestore(nodeMongo, dbPathPrefix, {"replSet": shardName, "shardsvr": ''});
    }
} else {
    // Is replica set so just need to do one restore.
    const conn = db.getMongo();
    const backupDbPath = MongoRunner.dataPath + '../magicRestore/node0';
    performMagicRestore(conn, backupDbPath, {"replSet": "rs"});
}

/**
 * A file used to perform a magic restore against the current primary node. Requires that a backup
 * cursor has already been taken by magic_restore_backup.js.
 */

import {MagicRestoreUtils} from "jstests/libs/backup_utils.js";
import {DiscoverTopology, Topology} from "jstests/libs/discover_topology.js";

// Starts up a new node on dbpath where a backup cursor has already been written from sourceConn.
// sourceConn must also contain a timestamp in `test.magic_restore_checkpointTimestamp` of when the
// backup was taken.
function performRestore(sourceConn, expectedConfig, dbpath, name, options) {
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
    jsTestLog("Restore configuration: " + tojson(objs[0]));

    let oplog = sourceConn.getDB("local").getCollection('oplog.rs');
    const entriesAfterBackup =
        oplog.find({ts: {$gt: checkpointTimestamp}, ns: {$ne: "magic_restore_metadata"}})
            .sort({ts: 1})
            .toArray();

    if (entriesAfterBackup.length > 0) {
        // BSON arrays take up more space than raw objects do, but computing the size of a BSON
        // array is extremely expensive (O(N^2) time). As a compromise we will limit our size to be
        // 90% of the real BSON max which should allow us to stay under the threshold of max BSON
        // size. This will cause us to need to disable some tests with super large oplog entries.
        const maxSize = sourceConn.getDB("test").hello().maxBsonObjectSize * 0.9;

        let currentBatch = [];
        let currentBatchSize = 0;

        const metadataDocument = {
            "nodeType": "replicaSet",
            "replicaSetConfig": expectedConfig,
            "maxCheckpointTs": checkpointTimestamp,
            // Restore to the timestamp of the last oplog entry on the source cluster.
            "pointInTimeTimestamp": entriesAfterBackup[entriesAfterBackup.length - 1].ts
        };

        currentBatch.push(metadataDocument);
        currentBatchSize += Object.bsonsize(metadataDocument);

        // Loop over every oplog entry and try and fit it into a batch. If a batch goes over maxBSON
        // size then we create a new batch.
        entriesAfterBackup.forEach((entry) => {
            // See if the entry could push the current batch over the max size, if so we need to
            // start a new one.
            const entrySize = Object.bsonsize(entry);
            if (currentBatchSize + entrySize > maxSize) {
                jsTestLog("Magic Restore: Writing " + currentBatchSize.toString() +
                          " bytes to pipe.");

                MagicRestoreUtils.writeObjsToMagicRestorePipe(
                    MongoRunner.dataDir + "/" + name, currentBatch, true /* persistPipe */);

                currentBatch = [];
                currentBatchSize = 0;

                // Writing items to the restore pipe can take a long time for 16MB of documents. Do
                // a small sleep here to make sure we do not write oplog entries out of order.
                sleep(2000);
            }

            // Add the entry to the current batch.
            currentBatch.push(entry);
            currentBatchSize += entrySize;
        });

        // If non-empty batch remains push it into batches.
        if (currentBatch.length != 0) {
            MagicRestoreUtils.writeObjsToMagicRestorePipe(
                MongoRunner.dataDir + "/" + name, currentBatch, true /* persistPipe */);
        }
    } else {
        const objs = [{
            "nodeType": "replicaSet",
            "replicaSetConfig": expectedConfig,
            "maxCheckpointTs": checkpointTimestamp,
        }];
        MagicRestoreUtils.writeObjsToMagicRestorePipe(MongoRunner.dataDir + "/" + name, objs);
    }

    MagicRestoreUtils.runMagicRestoreNode(MongoRunner.dataDir + "/" + name, dbpath, options);
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
        const dbStats = assert.commandWorked(sourceDb.runCommand({dbStats: 1}));
        jsTestLog("Magic Restore: Checked the consistency of database " + dbName +
                  ". dbStats: " + tojson(dbStats));
    });
}

function performMagicRestore(sourceNode, dbPath, name, options) {
    jsTestLog("Magic Restore: Beginning magic restore for node " + sourceNode.host + ".");

    let rst = new ReplSetTest({nodes: 1});

    rst.startSet();
    rst.initiateWithHighElectionTimeout();

    let expectedConfig =
        assert.commandWorked(rst.getPrimary().adminCommand({replSetGetConfig: 1})).config;

    jsTestLog("Magic Restore: Stopping cluster.");

    rst.stopSet(null /*signal*/, true /*forRestart*/);

    jsTestLog("Magic Restore: Restarting with magic restore options.");

    performRestore(sourceNode, expectedConfig, dbPath, name, options);

    jsTestLog("Magic Restore: Starting restore cluster for data consistency check.");

    rst.startSet({restart: true, dbpath: dbPath});

    dataConsistencyCheck(sourceNode, rst.getPrimary());

    jsTestLog("Magic Restore: Stopping magic restore cluster and cleaning up restore dbpath.");

    // TODO SERVER-87225: Remove skipValidation once fastcount works properly for PIT restore.
    // ReplSetTest clears the dbpath when it is stopped.
    rst.stopSet(null, false, {'skipValidation': true});

    jsTestLog("Magic Restore: Magic restore complete.");
}

const topology = DiscoverTopology.findConnectedNodes(db);

if (topology.type == Topology.kShardedCluster) {
    // Perform restore for the config server.
    const path = MongoRunner.dataPath + '../magicRestore/configsvr/node0'
    let configMongo = new Mongo(topology.configsvr.nodes[0]);
    performMagicRestore(configMongo, path, "configsvr", {"replSet": "config-rs", "configsvr": ''});

    // Need to iterate over the shards and do one restore per shard.
    for (const [shardName, shard] of Object.entries(topology.shards)) {
        const dbPathPrefix = MongoRunner.dataPath + '../magicRestore/' + shardName + '/node0';
        let nodeMongo = new Mongo(shard.nodes[0]);
        performMagicRestore(
            nodeMongo, dbPathPrefix, shardName, {"replSet": shardName, "shardsvr": ''});
    }
} else {
    // Is replica set so just need to do one restore.
    const conn = db.getMongo();
    const backupDbPath = MongoRunner.dataPath + '../magicRestore/node0';
    performMagicRestore(conn, backupDbPath, "rs", {"replSet": "rs"});
}

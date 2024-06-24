/**
 * A file used to perform a magic restore against the current primary node. Requires that a backup
 * cursor has already been taken by magic_restore_backup.js.
 */

import {MagicRestoreUtils} from "jstests/libs/backup_utils.js";
import {DiscoverTopology, Topology} from "jstests/libs/discover_topology.js";

// Starts up a new node on dbpath where a backup cursor has already been written from sourceConn.
// sourceConn must also contain a timestamp in `test.magic_restore_checkpointTimestamp` of when the
// backup was taken.
function performRestore(sourceConn, expectedConfig, nodeType, dbpath, name, options) {
    // Read checkpointTimestamp from source cluster.
    const checkpointTimestamp = sourceConn.getDB("magic_restore_metadata")
                                    .getCollection("magic_restore_checkpointTimestamp")
                                    .findOne()
                                    .ts;

    let consistencyTs = checkpointTimestamp;
    let oplog = sourceConn.getDB("local").getCollection('oplog.rs');
    const entriesAfterBackup =
        oplog
            .find(
                {ts: {$gt: checkpointTimestamp}, ns: {$not: {$regex: "magic_restore_metadata.*"}}})
            .sort({ts: 1})
            .toArray();

    if (entriesAfterBackup.length > 0) {
        // Need to check if the oplog has been truncated before attempting a PIT restore. If the
        // oplog has been truncated and the entry at the checkpoint timestamp does not exist we
        // cannot proceed with a PIT restore. We should throw an error so we don't get an obscure
        // error which looks like data inconsistency later.
        const checkpointOplogEntry = oplog.findOne({ns: {$regex: "magic_restore_metadata.*"}});
        if (!checkpointOplogEntry) {
            throw new Error(
                "Oplog has been truncated while getting PIT restore oplog entries during Magic Restore.");
        }

        // BSON arrays take up more space than raw objects do, but computing the size of a BSON
        // array is extremely expensive (O(N^2) time). As a compromise we will limit our size to be
        // 90% of the real BSON max which should allow us to stay under the threshold of max BSON
        // size. This will cause us to need to disable some tests with super large oplog entries.
        const maxSize = sourceConn.getDB("test").hello().maxBsonObjectSize * 0.9;

        let currentBatch = [];
        let currentBatchSize = 0;

        const metadataDocument = {
            "nodeType": nodeType,
            "replicaSetConfig": expectedConfig,
            "maxCheckpointTs": checkpointTimestamp,
            // Restore to the timestamp of the last oplog entry on the source cluster.
            "pointInTimeTimestamp": entriesAfterBackup[entriesAfterBackup.length - 1].ts
        };
        jsTestLog("Restore configuration: " + tojson(metadataDocument));
        consistencyTs = entriesAfterBackup[entriesAfterBackup.length - 1].ts;

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
            "nodeType": nodeType,
            "replicaSetConfig": expectedConfig,
            "maxCheckpointTs": checkpointTimestamp,
        }];
        jsTestLog("Restore configuration: " + tojson(objs[0]));
        MagicRestoreUtils.writeObjsToMagicRestorePipe(MongoRunner.dataDir + "/" + name, objs);
    }

    MagicRestoreUtils.runMagicRestoreNode(MongoRunner.dataDir + "/" + name, dbpath, options);
    return consistencyTs;
}

// Helper function to retrieve the databases and collections on a node. The result is a map of
// database names to lists of collections in that database.
function getDatabasesAndCollectionsSnapshot(node, consistencyTs) {
    // Magic restore explicitly drops these collections from the config database.
    const excludedCollections = [
        "clusterParameters",
        "mongos",
        "cache.collections",
        "cache.databases",
    ];
    return node.getDB("admin")
        .aggregate([{$listCatalog: {}}],
                   {readConcern: {level: 'snapshot', atClusterTime: consistencyTs}})
        .toArray()
        .reduce((acc, {db, name, md}) => {
            // Need to filter out the metadata database from the source.
            if (db === "magic_restore_metadata") {
                return acc;
            }
            // Skip the collection if it is temporary since it will not have been migrated in
            // restore.
            if (md && md.options.temp == true) {
                jsTestLog("Magic Restore: Skipping consistency check for temporary namespace " +
                          db + "." + name + ".");
                return acc;
            }

            // We drop cached metadata during restore. There could be cached metadata for different
            // namespaces produced by the tests, so we must match any name to "cache.chunks.*".
            if (db === "config" &&
                (excludedCollections.includes(name) || name.startsWith("cache.chunks"))) {
                return acc;
            }

            // Magic restore will drop and re-insert the shard identity document on config servers,
            // which will alter the field ordering. We manually check the shard identity document
            // elsewhere.
            if (db === "admin" && name === "system.version") {
                return acc;
            }

            if (!acc[db]) {
                acc[db] = [];
            }
            acc[db].push(name);
            return acc;
        }, {});
}

// Performs a data consistency check between two nodes. The `local` database is ignored due to
// containing different contents on the source and restore node. The collection
// `test.magic_restore_checkpointTimestamp` is ignored on the source node for comparisons.
function dataConsistencyCheck(sourceNode, restoreNode, consistencyTs) {
    // Grab the database and collection names from both nodes.
    const sourceDatabases = getDatabasesAndCollectionsSnapshot(sourceNode, consistencyTs);
    const restoreDatabases = getDatabasesAndCollectionsSnapshot(restoreNode, consistencyTs);

    // Make sure the lists contain the same elements.
    if (Object.keys(sourceDatabases).length !== Object.keys(restoreDatabases).length ||
        Object.keys(sourceDatabases).every((dbName) => !restoreDatabases.hasOwnProperty(dbName))) {
        throw new Error("Source and restore databases do not match. source database names: " +
                        tojson(Object.keys(sourceDatabases)) +
                        ". restore database names: " + tojson(Object.keys(restoreDatabases)));
    }

    // Check the shard identity documents.
    let sourceShardIdentity =
        sourceNode.getDB("admin").getCollection("system.version").findOne({_id: "shardIdentity"});
    let destShardIdentity =
        sourceNode.getDB("admin").getCollection("system.version").findOne({_id: "shardIdentity"});
    // On replica set nodes, output of 'findOne' will be null as the shard identity document should
    // not exist. On shard and config servers, the documents may have different field orderings but
    // the contents should match.
    assert((sourceShardIdentity === null && destShardIdentity === null) ||
               bsonUnorderedFieldsCompare(sourceShardIdentity, destShardIdentity) === 0,
           "shard identity documents do not match. source shard identity: " +
               tojson(sourceShardIdentity) +
               " destination shard identity: " + tojson(destShardIdentity));

    Object.keys(sourceDatabases).forEach((dbName) => {
        // Ignore the `local` db.
        if (dbName === "local") {
            return;
        }

        let sourceDb = sourceNode.getDB(dbName);
        let restoreDb = restoreNode.getDB(dbName);

        let sourceCollections = sourceDatabases[dbName].sort((a, b) => a.localeCompare(b));
        let restoreCollections = restoreDatabases[dbName].sort((a, b) => a.localeCompare(b));

        let idx = 0;
        sourceCollections.forEach((sourceCollName) => {
            // If we have finished iterating restoreCollections then we are missing a
            // collection.
            assert(idx < restoreCollections.length,
                   "restore node is missing the " + dbName + "." + sourceCollName + " namespace.");

            let restoreCollName = restoreCollections[idx++];

            // When we restore a sharded cluster we are running the individual shards individually
            // as replica sets. This causes the system.keys collection to be populated differently
            // than it is in a complete sharded cluster with configsvr. The `config.mongos`
            // collection is expected to be different here since shard names and last known ping
            // times will be different from the source node. The preimages and change_collections
            // collections use independent untimestamped truncates to delete old data, and therefore
            // they be inconsistent between source and destination.
            if (sourceCollName === "system.keys" || sourceCollName === "mongos" ||
                sourceCollName === "system.preimages" ||
                sourceCollName === "system.change_collection") {
                return;
            }

            // Make sure we compare the same collections (if they don't match one is missing from
            // restore node).
            assert(sourceCollName === restoreCollName,
                   "restore node is missing the " + dbName + "." + sourceCollName + " namespace.");

            // Reads on config.transactions do not support snapshot read concern, so we should read
            // with 'majority'.
            let readConcern =
                dbName === "config" && sourceCollName === "transactions" ? "majority" : "snapshot";
            let atClusterTime =
                dbName === "config" && sourceCollName === "transactions" ? null : consistencyTs;
            let sourceCursor = sourceDb.getCollection(sourceCollName)
                                   .find()
                                   .readConcern(readConcern, atClusterTime)
                                   .sort({_id: 1});
            let restoreCursor = restoreDb.getCollection(restoreCollName)
                                    .find()
                                    .readConcern(readConcern, atClusterTime)
                                    .sort({_id: 1});

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
        assert(idx == restoreCollections.length,
               "restore node contains more collections than its source for the " + dbName +
                   " database.");
        const dbStats = assert.commandWorked(sourceDb.runCommand({dbStats: 1}));
        jsTestLog("Magic Restore: Checked the consistency of database " + dbName +
                  ". dbStats: " + tojson(dbStats));
    });
}

function performMagicRestore(sourceNode, dbPath, nodeType, name, options) {
    jsTestLog("Magic Restore: Beginning magic restore for node " + sourceNode.host + ".");

    let rst = new ReplSetTest({nodes: 1});

    rst.startSet();
    rst.initiateWithHighElectionTimeout();

    let expectedConfig =
        assert.commandWorked(rst.getPrimary().adminCommand({replSetGetConfig: 1})).config;

    jsTestLog("Magic Restore: Stopping cluster.");

    rst.stopSet(null /*signal*/, true /*forRestart*/);

    jsTestLog("Magic Restore: Restarting with magic restore options.");

    // Increase snapshot history window on the restore node so we don't get a SnapshotTooOld error
    // when doing the consistency checker for long running tests.
    const snapshotHistory = 3600;
    options.setParameter = {minSnapshotHistoryWindowInSeconds: snapshotHistory};

    // performRestore returns a read timestamp for snapshot reads in consistency checks.
    const consistencyTs =
        performRestore(sourceNode, expectedConfig, nodeType, dbPath, name, options);

    jsTestLog(
        "Magic Restore: Starting restore cluster for data consistency check at snapshot timestamp " +
        tojson(consistencyTs) + ".");

    rst.startSet({
        restart: true,
        dbpath: dbPath,
        setParameter: {minSnapshotHistoryWindowInSeconds: snapshotHistory}
    });

    dataConsistencyCheck(sourceNode, rst.getPrimary(), consistencyTs);

    jsTestLog("Magic Restore: Stopping magic restore cluster and cleaning up restore dbpath.");

    // TODO SERVER-87225: Remove skipValidation once fastcount works properly for PIT restore.
    // ReplSetTest clears the dbpath when it is stopped.
    rst.stopSet(null, false, {'skipValidation': true});

    jsTestLog("Magic Restore: Magic restore complete.");
}

const topology = DiscoverTopology.findConnectedNodes(db);

if (topology.type == Topology.kShardedCluster) {
    // Perform restore for the config server.
    const path = MongoRunner.dataPath + '../magicRestore/configsvr/node0';
    let configMongo = new Mongo(topology.configsvr.nodes[0]);

    // Config shards must perform both dedicated config server and shard server steps in restore, so
    // we must make the distinction between a config shard and dedicated config server in the
    // nodeType. We can determine the node role by checking the 'config.shards' collection and the
    // node's shard identity document.
    const isConfigShard = (conn) => {
        const configShardDoc = conn.getDB("config").shards.findOne({_id: "config"});
        if (configShardDoc == null) {
            return false;
        }
        const shardIdentityDoc = conn.getDB("admin").system.version.findOne({_id: "shardIdentity"});
        if (shardIdentityDoc == null) {
            return false;
        }
        return shardIdentityDoc.shardName == "config";
    };
    const cfgNodeType = isConfigShard(configMongo) ? "configShard" : "configServer";
    performMagicRestore(configMongo, path, cfgNodeType, "configsvr", {"replSet": "config-rs"});

    // Need to iterate over the shards and do one restore per shard.
    for (const [shardName, shard] of Object.entries(topology.shards)) {
        const dbPathPrefix = MongoRunner.dataPath + '../magicRestore/' + shardName + '/node0';
        let nodeMongo = new Mongo(shard.nodes[0]);
        performMagicRestore(nodeMongo, dbPathPrefix, "shard", shardName, {"replSet": shardName});
    }
} else {
    // Is replica set so just need to do one restore.
    const conn = db.getMongo();
    const backupDbPath = MongoRunner.dataPath + '../magicRestore/node0';
    performMagicRestore(conn, backupDbPath, "replicaSet", "rs", {"replSet": "rs"});
}

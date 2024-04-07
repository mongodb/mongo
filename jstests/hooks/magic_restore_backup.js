/**
 * A file used to open a backup cursor and copy data files from the current primary node.
 */

import {
    copyBackupCursorExtendFiles,
    copyBackupCursorFiles,
    extendBackupCursor,
    getBackupCursorMetadata,
    openBackupCursor
} from "jstests/libs/backup_utils.js";
import {DiscoverTopology, Topology} from "jstests/libs/discover_topology.js";

function writeMetadataInfo(conn, checkpoint) {
    let testDB = conn.getDB("magic_restore_metadata");
    testDB.runCommand({insert: "magic_restore_checkpointTimestamp", documents: [{ts: checkpoint}]});
}

function takeBackup(conn, dbPathPrefix) {
    jsTestLog("Magic Restore: Taking backup of node " + conn.host);
    // Take the initial checkpoint.
    assert.commandWorked(db.adminCommand({fsync: 1}));

    // Creates the directory if it doesn't exist already.
    mkdir(dbPathPrefix);
    let backupCursor = openBackupCursor(conn.getDB("admin"));
    let metadata = getBackupCursorMetadata(backupCursor);
    jsTestLog("Backup cursor metadata document: " + tojson(metadata));
    copyBackupCursorFiles(backupCursor, /*namespacesToSkip=*/[], metadata.dbpath, dbPathPrefix);

    jsTestLog("Magic Restore: Backup written to " + dbPathPrefix);

    return [backupCursor, metadata];
}

const topology = DiscoverTopology.findConnectedNodes(db);

if (topology.type == Topology.kReplicaSet) {
    const conn = db.getMongo();
    const dbPathPrefix = MongoRunner.dataPath + '../magicRestore/node0'
    let [cursor, metadata] = takeBackup(conn, dbPathPrefix);
    writeMetadataInfo(conn, metadata.checkpointTimestamp);
    cursor.close();
} else {
    let nodes = [];
    let dbPaths = [];
    let restorePaths = [];
    let backupIds = [];
    let cursors = [];
    const checkpointTimestamps = {};

    let maxCheckpointTimestamp = Timestamp();

    // Take configsvr backup.
    const path = MongoRunner.dataPath + '../magicRestore/configsvr/node0'
    restorePaths.push(path);

    let nodeMongo = new Mongo(topology.configsvr.nodes[0]);
    nodes.push(nodeMongo);

    let [cursor, metadata] = takeBackup(nodeMongo, path);
    dbPaths.push(metadata.dbpath);
    backupIds.push(metadata.backupId);
    checkpointTimestamps[nodeMongo.host] = metadata.checkpointTimestamp;

    if (timestampCmp(metadata.checkpointTimestamp, maxCheckpointTimestamp) > 0) {
        maxCheckpointTimestamp = metadata.checkpointTimestamp;
    }

    cursors.push(cursor);

    // Take backup for each shard.
    for (const [shardName, shard] of Object.entries(topology.shards)) {
        const dbPathPrefix = MongoRunner.dataPath + '../magicRestore/' + shardName + '/node0';
        restorePaths.push(dbPathPrefix);

        let nodeMongo = new Mongo(shard.nodes[0]);
        let [cursor, metadata] = takeBackup(nodeMongo, dbPathPrefix);

        nodes.push(nodeMongo);

        dbPaths.push(metadata.dbpath);
        backupIds.push(metadata.backupId);
        checkpointTimestamps[nodeMongo.host] = metadata.checkpointTimestamp;

        if (timestampCmp(metadata.checkpointTimestamp, maxCheckpointTimestamp) > 0) {
            maxCheckpointTimestamp = metadata.checkpointTimestamp;
        }

        cursors.push(cursor);
    }
    jsTestLog("Magic Restore: Checkpoint timestamps for nodes: " + tojson(checkpointTimestamps));
    jsTestLog("Magic Restore: maxCheckpointTimestamp for cluster is " +
              tojson(maxCheckpointTimestamp));
    let threads = [];

    // Need to extend the backup cursor and copy the files over for each shard node.
    // Then need to write the max checkpoint timestamp into the node.
    for (let i = 0; i < nodes.length; i++) {
        writeMetadataInfo(nodes[i], maxCheckpointTimestamp);
        jsTestLog("Magic Restore: Extending backup cursor for node " + nodes[i].host +
                  " to timestamp: " + tojson(maxCheckpointTimestamp));
        let cursor = extendBackupCursor(nodes[i], backupIds[i], maxCheckpointTimestamp);
        let thread = copyBackupCursorExtendFiles(
            cursor, /*namespacesToSkip=*/[], dbPaths[i], restorePaths[i], true);
        threads.push(thread);
        cursor.close();
    }

    threads.forEach((thread) => {
        thread.join();
    });
    cursors.forEach((cursor) => {
        cursor.close();
    });
}

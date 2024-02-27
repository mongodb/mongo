/*
 * Basic tests for _shardsvrBeginMigrationBlockingOperation/_shardsvrEndMigrationBlockingOperation.
 * @tags: [
 *  featureFlagPauseMigrationsDuringMultiUpdatesAvailable,
 *  requires_fcv_80
 * ]
 */

import {migrationsAreAllowed} from "jstests/libs/chunk_manipulation_util.js";
import {CreateShardedCollectionUtil} from "jstests/sharding/libs/create_sharded_collection_util.js";
import {ShardVersioningUtil} from "jstests/sharding/libs/shard_versioning_util.js";

const st = new ShardingTest({shards: {rs0: {nodes: 3}}});
const replicaSet = new ReplSetTest({nodes: 1});
replicaSet.startSet();
replicaSet.initiate();

const dbName = "test";
const collName = "coll";
const namespace = `${dbName}.${collName}`;

const db = st.s.getDB(dbName);
const coll = st.s.getCollection(namespace);
CreateShardedCollectionUtil.shardCollectionWithChunks(coll, {key: 1}, [
    {min: {key: MinKey}, max: {key: MaxKey}, shard: st.shard0.shardName},
]);

function assertCommandReturns(connection, command, uuid, code) {
    const response = connection.adminCommand({
        [command]: namespace,
        operationId: uuid,
        databaseVersion: ShardVersioningUtil.getDbVersion(st.s, dbName)
    });
    if (code === ErrorCodes.OK) {
        assert.commandWorked(response);
    } else {
        assert.commandFailedWithCode(response, code)
    }
}

const kBeginCommand = "_shardsvrBeginMigrationBlockingOperation";
const kEndCommand = "_shardsvrEndMigrationBlockingOperation";
const uuid = UUID();

for (const command of [kBeginCommand, kEndCommand]) {
    jsTestLog(`Performing basic tests for command ${command}`);
    // Verify _shardsvrCoordinateMultiUpdate only runs on shard servers.
    assertCommandReturns(st.rs0.getSecondary(), command, uuid, ErrorCodes.NotWritablePrimary);
    assertCommandReturns(st.s, command, uuid, ErrorCodes.CommandNotFound);
    assertCommandReturns(
        replicaSet.getPrimary(), command, uuid, ErrorCodes.ShardingStateNotInitialized);
}

assert(migrationsAreAllowed(db, collName));
assertCommandReturns(st.rs0.getPrimary(), kBeginCommand, uuid, ErrorCodes.OK);
assert(!migrationsAreAllowed(db, collName));
assertCommandReturns(st.rs0.getPrimary(), kEndCommand, uuid, ErrorCodes.OK);
assert(migrationsAreAllowed(db, collName));

replicaSet.stopSet();
st.stop();

/*
 * Verify that after a failover a MultiUpdateCoordinator tries to wait for any ongoing updates on
 * other nodes before returning. We don't make any guarantees regarding updateMany if there is a
 * failover, so this behavior should be considered best effort.
 * @tags: [
 *  featureFlagPauseMigrationsDuringMultiUpdatesAvailable,
 *  requires_fcv_80
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {CreateShardedCollectionUtil} from "jstests/sharding/libs/create_sharded_collection_util.js";
import {ShardVersioningUtil} from "jstests/sharding/libs/shard_versioning_util.js";

const st = new ShardingTest({shards: {rs0: {nodes: 3}, rs1: {nodes: 3}}});

function assertCoordinatorStillRunningAfterDelay() {
    sleep(5000);
    const configDb = st.rs0.getPrimary().getDB("config");
    const stateDocuments = configDb.localMigrationBlockingOperations.multiUpdateCoordinators;
    assert.eq(1, stateDocuments.countDocuments({ns: namespace}));
}

const dbName = "test";
const collName = "coll";
const namespace = `${dbName}.${collName}`;
const mongos = st.s0;

const sourceCollection = mongos.getCollection(namespace);
CreateShardedCollectionUtil.shardCollectionWithChunks(sourceCollection, {oldKey: 1}, [
    {min: {oldKey: MinKey}, max: {oldKey: 0}, shard: st.shard0.shardName},
    {min: {oldKey: 0}, max: {oldKey: MaxKey}, shard: st.shard1.shardName},
]);
assert.commandWorked(st.s.adminCommand({movePrimary: dbName, to: st.shard0.name}));

assert.commandWorked(st.s0.getDB(dbName).getCollection(collName).insertMany([
    {
        _id: 1,
        member: "abc123",
        points: 0,
        _oldKey: -5,
    },
    {
        _id: 2,
        member: "abc123",
        points: 59,
        _oldKey: 10,
    },
]));

const shard0Primary = st.rs0.getPrimary();
const shard1Primary = st.rs1.getPrimary();
const hangBeforeUpdateFp =
    configureFailPoint(shard1Primary, "hangUpdateBeforeWrite", {ns: namespace});

const joinShell = startParallelShell(
    funWithArgs((nss, collName, databaseVersion) => {
        const resp = db.adminCommand({
            _shardsvrCoordinateMultiUpdate: nss,
            uuid: UUID(),
            command: {
                update: collName,
                updates: [{q: {member: "abc123"}, u: {$set: {points: 50}}, multi: true}]
            },
            databaseVersion
        });
        assert.commandFailed(resp);
    }, namespace, collName, ShardVersioningUtil.getDbVersion(st.s, dbName)), shard0Primary.port);

hangBeforeUpdateFp.wait();

const newPrimary = st.rs0.getSecondary();
const waitForPendingUpdateFp =
    configureFailPoint(newPrimary, "hangDuringMultiUpdateCoordinatorPendingUpdates");

st.rs0.stepUp(newPrimary);
st.rs0.awaitNodesAgreeOnPrimary();

waitForPendingUpdateFp.wait();
waitForPendingUpdateFp.off();

assertCoordinatorStillRunningAfterDelay();

hangBeforeUpdateFp.off();
joinShell();

st.stop();

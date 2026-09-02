/**
 * Test that an update command updating the shard key and moving a document across shards does
 * not fail when the server is already in shutdown.
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";
import {assertCreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";

const dbName = jsTestName();
const collName = jsTestName();

const st = new ShardingTest({
    mongos: 1,
    mongosOptions: {setParameter: {featureFlagUnifiedWriteExecutor: false}},
    shards: 2,
    rs: {nodes: 1},
});

const db = st.s.getDB(dbName);

// Create sharding and split the test collection between two shards.
assert.commandWorked(db.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));
const coll = assertCreateCollection(db, collName);

assert.commandWorked(db.adminCommand({shardCollection: coll.getFullName(), key: {shardKey: 1}}));
assert.commandWorked(db.adminCommand({split: coll.getFullName(), middle: {shardKey: 0}}));

assert.commandWorked(
    db.adminCommand({
        moveChunk: coll.getFullName(),
        find: {shardKey: -1},
        to: st.shard0.shardName,
        _waitForDelete: true,
    }),
);
assert.commandWorked(
    db.adminCommand({
        moveChunk: coll.getFullName(),
        find: {shardKey: 1},
        to: st.shard1.shardName,
        _waitForDelete: true,
    }),
);

// Create a document on shard0.
assert.commandWorked(coll.insert({_id: 1, shardKey: -1}));

const runUpdateWithRetryableWrites = () => {
    const session = db.getMongo().startSession({retryWrites: true});
    const result = session.getDatabase(dbName)[collName].update({_id: 1}, {$set: {shardKey: 1}});
    session.endSession();
    return result;
};

// Enable failpoint on mongos that makes non-shard key updates fail with 'InterruptedAtShutdown' error.
const fp = configureFailPoint(st.s, "updateChangeOwningShardThrowsInterruptedAtShutdown");
assert.commandFailedWithCode(runUpdateWithRetryableWrites(), [ErrorCodes.HostUnreachable]);
fp.off();

// After disabling the failpoint, the command should succeed.
const res = runUpdateWithRetryableWrites();
assert.eq(res, {nMatched: 1, nUpserted: 0, nModified: 1});

st.stop();

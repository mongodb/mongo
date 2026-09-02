/**
 * Test that a findAndModify that updates the shard key and moves a document across shards does
 * not fail when the server is already in shutdown.
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";
import {assertCreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";

const dbName = jsTestName();
const collName = jsTestName();

const st = new ShardingTest({shards: 2});

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

const runFindAndModifyWithRetryableWrites = () => {
    const session = db.getMongo().startSession({retryWrites: true});
    const result = session.getDatabase(dbName)[collName].findAndModify({
        query: {_id: 1},
        update: {$set: {shardKey: 1}},
        new: true,
    });
    session.endSession();
    return result;
};

// Enable failpoint on mongos that makes findAndModify fail with 'InterruptedAtShutdown' error and
// run the findAndModify command. This is supposed to fail.
const fp = configureFailPoint(st.s, "findAndModifyChangeOwningShardThrowsInterruptedAtShutdown");
try {
    runFindAndModifyWithRetryableWrites();
    assert(false, "findAndModify command should have failed");
} catch (e) {
    // Not using 'assert.commandFailedWithCode()' because 'collection.findAndModify()' immediately
    // throws an exception.
    assert.eq(e.code, ErrorCodes.HostUnreachable);
}
fp.off();

// After disabling the failpoint, the findAndModify command should succeed.
const res = runFindAndModifyWithRetryableWrites();
assert.eq(res, {"_id": 1, "shardKey": 1});

st.stop();

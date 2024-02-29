/**
 * Tests updateOne with id without shard key uses PM-3190 for retryable
 * writes and doesn't for transactions or non-retryable writes.
 *
 * @tags: [featureFlagUpdateOneWithIdWithoutShardKey, requires_fcv_73]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {CreateShardedCollectionUtil} from "jstests/sharding/libs/create_sharded_collection_util.js";

const st = new ShardingTest({shards: 2, mongos: 1});
const mongos = st.s0;
let db = mongos.getDB(jsTestName());

const coll = db.coll;
coll.drop();

CreateShardedCollectionUtil.shardCollectionWithChunks(coll, {x: 1}, [
    {min: {x: MinKey}, max: {x: 0}, shard: st.shard0.shardName},
    {min: {x: 0}, max: {x: MaxKey}, shard: st.shard1.shardName},
]);

// Write two documents.
assert.commandWorked(coll.insert({x: -1, _id: -1}));
assert.commandWorked(coll.insert({x: 1, _id: 1}));

let fp = configureFailPoint(st.s, 'hangAfterCompletingWriteWithoutShardKeyWithId');

// Test that transactions do not use broadcast protocol per PM-3190.
let session = st.s.startSession({retryWrites: false});
session.startTransaction();

let sessionColl = session.getDatabase(db.getName()).getCollection(coll.getName());
let updateCmd = {
    updates: [
        {q: {_id: -1}, u: {$inc: {counter: 1}}},
    ],
    txnNumber: NumberLong(0),
};
assert.commandWorked(sessionColl.runCommand("update", updateCmd));

session.commitTransaction();
session.endSession();

// Test that retryable internal transactions do not use broadcast protocol per PM-3190.
const lsidWithUUID2 = {
    id: UUID(),
    txnUUID: UUID()
};
assert.commandWorked(db.runCommand({
    update: coll.getName(),
    updates: [{q: {_id: -1}, u: {$inc: {counter: 1}}}],
    lsid: lsidWithUUID2,
    txnNumber: NumberLong(1),
    startTransaction: true,
    autocommit: false
}));

// Test that retryable transactions do not use broadcast protocol per PM-3190.
session = st.s.startSession({retryWrites: true});
session.startTransaction();

sessionColl = session.getDatabase(db.getName()).getCollection(coll.getName());
updateCmd = {
    updates: [
        {q: {_id: 1}, u: {$inc: {counter: 1}}},
    ],
    txnNumber: NumberLong(0),
};

assert.commandWorked(sessionColl.runCommand("update", updateCmd));
session.commitTransaction();
session.endSession();

// Test that non-retryable writes do not use broadcast protocol per PM-3190.
assert.commandWorked(coll.updateOne({_id: 1}, {$inc: {counter: 1}}));

// Test that non-retryable write sessions do not use broadcast protocol per PM-3190.
session = st.s.startSession({retryWrites: false});

sessionColl = session.getDatabase(db.getName()).getCollection(coll.getName());
updateCmd = {
    updates: [
        {q: {_id: 1}, u: {$inc: {counter: 1}}},
    ]
};

assert.commandWorked(sessionColl.runCommand("update", updateCmd));
session.endSession();

// Test that retryable writes use broadcast protocol per PM-3190
session = st.s.startSession({retryWrites: true});
const lsid = session.getSessionId();

const joinUpdate = startParallelShell(
    funWithArgs(function(dbName, collName, lsid) {
        assert.commandWorked(db.getSiblingDB(dbName).getCollection(collName).runCommand("update", {
            updates: [
                {q: {_id: 1}, u: {$inc: {counter: 1}}},
            ],
            lsid: lsid,
            txnNumber: NumberLong(5)
        }));
    }, db.getName(), coll.getName(), lsid), mongos.port);

// We should hit the configured failpoint if PM-3190 code is used.
fp.wait();
fp.off();
joinUpdate();

session.endSession();

st.stop();

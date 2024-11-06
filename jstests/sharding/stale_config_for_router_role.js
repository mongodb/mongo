/*
 * Tests to validate the correct handling of StaleConfig exceptions when the shard enter in the
 * router role.
 */

import {withRetryOnTransientTxnError} from "jstests/libs/auto_retry_transaction_in_sharding.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({shards: 3});

const dbName = "test";
const localColl = "local";
const foreignColl = "foreign";
const localNs = dbName + "." + localColl;
const foreignNs = dbName + "." + foreignColl;

const lookupPipeline = [
    {$lookup: {from: foreignColl,
                let : {localVar: "$_id"},
                pipeline: [{$match : {"_id": {$gte: -5}}}],
                as: "result"}},
    {$sort: {"_id": 1}}
];

const db = st.s.getDB(dbName);

assert.commandWorked(
    st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));

// Create a sharded collection, "local", with one chunk placed in shard0.
assert.commandWorked(db[localColl].insert({_id: 0}));
assert.commandWorked(st.s.adminCommand({shardCollection: localNs, key: {_id: 1}}));

// Create a sharded collection, "foreign", with one chunk placed in shard0.
assert.commandWorked(db[foreignColl].insert({_id: 0}));
assert.commandWorked(st.s.adminCommand({shardCollection: foreignNs, key: {_id: 1}}));

{
    jsTestLog("Remote $lookup with a stale routing information inside a txn.");

    (function setUp() {
        // Move all chunks from shard0 to shard2 for the foreignNs, making the routing information
        // from shard0 think that shard1 still contains chunks.
        assert.commandWorked(
            st.s.adminCommand({moveChunk: foreignNs, find: {_id: 0}, to: st.shard1.shardName}));
        assert.commandWorked(
            st.s.adminCommand({moveChunk: foreignNs, find: {_id: 0}, to: st.shard2.shardName}));
    })();

    const session = st.s.startSession();

    withRetryOnTransientTxnError(
        () => {
            session.startTransaction();
            assert.commandWorked(session.getDatabase(dbName).runCommand(
                {aggregate: localColl, pipeline: lookupPipeline, cursor: {}}));
            session.abortTransaction();
        },
        () => {
            session.abortTransaction_forTesting();
        });
}

{
    jsTestLog("Remote $lookup with a stale routing information.");

    (function setUp() {
        // The routing information from shard0 is now up-to-date, because the previous test case has
        // refreshed the filtering metadata and routing information. Let's force shard0 to be stale
        // again by moving chunks outside shard2.
        assert.commandWorked(
            st.s.adminCommand({moveChunk: foreignNs, find: {_id: 1}, to: st.shard1.shardName}));
    })();

    assert.commandWorked(
        db.runCommand({aggregate: localColl, pipeline: lookupPipeline, cursor: {}}));
}

{
    jsTestLog("Local $lookup with a stale routing information.");

    (function setUp() {
        // First, shard0 needs to acknowledge that owns a chunk in the routing information.
        assert.commandWorked(
            st.s.adminCommand({moveChunk: foreignNs, find: {_id: 1}, to: st.shard0.shardName}));
        assert.commandWorked(
            db.runCommand({aggregate: localColl, pipeline: lookupPipeline, cursor: {}}));

        // After that, let's move chunks outside shard0, so next time we run the aggregation, shard0
        // still think it owns chunks, and try to make a local $lookup.
        assert.commandWorked(
            st.s.adminCommand({moveChunk: foreignNs, find: {_id: 1}, to: st.shard1.shardName}));
    })();

    assert.commandWorked(
        db.runCommand({aggregate: localColl, pipeline: lookupPipeline, cursor: {}}));
}

{
    jsTestLog("Local $lookup with a stale routing information inside a txn.");

    const session = st.s.startSession();

    (function setUp() {
        // First, shard0 needs to acknowledge that owns a chunk in the routing information.
        assert.commandWorked(
            st.s.adminCommand({moveChunk: foreignNs, find: {_id: 1}, to: st.shard0.shardName}));
        assert.commandWorked(
            db.runCommand({aggregate: localColl, pipeline: lookupPipeline, cursor: {}}));

        // After that, let's move chunks outside shard0, so next time we run the aggregation, shard0
        // still think it owns chunks, and try to make a local $lookup.
        assert.commandWorked(
            st.s.adminCommand({moveChunk: foreignNs, find: {_id: 1}, to: st.shard1.shardName}));
    })();

    withRetryOnTransientTxnError(
        () => {
            session.startTransaction();
            assert.commandWorked(session.getDatabase(dbName).runCommand(
                {aggregate: localColl, pipeline: lookupPipeline, cursor: {}}));
            session.abortTransaction();
        },
        () => {
            session.abortTransaction_forTesting();
        });
}

st.stop();

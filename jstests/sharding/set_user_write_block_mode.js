/**
 * Tests sharding specific functionality of the setUserWriteBlockMode command. Non sharding specific
 * aspects of this command should be checked on jstests/noPassthrough/set_user_write_block_mode.js
 * instead.
 *
 * @tags: [
 *   requires_fcv_60,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {removeShard} from "jstests/sharding/libs/remove_shard_util.js";
import {ShardedIndexUtil} from "jstests/sharding/libs/sharded_index_util.js";

const st = new ShardingTest({shards: 2});

const dbName = "test";
const collName = "foo";
const ns = dbName + "." + collName;

assert.commandWorked(
    st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));

let db = st.s.getDB(dbName);
let coll = db[collName];

const newShardName = 'newShard';
const newShard = new ReplSetTest({name: newShardName, nodes: 1});
newShard.startSet({shardsvr: ''});
newShard.initiate();

// Test addShard sets the proper user writes blocking state on the new shard.
{
    // Create a collection on the new shard before adding it to the cluster.
    const newShardDB = 'newShardDB';
    const newShardColl = 'newShardColl';
    const newShardCollDirect = newShard.getPrimary().getDB(newShardDB).getCollection(newShardColl);
    assert.commandWorked(newShardCollDirect.insert({x: 1}));
    const newShardCollMongos = st.s.getDB(newShardDB).getCollection(newShardColl);

    // Start blocking user writes.
    assert.commandWorked(st.s.adminCommand({setUserWriteBlockMode: 1, global: true}));

    // Add a new shard.
    assert.commandWorked(st.s.adminCommand({addShard: newShard.getURL(), name: newShardName}));

    // Check that we cannot write on the new shard.
    assert.commandFailedWithCode(newShardCollMongos.insert({x: 2}), ErrorCodes.UserWritesBlocked);

    // Now unblock and check we can write to the new shard.
    assert.commandWorked(st.s.adminCommand({setUserWriteBlockMode: 1, global: false}));
    assert.commandWorked(newShardCollMongos.insert({x: 2}));

    // Block again and see we can remove the shard even when write blocking is enabled. Before
    // removing the shard we first need to drop the dbs for which 'newShard' is the db-primary
    // shard.
    assert.commandWorked(st.s.getDB(newShardDB).dropDatabase());
    assert.commandWorked(st.s.adminCommand({setUserWriteBlockMode: 1, global: true}));
    removeShard(st, newShardName);

    // Disable write blocking while 'newShard' is not part of the cluster.
    assert.commandWorked(st.s.adminCommand({setUserWriteBlockMode: 1, global: false}));

    // Add the shard back and check that user write blocking is disabled.
    assert.commandWorked(st.s.adminCommand({addShard: newShard.getURL(), name: newShardName}));
    assert.commandWorked(newShardCollDirect.insert({x: 10}));
}

// Test addShard serializes with setUserWriteBlockMode.
{
    // Start setUserWriteBlockMode and make it hang during the SetUserWriteBlockModeCoordinator
    // execution.
    let hangInShardsvrSetUserWriteBlockModeFailPoint =
        configureFailPoint(st.shard0, "hangInShardsvrSetUserWriteBlockMode");
    let awaitShell = startParallelShell(() => {
        assert.commandWorked(db.adminCommand({setUserWriteBlockMode: 1, global: true}));
    }, st.s.port);
    hangInShardsvrSetUserWriteBlockModeFailPoint.wait();

    assert.commandFailedWithCode(
        st.s.adminCommand({addShard: newShard.getURL(), name: newShardName, maxTimeMS: 1000}),
        ErrorCodes.MaxTimeMSExpired);

    hangInShardsvrSetUserWriteBlockModeFailPoint.off();
    awaitShell();

    assert.commandWorked(st.s.adminCommand({addShard: newShard.getURL(), name: newShardName}));
    assert.commandWorked(st.s.adminCommand({removeShard: newShardName}));

    assert.commandWorked(st.s.adminCommand({setUserWriteBlockMode: 1, global: false}));
}

// Test chunk migrations work even if user writes are blocked
{
    coll.drop();
    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));
    // Insert a document to the chunk that will be migrated to ensure that the recipient will at
    // least insert one document as part of the migration.
    coll.insert({_id: 1});

    // Create an index to check that the recipient, upon receiving its first chunk, can create it.
    const indexKey = {x: 1};
    assert.commandWorked(coll.createIndex(indexKey));

    // Start blocking user writes.
    assert.commandWorked(st.s.adminCommand({setUserWriteBlockMode: 1, global: true}));

    // Move one chunk to shard1.
    assert.commandWorked(st.s.adminCommand({split: ns, middle: {_id: 0}}));
    assert.commandWorked(
        st.s.adminCommand({moveChunk: ns, find: {_id: 0}, to: st.shard1.shardName}));

    assert.eq(1, coll.find({_id: 1}).itcount());
    assert.eq(1, st.shard1.getDB(dbName)[collName].find({_id: 1}).itcount());

    // Check that the index has been created on recipient.
    ShardedIndexUtil.assertIndexExistsOnShard(st.shard1, dbName, collName, indexKey);

    // Check that orphans are deleted from the donor.
    assert.soon(() => {
        return st.shard0.getDB(dbName)[collName].find({_id: 1}).itcount() === 0;
    });

    // Create an extra index on shard1. This index is not present in the shard0, thus shard1 will
    // drop it when it receives its first chunk.
    {
        // Leave shard1 without any chunk.
        assert.commandWorked(
            st.s.adminCommand({moveChunk: ns, find: {_id: 0}, to: st.shard0.shardName}));

        // Create an extra index on shard1.
        assert.commandWorked(st.s.adminCommand({setUserWriteBlockMode: 1, global: false}));
        const extraIndexKey = {y: 1};
        assert.commandWorked(st.shard1.getDB(dbName)[collName].createIndex(extraIndexKey));
        assert.commandWorked(st.s.adminCommand({setUserWriteBlockMode: 1, global: true}));

        // Move a chunk to shard1.
        assert.commandWorked(
            st.s.adminCommand({moveChunk: ns, find: {_id: 0}, to: st.shard1.shardName}));

        // Check the mismatched index was dropped.
        ShardedIndexUtil.assertIndexDoesNotExistOnShard(st.shard1, dbName, collName, extraIndexKey);
    }

    assert.commandWorked(st.s.adminCommand({setUserWriteBlockMode: 1, global: false}));
}

// Test movePrimary works while user writes are blocked.
{
    // Create an unsharded collection so that its data needs to be cloned to the new db-primary.
    const unshardedCollName = 'unshardedColl';
    const unshardedColl = db[unshardedCollName];
    assert.commandWorked(unshardedColl.createIndex({x: 1}));
    unshardedColl.insert({x: 1});

    // Start blocking user writes
    assert.commandWorked(st.s.adminCommand({setUserWriteBlockMode: 1, global: true}));

    const fromShard = st.getPrimaryShard(dbName);
    const toShard = st.getOther(fromShard);
    assert.commandWorked(st.s.adminCommand({movePrimary: dbName, to: toShard.name}));

    // Check that the new primary has cloned the data. The data will only be moved if the collection
    // is untracked.
    const isTrackUnshardedDisabled = !FeatureFlagUtil.isPresentAndEnabled(
        st.s.getDB('admin'), "TrackUnshardedCollectionsUponCreation");
    if (isTrackUnshardedDisabled) {
        assert.eq(1, toShard.getDB(dbName)[unshardedCollName].find().itcount());
    }

    if (isTrackUnshardedDisabled) {
        // Check that the collection has been removed from the former primary.
        assert.eq(0,
                  fromShard.getDB(dbName)
                      .runCommand({listCollections: 1, filter: {name: unshardedCollName}})
                      .cursor.firstBatch.length);
    } else {
        // Check that the database primary has been changed to the new primary.
        assert.eq(1,
                  st.s.getDB("config")
                      .getCollection("databases")
                      .find({_id: dbName, primary: toShard.shardName})
                      .itcount());
    }

    assert.commandWorked(st.s.adminCommand({setUserWriteBlockMode: 1, global: false}));
}

// Test setAllowMigrations works while user writes are blocked.
{
    coll.drop();
    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));

    // Start blocking user writes.
    assert.commandWorked(st.s.adminCommand({setUserWriteBlockMode: 1, global: true}));

    // Disable migrations for 'ns'.
    assert.commandWorked(st.s.adminCommand({setAllowMigrations: ns, allowMigrations: false}));

    const fromShard = st.getPrimaryShard(dbName);
    const toShard = st.getOther(fromShard);
    assert.commandFailedWithCode(
        st.s.adminCommand({moveChunk: ns, find: {_id: 0}, to: toShard.shardName}),
        ErrorCodes.ConflictingOperationInProgress);

    // Reenable migrations for 'ns'.
    assert.commandWorked(st.s.adminCommand({setAllowMigrations: ns, allowMigrations: true}));

    assert.commandWorked(st.s.adminCommand({setUserWriteBlockMode: 1, global: false}));
}

{
    const db2Name = 'db2';
    assert.commandWorked(
        st.s.adminCommand({enableSharding: db2Name, primaryShard: st.shard0.shardName}));
    const db2 = st.s.getDB(db2Name);

    let lsid = assert.commandWorked(st.s.getDB("admin").runCommand({startSession: 1})).id;

    // Send _shardsvrSetUserWriteBlockMode commands to directly to shard0 so that it starts blocking
    // user writes.
    assert.commandWorked(st.shard0.adminCommand({
        _shardsvrSetUserWriteBlockMode: 1,
        global: true,
        phase: 'prepare',
        lsid: lsid,
        txnNumber: NumberLong(1),
        writeConcern: {w: "majority"}
    }));
    assert.commandWorked(st.shard0.adminCommand({
        _shardsvrSetUserWriteBlockMode: 1,
        global: true,
        phase: 'complete',
        lsid: lsid,
        txnNumber: NumberLong(2),
        writeConcern: {w: "majority"}
    }));

    // Check shard0 is now blocking writes.
    assert.commandFailed(db2.bar.insert({x: 1}));

    // Send _shardsvrSetUserWriteBlockMode commands to directly to shard0 so that it stops blocking
    // user writes.
    assert.commandWorked(st.shard0.adminCommand({
        _shardsvrSetUserWriteBlockMode: 1,
        global: false,
        phase: 'prepare',
        lsid: lsid,
        txnNumber: NumberLong(3),
        writeConcern: {w: "majority"}
    }));
    assert.commandWorked(st.shard0.adminCommand({
        _shardsvrSetUserWriteBlockMode: 1,
        global: false,
        phase: 'complete',
        lsid: lsid,
        txnNumber: NumberLong(4),
        writeConcern: {w: "majority"}
    }));

    // Check shard0 is no longer blocking writes.
    assert.commandWorked(db2.bar.insert({x: 1}));

    // Replay the first two _shardsvrSetUserWriteBlockMode commands (as if it was due to a duplicate
    // network packet). These messages should fail to process, so write blocking should not be
    // re-enabled.
    assert.commandFailedWithCode(st.shard0.adminCommand({
        _shardsvrSetUserWriteBlockMode: 1,
        global: true,
        phase: 'prepare',
        lsid: lsid,
        txnNumber: NumberLong(1),
        writeConcern: {w: "majority"}
    }),
                                 ErrorCodes.TransactionTooOld);
    assert.commandFailedWithCode(st.shard0.adminCommand({
        _shardsvrSetUserWriteBlockMode: 1,
        global: true,
        phase: 'complete',
        lsid: lsid,
        txnNumber: NumberLong(2),
        writeConcern: {w: "majority"}
    }),
                                 ErrorCodes.TransactionTooOld);

    // Check shard0 is not blocking writes.
    assert.commandWorked(db2.bar.insert({x: 2}));
}

st.stop();
newShard.stopSet();

/**
 * Tests that change streams is able to find and return results when transition to or from dedicated
 * config server.
 *
 * @tags: [
 *   requires_fcv_80,
 *   requires_majority_read_concern,
 *   uses_change_streams,
 * ]
 */

import {ChangeStreamTest, ChangeStreamWatchMode} from "jstests/libs/change_stream_util.js";
import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {ConfigShardUtil} from "jstests/libs/config_shard_util.js";
import {
    moveDatabaseAndUnshardedColls
} from "jstests/sharding/libs/move_database_and_unsharded_coll_helper.js";

const st = new ShardingTest({
    shards: 2,
    configShard: true,
    rs: {nodes: 2, setParameter: {periodicNoopIntervalSecs: 1, writePeriodicNoops: true}},
    other: {enableBalancer: true},
});

const db = st.s.getDB("test");
const collName = "coll";
const nDocs = 100;
const configShard = st.config0;
const otherShard = st.shard0.shardName === 'config' ? st.shard1 : st.shard0;

function runTest(coll, isConfigShard, watchMode) {
    // Open a changeStream.
    const cst = new ChangeStreamTest(ChangeStreamTest.getDBForChangeStream(watchMode, db));
    let changeStream = cst.getChangeStream({watchMode: watchMode, coll: coll});

    // Write some documents that will end up on each shard. Use a bulk write to increase the chance
    // that two of the writes get the same cluster time on each shard.
    const bulk = coll.initializeUnorderedBulkOp();
    const kIds = [];
    for (let i = 0; i < nDocs / 2; i++) {
        kIds.push(i);
        bulk.insert({_id: i});
        kIds.push(i + nDocs / 2);
        bulk.insert({_id: i + nDocs / 2});
    }
    assert.commandWorked(bulk.execute({w: "majority"}));

    const firstChange = cst.getOneChange(changeStream);

    // Transition to/from dedicated config server to read events after that transition.
    if (isConfigShard) {
        moveDatabaseAndUnshardedColls(db, otherShard.shardName);
        ConfigShardUtil.transitionToDedicatedConfigServer(st);
    } else {
        assert.commandWorked(st.s.adminCommand({transitionFromDedicatedConfigServer: 1}));
    }

    // Read the remaining documents from the original stream.
    const docsFoundInOrder = [firstChange];
    for (let i = 0; i < nDocs - 1; i++) {
        const change = cst.getOneChange(changeStream);
        assert.docEq({db: db.getName(), coll: coll.getName()}, change.ns);
        assert.eq(change.operationType, "insert");

        docsFoundInOrder.push(change);
    }

    // Assert that we found the documents we inserted (in any order).
    assert.setEq(new Set(kIds), new Set(docsFoundInOrder.map(doc => doc.fullDocument._id)));

    // TODO (SERVER-90358): Test change stream using a resume token from the first change.
}

for (let key of Object.keys(ChangeStreamWatchMode)) {
    const watchMode = ChangeStreamWatchMode[key];

    {
        jsTestLog("Running test for a unsplittable collection placed on config shard for mode " +
                  watchMode + " with a transition to dedicated config server");

        // Create an unsplittable collection placed on the config shard.
        assert.commandWorked(db.dropDatabase());
        assert.commandWorked(
            db.adminCommand({enableSharding: db.getName(), primaryShard: configShard.shardName}));

        const coll = assertDropAndRecreateCollection(db, collName);
        runTest(coll, true /* isConfigShard */, watchMode);
    }

    {
        jsTestLog("Running test for a unsplittable collection placed on config shard for mode " +
                  watchMode + " with a transition from dedicated config server");

        const coll = assertDropAndRecreateCollection(db, collName);
        runTest(coll, false /* isConfigShard */, watchMode);
    }

    {
        jsTestLog("Running test for a sharded collection for mode " + watchMode +
                  " with a transition to dedicated config server");

        assert.commandWorked(db.dropDatabase());
        assert.commandWorked(
            db.adminCommand({enableSharding: db.getName(), primaryShard: configShard.shardName}));

        // Create a sharded collection with 2 chunks placed on each shard.
        const coll = assertDropAndRecreateCollection(db, collName);
        st.shardColl(
            coll,
            {_id: 1},              // key
            {_id: nDocs / 2},      // split
            {_id: nDocs / 2 + 1},  // move
            db.getName(),          // dbName
            false                  // waitForDelete
        );

        runTest(coll, true /* isConfigShard */, watchMode);
    }

    {
        jsTestLog("Running test for a sharded collection for mode " + watchMode +
                  " with a transition from dedicated config server");

        // Create a sharded collection with 2 chunks placed on each shard.
        const coll = assertDropAndRecreateCollection(db, collName);
        st.shardColl(
            coll,
            {_id: 1},              // key
            {_id: nDocs / 2},      // split
            {_id: nDocs / 2 + 1},  // move
            db.getName(),          // dbName
            false                  // waitForDelete
        );
        runTest(coll, false /* isConfigShard */, watchMode);
    }
}

st.stop();

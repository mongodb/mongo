/**
 * This test checks the 'n'/'nModified' values reported by mongos when retrying updates/deletes
 * by _id without a shard key after a chunk migration.
 *
 * @tags: [requires_fcv_80]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";
import {CreateShardedCollectionUtil} from "jstests/sharding/libs/create_sharded_collection_util.js";
import {isUweEnabled} from "jstests/libs/query/uwe_utils.js";

(function () {
    "use strict";

    const st = new ShardingTest({mongos: 1, config: 1, shards: 2, rs: {nodes: 1}});

    const db = st.s.getDB("test");
    const collection = db.getCollection("mycoll");
    const uweEnabled = isUweEnabled(st.s);

    CreateShardedCollectionUtil.shardCollectionWithChunks(collection, {x: 1}, [
        {min: {x: MinKey}, max: {x: 0}, shard: st.shard0.shardName},
        {min: {x: 0}, max: {x: 10}, shard: st.shard0.shardName},
        {min: {x: 10}, max: {x: MaxKey}, shard: st.shard1.shardName},
    ]);

    for (let i = 0; i < 5; i++) {
        assert.commandWorked(collection.insert({_id: i, x: 5, counter: 0}));
    }

    const sessionColl = st.s
        .startSession({causalConsistency: false, retryWrites: false})
        .getDatabase(db.getName())
        .getCollection(collection.getName());

    const updateCmdSingleOp = {
        updates: [{q: {_id: 0}, u: {$inc: {counter: 1}}}],
        ordered: true,
        txnNumber: NumberLong(0),
    };

    const deleteCmdSingleOp = {
        deletes: [{q: {_id: 0}, limit: 1}],
        ordered: true,
        txnNumber: NumberLong(1),
    };

    const updateCmdOrdered = {
        updates: [
            {q: {_id: 1}, u: {$inc: {counter: 1}}},
            {q: {_id: 2}, u: {$inc: {counter: 1}}},
        ],
        ordered: true,
        txnNumber: NumberLong(2),
    };

    const deleteCmdOrdered = {
        deletes: [
            {q: {_id: 1}, limit: 1},
            {q: {_id: 2}, limit: 1},
        ],
        ordered: true,
        txnNumber: NumberLong(3),
    };

    const updateCmdUnordered = {
        updates: [
            {q: {_id: 3}, u: {$inc: {counter: 1}}},
            {q: {_id: 4}, u: {$inc: {counter: 1}}},
        ],
        ordered: false,
        txnNumber: NumberLong(4),
    };

    const deleteCmdUnordered = {
        deletes: [
            {q: {_id: 3}, limit: 1},
            {q: {_id: 4}, limit: 1},
        ],
        ordered: false,
        txnNumber: NumberLong(5),
    };

    function runUpdateTwice(cmdObj, coll, shard0, shard1, firstExp, secondExp) {
        const firstRes = assert.commandWorked(coll.runCommand("update", cmdObj));
        assert.eq(firstRes.n, firstExp);
        assert.eq(firstRes.nModified, firstExp);

        assert.commandWorked(db.adminCommand({moveChunk: coll.getFullName(), find: {x: 5}, to: shard1}));

        const secondRes = assert.commandWorked(coll.runCommand("update", cmdObj));
        assert.eq(secondRes.n, secondExp);
        assert.eq(secondRes.nModified, secondExp);

        assert.commandWorked(db.adminCommand({moveChunk: coll.getFullName(), find: {x: 5}, to: shard0}));
    }

    function runDeleteTwice(cmdObj, coll, shard0, shard1, firstExp, secondExp) {
        const firstRes = assert.commandWorked(coll.runCommand("delete", cmdObj));
        assert.eq(firstRes.n, firstExp);

        assert.commandWorked(db.adminCommand({moveChunk: coll.getFullName(), find: {x: 5}, to: shard1}));

        const secondRes = assert.commandWorked(coll.runCommand("delete", cmdObj));
        assert.eq(secondRes.n, secondExp);

        assert.commandWorked(db.adminCommand({moveChunk: coll.getFullName(), find: {x: 5}, to: shard0}));
    }

    const shardName0 = st.shard0.shardName;
    const shardName1 = st.shard1.shardName;

    // Updates/deletes by _id without a shard key are broadcasted to all shards which own chunks for
    // the collection. After the session information is migrated to shard1 from the moveChunk
    // command, both shard0 and shard1 will report {n: 1, nModified: 1} for the retried stmt ids.
    //
    // What happens when this command is retried?
    //
    // If BatchWriteExec or bulk_write_exec is used -AND- if each update/delete by _id executes in a
    // batch by itself (either because ordered=true or due to other circumstances), there is special
    // case logic (see WriteOp::_noteWriteWithoutShardKeyWithIdBatchResponseWithSingleWrite()) that
    // ensures that each op will only count as "1" when generating the response to send to the
    // client.
    //
    // If UWE is used -OR- if ordered=false and multiple updates/deletes by _id executed in a batch
    // together, then the responses from all the shards get summed together, which in this test
    // example results in each op being counted twice.
    //
    // TODO SERVER-54019 Avoid over-counting 'n' and 'nModified' values when retrying updates by _id
    // or deletes by _id after chunk migration.
    let firstExp = 1;
    let secondExp = uweEnabled ? 2 * firstExp : firstExp;
    runUpdateTwice(updateCmdSingleOp, sessionColl, shardName0, shardName1, firstExp, secondExp);
    runDeleteTwice(deleteCmdSingleOp, sessionColl, shardName0, shardName1, firstExp, secondExp);

    firstExp = 2;
    secondExp = uweEnabled ? 2 * firstExp : firstExp;
    runUpdateTwice(updateCmdOrdered, sessionColl, shardName0, shardName1, firstExp, secondExp);
    runDeleteTwice(deleteCmdOrdered, sessionColl, shardName0, shardName1, firstExp, secondExp);

    firstExp = 2;
    secondExp = 2 * firstExp;
    runUpdateTwice(updateCmdUnordered, sessionColl, shardName0, shardName1, firstExp, secondExp);
    runDeleteTwice(deleteCmdUnordered, sessionColl, shardName0, shardName1, firstExp, secondExp);

    st.stop();
})();

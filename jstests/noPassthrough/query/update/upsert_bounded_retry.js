/**
 * When an upsert sends a documents into a partial/sparse unique index, retrying on duplicate key
 * error can result in an infinite loop. This test confirms the retry mechanism is bounded.
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

function runOnReplsetAndShardedCluster(callbackFn) {
    {
        const rst = new ReplSetTest({nodes: 3});
        rst.startSet();
        rst.initiate();

        callbackFn(rst.getPrimary());

        rst.stopSet();
    }

    {
        const st = new ShardingTest(Object.assign({shards: 2}));

        const testDB = st.s.getDB("test");
        assert.commandWorked(testDB.adminCommand(
            {enableSharding: testDB.getName(), primaryShard: st.shard0.shardName}));

        callbackFn(st.s);

        st.stop();
    }
}

function main(conn) {
    const db = conn.getDB("test");

    // Partial index, non-transactional upsert and findAndModify.
    //
    assert(db.test.drop());
    assert.commandWorked(
        db.test.createIndex({userId: 1}, {unique: true, partialFilterExpression: {indexed: true}}));
    assert.commandWorked(db.test.insert({userId: 1}));
    assert.commandWorked(db.test.insert({userId: 1, indexed: true}));
    assert.writeError(db.test.update({userId: 1}, {$set: {indexed: true}}, {upsert: true}));
    assert.throwsWithCode(() => db.test.findAndModify({
        query: {userId: 1},
        update: {$set: {userId: 1, indexed: true}},
        upsert: true,
    }),
                          ErrorCodes.DuplicateKey);

    // Sparse index, non-transactional upsert and findAndModify.
    //
    assert(db.test.drop());
    assert.commandWorked(db.test.createIndex({userId: 1}, {unique: true, sparse: true}));
    assert.commandWorked(db.test.insert({}));
    assert.commandWorked(db.test.insert({userId: null}));
    assert.writeError(db.test.update({userId: null}, {$set: {userId: null}}, {upsert: true}));
    assert.throwsWithCode(() => db.test.findAndModify({
        query: {userId: null},
        update: {$set: {userId: null}},
        upsert: true,
    }),
                          ErrorCodes.DuplicateKey);
}

runOnReplsetAndShardedCluster(main);

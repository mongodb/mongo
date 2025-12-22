/**
 * Tests that updates to a shard key work on documents that has dollar-prefixed fields.
 * @tags: [
 *  requires_fcv_83,
 * ]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({
    mongos: 1,
    shards: {rs0: {nodes: 1}, rs1: {nodes: 1}},
});

const coll = st.s.getDB("test").getCollection(jsTestName());

const badDoc = {
    _id: 1,
    shard: 1,
    array: [2, {"$alpha": 3}],
    obj: {"$beta": 4},
    obj2: {"$charlie": 5, "$delta": {"$foxtrot": 6}},
    "$golf": 7,
    "$hotel": {"$india": {"$juliett": 9}},
    obj3: {subobj: {"$kilo": 10}},
    "$mike": [11, 12],
};

assert.commandWorked(coll.insert([badDoc, {_id: 2, shard: 3, x: 2}]));

const shardKey = {
    shard: 1,
};
assert.commandWorked(coll.createIndex(shardKey));
assert.commandWorked(st.s.adminCommand({shardCollection: coll.getFullName(), key: shardKey}));

assert.commandWorked(st.s.adminCommand({split: coll.getFullName(), middle: {shard: 2}}));
assert.commandWorked(
    st.s.adminCommand({
        moveChunk: coll.getFullName(),
        find: {shard: 1},
        to: st.shard0.shardName,
        _waitForDelete: true,
    }),
);
assert.commandWorked(
    st.s.adminCommand({
        moveChunk: coll.getFullName(),
        find: {shard: 3},
        to: st.shard1.shardName,
        _waitForDelete: true,
    }),
);

const retrySession = st.s.startSession({retryWrites: true});
const sessionColl = retrySession.getDatabase(coll.getDB().getName()).getCollection(coll.getName());

// TODO SERVER-114994 findAndModify support for UWE.
assert.docEq(badDoc, sessionColl.findAndModify({query: {_id: 1}, update: {$set: {shard: 2}}}));
assert.docEq({shard: 2}, sessionColl.findOne({_id: 1}, {_id: 0, shard: 1}));

st.stop();

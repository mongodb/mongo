/**
 * This is the test for $documents stage in aggregation pipeline on a sharded collection.
 * @tags: [ requires_fcv_72 ]
 */

import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

let st = new ShardingTest({shards: 2});

const db = st.s.getDB("sharded_documents_stage");
var admin = st.s.getDB("admin");

const coll = db.test_coll;
assert.commandWorked(coll.insert({_id: 0, x: 1, y: 1}));
assert.commandWorked(coll.insert({_id: 1, x: 2, y: 1}));
assert.commandWorked(coll.insert({_id: 2, x: 3, y: 1}));

assert.commandWorked(admin.runCommand({enableSharding: coll.getDB() + ""}));
assert.commandWorked(admin.runCommand({shardCollection: coll + "", key: {_id: 1}}));
assert.commandWorked(admin.runCommand({split: coll + "", middle: {_id: 1}}));
assert.commandWorked(admin.runCommand(
    {moveChunk: coll + "", find: {_id: 1}, to: st.shard1.shardName, _waitForDelete: true}));
assert.commandWorked(admin.runCommand(
    {moveChunk: coll + "", find: {_id: 0}, to: st.shard0.shardName, _waitForDelete: true}));

assert.doesNotThrow(
    () => db.aggregate(
        [{$documents: Array.from({length: 20}, () => ({}))}, {$out: 'test_collection'}]));
assert.eq(db.test_collection.count(), 20);

assert.doesNotThrow(
    () => db.aggregate(
        [{$documents: Array.from({length: 20}, () => ({}))}, {$merge: 'test_collection'}]));
assert.eq(db.test_collection.count(), 40);

let expectedDocsForPipelineLookup = [
    {"_id": 0, "x": 1, "y": 1, "t": [{"x": 1, "z": [1, 2, 3]}]},
    {"_id": 1, "x": 2, "y": 1, "t": [{"x": 2, "z": [1, 2, 3]}]},
    {"_id": 2, "x": 3, "y": 1, "t": [{"x": 3, "z": [1, 2, 3]}]}
];

let expectedDocsForCollectionLookup = [
    {"x": 1, "z": [1, 2, 3], "t": [{"_id": 0, "x": 1, "y": 1}]},
    {"x": 2, "z": [1, 2, 3], "t": [{"_id": 1, "x": 2, "y": 1}]},
    {"x": 3, "z": [1, 2, 3], "t": [{"_id": 2, "x": 3, "y": 1}]}
];

const lookupResult1 =
    coll.aggregate({$lookup: {as: "t",localField: "x", foreignField:"x", pipeline: [{$documents: [{x:1,z: [1, 2, 3]}, {x:2,z: [1, 2, 3]}, {x:3, z: [1, 2, 3]}]}]}});

assertArrayEq({actual: lookupResult1.toArray(), expected: expectedDocsForPipelineLookup});

const lookupResult2 = db.aggregate(
  [{$documents: [{"_id": 0,x: 1, y: 1}, {"_id": 1,x: 2, y: 1}, {"_id": 2,x: 3, y: 1}]}, {$lookup: {as: "t", localField:
  "x", foreignField:"x",pipeline: [{$documents: [{x:1, z: [1, 2, 3]}, {x:2, z: [1, 2, 3]}, {x:3, z:
  [1, 2, 3]}]}]}}]);
assertArrayEq({actual: lookupResult2.toArray(), expected: expectedDocsForPipelineLookup});

const lookupResult3 = db.aggregate([
    {$documents: [{x: 1, z: [1, 2, 3]}, {x: 2, z: [1, 2, 3]}, {x: 3, z: [1, 2, 3]}]},
    {$lookup: {from: "test_coll", as: "t", localField: "x", foreignField: "x"}}
]);
assertArrayEq({actual: lookupResult3.toArray(), expected: expectedDocsForCollectionLookup});

st.stop();

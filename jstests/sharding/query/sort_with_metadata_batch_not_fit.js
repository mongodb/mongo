/**
 * Tests that shards includes metadata for the mongos to merge sort in the correct order, even when documents
 * do not fit into the batch and need to be stashed.
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";

const dbName = "test";
const collName = jsTestName();

const st = new ShardingTest({name: jsTestName(), shards: 2, mongos: 2});
const db = st.s.getDB(dbName);
const coll = db[collName];

st.shardColl(
    collName,
    {_id: 1} /* shard key */,
    {_id: 0} /* split at */,
    {_id: 0} /* move the chunk to its own shard */,
    dbName,
    true /* waitForDelete */,
);
assert.commandWorked(coll.createIndex({s: "text"}));

// Add docs to each shard. Docs are large enough to not fit into a single batch.
// The shards must include textScore metadata for the mongos to merge the docs
// in the correct interleaving orders, i.e. [-2, 0, -1, 1].
// Shard 0
assert.commandWorked(coll.insert({_id: -2, s: "s s s s", b: "b".repeat(15 * 1024 * 1024)}));
assert.commandWorked(coll.insert({_id: -1, s: "s s", b: "b".repeat(15 * 1024 * 1024)}));
// Shard 1
assert.commandWorked(coll.insert({_id: 0, s: "s s s", b: "b".repeat(15 * 1024 * 1024)}));
assert.commandWorked(coll.insert({_id: 1, s: "s", b: "b".repeat(15 * 1024 * 1024)}));

// Find the expected document order and make sure every document gets a different text score.
const expectedDocs = coll
    .find({$text: {$search: "s"}}, {_id: 1, score: {$meta: "textScore"}})
    .toArray()
    .sort((x, y) => y.score - x.score);
assert.eq(expectedDocs.length, new Set(expectedDocs.map((doc) => doc.score)).size);

function verifyDocOrder(res) {
    assert.eq(expectedDocs.length, res.length, res);
    for (let i = 0; i < res.length; ++i) {
        assert.eq(expectedDocs[i]._id, res[i]._id, res);
    }
}

let res = coll
    .find({$text: {$search: "s"}}, {_id: 1})
    .sort({textScore: {$meta: "textScore"}})
    .toArray();
verifyDocOrder(res);

res = coll
    .aggregate([{$match: {$text: {$search: "s"}}}, {$sort: {textScore: {$meta: "textScore"}}}, {$project: {_id: 1}}])
    .toArray();
verifyDocOrder(res);

st.stop();

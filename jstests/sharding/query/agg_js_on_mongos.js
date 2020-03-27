// Performs an aggregation that will execute JavaScript on mongos. This is a sanity check to confirm
// that JavaScript is available on mongos.
// @tags: [requires_fcv_44]
(function() {
"use strict";

const st = new ShardingTest({shards: 2});
const mongos = st.s;

let testDB = mongos.getDB("test");
let coll = testDB.coll;

// Shard the collection. Sharding on a hashed shard key and an empty collection will ensure that
// each shard has chunks.
st.shardColl(coll.getName(), {_id: "hashed"}, false);

const bulk = coll.initializeUnorderedBulkOp();
for (let i = 0; i < 100; ++i) {
    bulk.insert({x: i});
}
assert.commandWorked(bulk.execute());

const pipeline = [
    {$_internalSplitPipeline: {mergeType: "mongos"}},
    {
        $project: {
            y: {
                "$function": {
                    args: ["$_id"],
                    body: function(id) {
                        return id;
                    },
                    lang: "js"
                }
            }
        }
    }
];

// Confirm that an aggregate command with a Javascript expression that is expected to execute on
// mongos succeeds.
assert.commandWorked(testDB.runCommand({aggregate: 'coll', pipeline: pipeline, cursor: {}}));

// Confirm that the same pipeline fails when Javascript has been disabled on mongos.
st.restartMongos(0, {"noscripting": '', "restart": true});
// 'testDB' and 'coll' are no longer valid after mongos restart and must be reassigned.
testDB = st.s.getDB("test");
coll = testDB.coll;

assert.commandFailedWithCode(testDB.runCommand({aggregate: 'coll', pipeline: pipeline, cursor: {}}),
                             31264);

st.stop();
}());

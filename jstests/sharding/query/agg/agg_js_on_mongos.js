// Performs an aggregation that will execute JavaScript on mongos. This is a sanity check to confirm
// that JavaScript is available on mongos.
// @tags: [requires_scripting]

import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({
    shards: 2,
    // By default, our test infrastructure sets the election timeout to a very high value (24
    // hours). This test restarts the router, and if the router is embedded, it will also restart
    // the config shard. In this case, we need a shorter election timeout because the test relies on
    // nodes running an election when they don't detect an active primary. Therefore, we are setting
    // the electionTimeoutMillis to its default value.
    initiateWithDefaultElectionTimeout: jsTestOptions().embeddedRouter
});
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

// Confirm that the same pipeline fails when Javascript has been disabled on the router.
st.restartRouterNode(0, {"noscripting": '', "restart": true});
// 'testDB' and 'coll' are no longer valid after mongos restart and must be reassigned.
testDB = st.s.getDB("test");
coll = testDB.coll;

assert.commandFailedWithCode(testDB.runCommand({aggregate: 'coll', pipeline: pipeline, cursor: {}}),
                             31264);

st.stop();

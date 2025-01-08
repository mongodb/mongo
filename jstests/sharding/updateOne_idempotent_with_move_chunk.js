/**
 * Tests that concurrent retryable updateOne operation with _id without shard key and chunk
 * migration for the chunk being updated doesn't cause zero updates.
 *
 * @tags: [
 *   requires_fcv_80,
 *   # TODO (SERVER-97257): Re-enable this test or add an explanation why it is incompatible.
 *   embedded_router_incompatible,
 * ]
 */
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({shards: 2, mongos: 1, useBridge: true});
const mongos = st.s0;
let db = mongos.getDB(jsTestName());

const coll = db.coll;
const fullCollName = coll.getFullName();
coll.drop();

// Shard the test collection on x.
assert.commandWorked(mongos.adminCommand(
    {enableSharding: coll.getDB().getName(), primaryShard: st.shard0.shardName}));
assert.commandWorked(mongos.adminCommand({shardCollection: fullCollName, key: {x: 1}}));

// Split the collection into 2 chunks: [MinKey, 0), [0, MaxKey].
assert.commandWorked(mongos.adminCommand({split: fullCollName, middle: {x: 0}}));

// Move the [0, MaxKey] chunk to st.shard1.shardName.
assert.commandWorked(
    mongos.adminCommand({moveChunk: fullCollName, find: {x: 1}, to: st.shard1.shardName}));

// Write a document.
assert.commandWorked(coll.insert({x: -1, _id: 0}));

// Delay messages from mongos to shard 0 or shard 1 such that the updateOne to that shard
// reaches post chunk migration from shard 0 to shard 1 below.
const delayMillis = 500;
st.rs0.getPrimary().delayMessagesFrom(st.s, delayMillis);

const cmdObj = {
    update: coll.getName(),
    updates: [
        {q: {_id: 0}, u: {$inc: {counter: 1}}, multi: false},
    ],
    lsid: {id: UUID()},
    txnNumber: NumberLong(5)
};

const joinUpdate = startParallelShell(funWithArgs(function(cmdObj, testName) {
                                          const res = db.getSiblingDB(testName).runCommand(cmdObj);
                                          assert.commandWorked(res);
                                          assert.eq(1, res.nModified, tojson(res));
                                      }, cmdObj, jsTestName()), mongos.port);

const joinMoveChunk = startParallelShell(
    funWithArgs(function(fullCollName, shardName) {
        // Sleep for small duration to ascertain that we don't start
        // moveChunk before an updateOne is received by shard 0 or shard 1
        // depending on the scenario tested.
        sleep(100);
        assert.commandWorked(
            db.adminCommand({moveChunk: fullCollName, find: {x: -1}, to: shardName}));
    }, coll.getFullName(), st.shard1.shardName), mongos.port);

joinMoveChunk();
joinUpdate();

assert.neq(null, coll.findOne({x: -1, counter: 1}));
st.stop();

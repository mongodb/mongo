/**
 * Tests that concurrent retryable updateOne operation with _id without shard key and chunk
 * migration for the chunk being updated doesn't cause zero updates.
 *
 * @tags: [
 *   requires_fcv_80,
 * ]
 */
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {isUweEnabled} from "jstests/libs/query/uwe_utils.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({shards: 2, mongos: 1, useBridge: true});
const mongos = st.s0;
let db = mongos.getDB(jsTestName());

const coll = db.coll;
const fullCollName = coll.getFullName();
coll.drop();

// Shard the test collection on x.
assert.commandWorked(
    mongos.adminCommand({
        enableSharding: coll.getDB().getName(),
        primaryShard: st.shard0.shardName,
    }),
);
assert.commandWorked(mongos.adminCommand({shardCollection: fullCollName, key: {x: 1}}));

// Split the collection into 2 chunks: [MinKey, 0), [0, MaxKey].
assert.commandWorked(mongos.adminCommand({split: fullCollName, middle: {x: 0}}));

// Move the [0, MaxKey] chunk to st.shard1.shardName.
assert.commandWorked(
    mongos.adminCommand({moveChunk: fullCollName, find: {x: 1}, to: st.shard1.shardName}),
);

// Write a document.
assert.commandWorked(coll.insert({x: -1, _id: 0}));

// Delay messages from mongos to shard 0 or shard 1 such that the updateOne to that shard
// reaches post chunk migration from shard 0 to shard 1 below.
const delayMillis = 500;
st.rs0.getPrimary().delayMessagesFrom(st.s, delayMillis);

const cmdObj = {
    update: coll.getName(),
    updates: [{q: {_id: 0}, u: {$inc: {counter: 1}}, multi: false}],
    lsid: {id: UUID()},
    txnNumber: NumberLong(5),
};

// The unified write executor broadcasts a retryable update by _id to every shard in scope and sums
// the per-shard replies without the dedup that BatchWriteExec performs. Once the concurrent
// migration below has copied the session history to the recipient, a retry of the statement is
// counted on both shards and reported as 'nModified: 2'.
// TODO SERVER-54019 Avoid over-counting 'n' and 'nModified' values when retrying updates by _id
// or deletes by _id after chunk migration.
const uweEnabled = isUweEnabled(db);

const joinUpdate = startParallelShell(
    funWithArgs(
        function (cmdObj, testName, uweEnabled) {
            const res = db.getSiblingDB(testName).runCommand(cmdObj);
            assert.commandWorked(res);
            assert.contains(res.nModified, uweEnabled ? [1, 2] : [1], res);
        },
        cmdObj,
        jsTestName(),
        uweEnabled,
    ),
    mongos.port,
);

const joinMoveChunk = startParallelShell(
    funWithArgs(
        function (fullCollName, shardName) {
            // Sleep for small duration to ascertain that we don't start
            // moveChunk before an updateOne is received by shard 0 or shard 1
            // depending on the scenario tested.
            sleep(100);
            assert.commandWorked(
                db.adminCommand({moveChunk: fullCollName, find: {x: -1}, to: shardName}),
            );
        },
        coll.getFullName(),
        st.shard1.shardName,
    ),
    mongos.port,
);

joinMoveChunk();
joinUpdate();

// Regardless of the reported count, the update must have been applied exactly once.
assert.neq(null, coll.findOne({x: -1, counter: 1}));
st.stop();

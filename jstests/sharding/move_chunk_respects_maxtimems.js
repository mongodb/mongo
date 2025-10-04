/**
 * Tests that if maxTimeMS is sent with a moveChunk command, the client thread that issued moveChunk
 * will be interrupted when maxTimeMS is exceeded, but moveChunk will eventually succeed in the
 * background.
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {findChunksUtil} from "jstests/sharding/libs/find_chunks_util.js";

let st = new ShardingTest({shards: 2});

const dbName = "test";
const collName = "foo";
const ns = dbName + "." + collName;
let testDB = st.s.getDB(dbName);
let testColl = testDB.foo;

// Create a sharded collection with one chunk on shard0.
assert.commandWorked(st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {x: 1}}));

// Enable failpoint which will cause moveChunk to hang indefinitely.
let step1Failpoint = configureFailPoint(st.shard0, "moveChunkHangAtStep1");

const awaitResult = startParallelShell(
    funWithArgs(
        function (ns, toShardName) {
            // Send moveChunk with maxTimeMS. We set it to 15 seconds to ensure that the moveChunk
            // command is run and the task to execute the moveChunk logic is launched before maxTimeMS
            // expires. That way we can check below that a maxTimeMS timeout won't fail the migration.
            assert.commandFailedWithCode(
                db.adminCommand({moveChunk: ns, find: {x: 0}, to: toShardName, maxTimeMS: 15000}),
                ErrorCodes.MaxTimeMSExpired,
            );
        },
        ns,
        st.shard1.shardName,
    ),
    st.s.port,
);

awaitResult();
step1Failpoint.off();

jsTestLog("Waiting for moveChunk to succeed in the background");

// The moveChunk should eventually succeed in the background even though the client thread was
// interrupted.
assert.soon(() => {
    let numChunksOnShard0 = findChunksUtil.findChunksByNs(st.config, ns, {shard: st.shard0.shardName}).itcount();
    let numChunksOnShard1 = findChunksUtil.findChunksByNs(st.config, ns, {shard: st.shard1.shardName}).itcount();
    return numChunksOnShard0 == 0 && numChunksOnShard1 == 1;
});

st.stop();

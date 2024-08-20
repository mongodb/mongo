/*
 * Tests to validate the different behaviours of the moveChunk with concurrent index builds
 * creation.
 *
 * @tags: [
 *   requires_fcv_70,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {IndexBuildTest} from "jstests/noPassthrough/libs/index_build.js";
import {CreateShardedCollectionUtil} from "jstests/sharding/libs/create_sharded_collection_util.js";

// Configure initial sharding cluster
const st = new ShardingTest({});
let dbCounter = 0;

function setupCollection() {
    const db = st.s.getDB("test" + dbCounter++);
    const coll = db.coll;

    assert.commandWorked(
        st.s.adminCommand({enableSharding: db.getName(), primaryShard: st.shard0.shardName}));
    CreateShardedCollectionUtil.shardCollectionWithChunks(coll, {x: 1}, [
        {min: {x: MinKey}, max: {x: 1}, shard: st.shard0.shardName},
        {min: {x: 1}, max: {x: MaxKey}, shard: st.shard1.shardName},
    ]);

    return coll;
}

// Test the correct behaviour of the moveChunk when it is not the first migration to a shard, the
// collection is empty and there is an index build in progress. This moveChunk must succeed because
// it will wait for the index build to be finished before completing the migration.
(function testSucceedFirstMigrationWithInProgressIndexBuild() {
    const coll = setupCollection();
    const db = coll.getDB();
    const ns = coll.getFullName();

    // Insert documents to force a two-phase index build
    coll.insert({x: 10});

    // Create new index and pause its build on shard1
    const hangIndexBuildBeforeCommit = configureFailPoint(st.shard1, "hangIndexBuildBeforeCommit");
    const awaitIndexBuild = IndexBuildTest.startIndexBuild(db.getMongo(), ns, {y: 1});
    hangIndexBuildBeforeCommit.wait();

    // Migrate all chunks from shard1 to shard0
    assert.commandWorked(st.s.adminCommand(
        {moveChunk: ns, find: {x: 10}, to: st.shard0.shardName, _waitForDelete: true}));

    // Migrate one chunk from shard0 to shard1 in a parallel shell
    const hangMigrationRecipientBeforeWaitingNoIndexBuildInProgress =
        configureFailPoint(st.shard1, "hangMigrationRecipientBeforeWaitingNoIndexBuildInProgress");
    const shardName = st.shard1.shardName;
    const awaitMoveChunkShell = startParallelShell(
        funWithArgs(function(shardName, ns) {
            const mongos = db.getMongo();
            assert.commandWorked(
                mongos.adminCommand({moveChunk: ns, find: {x: 10}, to: shardName}));
        }, shardName, ns), st.s.port);

    // Wait for the awaiting of the in-progress index builds from the chunk migration
    hangMigrationRecipientBeforeWaitingNoIndexBuildInProgress.wait();
    hangMigrationRecipientBeforeWaitingNoIndexBuildInProgress.off();

    // Finish the index build on shard1
    hangIndexBuildBeforeCommit.off();

    // Finally check for the succeed in the index build and the last move chunk
    awaitIndexBuild();
    awaitMoveChunkShell();
})();

// Test the correct behaviour of the moveChunk when it is the first migration to a shard, the
// collection is not empty and there is an index build in progress. This moveChunk must fail until
// the index build is finished or there is no range deletion.
(function testFailedFirstMigrationWithInProgressIndexBuild() {
    const coll = setupCollection();
    const ns = coll.getFullName();

    // Pause range deletion on shard1
    const suspendRangeDeletion = configureFailPoint(st.shard1, "suspendRangeDeletion");

    // Insert documents to force a two-phase index build
    coll.insert({x: 10});

    // Create new index and pause its build on shard1
    const hangIndexBuildBeforeCommit = configureFailPoint(st.shard1, "hangIndexBuildBeforeCommit");
    const awaitIndexBuild = IndexBuildTest.startIndexBuild(coll.getMongo(), ns, {y: 1});
    hangIndexBuildBeforeCommit.wait();

    // Migrate all chunks from shard1 to shard0
    assert.commandWorked(
        st.s.adminCommand({moveChunk: ns, find: {x: 10}, to: st.shard0.shardName}));

    // Migrate one chunk from shard0 to shard1 and fail on the migration
    const res = st.s.adminCommand({moveChunk: ns, find: {x: -10}, to: st.shard1.shardName});
    assert.commandFailedWithCode(res, ErrorCodes.OperationFailed);
    assert.includes(
        res.errmsg, "Non-trivial index creation should be scheduled manually", tojson(res));

    suspendRangeDeletion.off();
    hangIndexBuildBeforeCommit.off();
    awaitIndexBuild();
})();

// Test the correct behaviour of the moveChunk when it is not the first migration to a shard, the
// collection is not empty and there is an index build in progress. This moveChunk must succeed
// before finishing the index build.
(function testSucceedNotFirstMigrationWithInProgressIndexBuild() {
    const coll = setupCollection();
    const ns = coll.getFullName();

    // Insert documents to force a two-phase index build
    coll.insert({x: 10});

    // Create new index and pause its build on shard1
    const hangIndexBuildBeforeCommit = configureFailPoint(st.shard1, "hangIndexBuildBeforeCommit");
    const awaitIndexBuild = IndexBuildTest.startIndexBuild(coll.getMongo(), ns, {y: 1});
    hangIndexBuildBeforeCommit.wait();

    // Migrate one chunk from shard0 to shard1
    assert.commandWorked(
        st.s.adminCommand({moveChunk: ns, find: {x: -10}, to: st.shard1.shardName}));

    hangIndexBuildBeforeCommit.off();
    awaitIndexBuild();
})();

st.stop();

/**
 * Deterministic reproducer for SERVER-99357: concurrent chunk migration and
 * createIndexes/dropIndexes leave the cluster with an inconsistent index set across shards.
 *
 * The race:
 *  1. The migration recipient takes an index-catalog snapshot from the donor at the start of the
 *     clone phase (`startedMoveChunk`).
 *  2. A router-driven `dropIndexes` (or `createIndexes`) issued in the gap between the snapshot
 *     and the migration commit lands on the donor before the recipient finishes applying the
 *     stale snapshot.
 *  3. After commit, the recipient's index set reflects the pre-drop snapshot while the donor's
 *     reflects the post-drop state -> inconsistent indexes -> future migrations into the
 *     recipient refuse to copy and `removeShard` is wedged.
 *
 * This test pins the bug shape: it wedges migration at the failpoint, runs the index command,
 * unwedges, and then runs the `$indexStats` divergence-detection pipeline from
 * `index_stats_pipeline_detects_inconsistent_indexes.js` to confirm whether the resulting state
 * is consistent. The test PASSES when the fix is in place (consistent across shards). Without
 * the fix the pipeline returns one or more rows describing the divergence and the test fails
 * with a structured message.
 *
 * @tags: [
 *   requires_fcv_80,
 * ]
 */
import {
    moveChunkStepNames,
    pauseMoveChunkAtStep,
    unpauseMoveChunkAtStep,
    waitForMoveChunkStep,
} from "jstests/libs/chunk_manipulation_util.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

// The whole point of the test is to produce, then assert against, an inconsistent index state.
TestData.skipCheckingIndexesConsistentAcrossCluster = true;
// The test deliberately leaves orphans behind on the wedged migration path.
TestData.skipCheckOrphans = true;

const indexStatsDivergencePipeline = [
    {$indexStats: {}},
    {$group: {_id: null, indexDoc: {$push: "$$ROOT"}, allShards: {$addToSet: "$shard"}}},
    {$unwind: "$indexDoc"},
    {
        $group: {
            _id: "$indexDoc.name",
            shards: {$push: "$indexDoc.shard"},
            specs: {$push: {$objectToArray: {$ifNull: ["$indexDoc.spec", {}]}}},
            allShards: {$first: "$allShards"},
        },
    },
    {
        $project: {
            missingFromShards: {$setDifference: ["$allShards", "$shards"]},
            inconsistentProperties: {
                $setDifference: [
                    {
                        $reduce: {
                            input: "$specs",
                            initialValue: {$arrayElemAt: ["$specs", 0]},
                            in: {$setUnion: ["$$value", "$$this"]},
                        },
                    },
                    {
                        $reduce: {
                            input: "$specs",
                            initialValue: {$arrayElemAt: ["$specs", 0]},
                            in: {$setIntersection: ["$$value", "$$this"]},
                        },
                    },
                ],
            },
        },
    },
    {
        $match: {
            $expr: {
                $or: [
                    {$gt: [{$size: "$missingFromShards"}, 0]},
                    {$gt: [{$size: "$inconsistentProperties"}, 0]},
                ],
            },
        },
    },
    {$project: {_id: 0, indexName: "$$ROOT._id", inconsistentProperties: 1, missingFromShards: 1}},
];

async function runDropIndexThroughRouter(host, ns, indexName) {
    const mongos = new Mongo(host);
    const [dbName, collName] = ns.split(".");
    return mongos.getDB(dbName).runCommand({dropIndexes: collName, index: indexName});
}

async function runCreateIndexThroughRouter(host, ns, indexName, indexKey) {
    const mongos = new Mongo(host);
    const [dbName, collName] = ns.split(".");
    return mongos.getDB(dbName).runCommand({
        createIndexes: collName,
        indexes: [{key: indexKey, name: indexName}],
    });
}

const st = new ShardingTest({shards: 2});
const dbName = "test";
const testDB = st.s.getDB(dbName);

assert.commandWorked(st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));

function setUpCollectionWithChunksOnBothShards(collName, indexName, indexKey) {
    const ns = `${dbName}.${collName}`;
    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));
    assert.commandWorked(st.s.adminCommand({split: ns, middle: {_id: 0}}));
    // Move the upper half to shard1 so both shards are data-bearing for the namespace.
    assert.commandWorked(
        st.s.adminCommand({moveChunk: ns, find: {_id: 0}, to: st.shard1.shardName}),
    );
    // Insert at least one doc on each side so the recipient enforces strict index sync.
    assert.commandWorked(testDB[collName].insert({_id: -1, x: -1}));
    assert.commandWorked(testDB[collName].insert({_id: 1, x: 1}));
    // Create the index that the race will mutate.
    if (indexName !== null) {
        assert.commandWorked(testDB[collName].createIndex(indexKey, {name: indexName}));
    }
    return ns;
}

function assertNoDivergence(collName, label) {
    const res = testDB[collName].aggregate(indexStatsDivergencePipeline).toArray();
    assert.eq(
        res.length,
        0,
        `[${label}] expected no index divergence after migration but $indexStats reported: ` +
            tojson(res),
    );
}

// ----------------------------------------------------------------------------
// Case 1: dropIndexes racing migration. Pre-fix, the recipient applies its stale snapshot AFTER
// the drop lands on the donor; recipient ends with the dropped index, donor without it.
// ----------------------------------------------------------------------------
(() => {
    jsTestLog("Case 1: dropIndexes during clone phase of an outgoing migration");

    const collName = "dropDuringMigration";
    const indexName = "raceIdx";
    const ns = setUpCollectionWithChunksOnBothShards(collName, indexName, {x: 1});

    // We will move the upper-half chunk back from shard1 to shard0. shard1 is the donor.
    const fromShard = st.shard1;
    const toShard = st.shard0;

    // Wedge the donor at startedMoveChunk: at this step the recipient has already taken the
    // donor's index snapshot but has not yet rejoined the steady state.
    pauseMoveChunkAtStep(fromShard, moveChunkStepNames.startedMoveChunk);

    const moveChunkThread = new Thread(
        async (host, ns, toShardName) => {
            const mongos = new Mongo(host);
            return mongos.adminCommand({moveChunk: ns, find: {_id: 0}, to: toShardName});
        },
        st.s.host,
        ns,
        toShard.shardName,
    );
    moveChunkThread.start();
    waitForMoveChunkStep(fromShard, moveChunkStepNames.startedMoveChunk);

    // Drop the index through the router. With the fix, this command should block on the
    // namespace DDL lock until the migration finishes (or be retried under shard-version-mismatch
    // and observe the post-migration routing table). Without the fix it races: the per-shard
    // step lands on the donor before the recipient's stale snapshot is applied.
    const dropThread = new Thread(runDropIndexThroughRouter, st.s.host, ns, indexName);
    dropThread.start();

    // Allow the migration to drain.
    unpauseMoveChunkAtStep(fromShard, moveChunkStepNames.startedMoveChunk);
    moveChunkThread.join();
    dropThread.join();

    // The migration itself may succeed or fail (interrupted by the index op). Either way, the
    // post-state must agree across shards. With the fix in place the dropIndexes either ran
    // before the migration's snapshot, after the commit, or was retried by the router and we
    // converge to a consistent state. Without the fix the recipient retains the pre-drop index.
    assertNoDivergence(collName, "dropIndexes-race");
})();

// ----------------------------------------------------------------------------
// Case 2: createIndexes racing migration. Pre-fix, the recipient's stale (pre-create) snapshot
// overwrites strict-sync onto the recipient; donor ends up with the new index, recipient
// without it.
// ----------------------------------------------------------------------------
(() => {
    jsTestLog("Case 2: createIndexes during clone phase of an outgoing migration");

    const collName = "createDuringMigration";
    const ns = setUpCollectionWithChunksOnBothShards(collName, /*indexName=*/ null, /*key=*/ null);

    const fromShard = st.shard1;
    const toShard = st.shard0;

    pauseMoveChunkAtStep(fromShard, moveChunkStepNames.startedMoveChunk);

    const moveChunkThread = new Thread(
        async (host, ns, toShardName) => {
            const mongos = new Mongo(host);
            return mongos.adminCommand({moveChunk: ns, find: {_id: 0}, to: toShardName});
        },
        st.s.host,
        ns,
        toShard.shardName,
    );
    moveChunkThread.start();
    waitForMoveChunkStep(fromShard, moveChunkStepNames.startedMoveChunk);

    const createThread = new Thread(
        runCreateIndexThroughRouter,
        st.s.host,
        ns,
        "raceIdxCreate",
        {y: 1},
    );
    createThread.start();

    unpauseMoveChunkAtStep(fromShard, moveChunkStepNames.startedMoveChunk);
    moveChunkThread.join();
    createThread.join();

    assertNoDivergence(collName, "createIndexes-race");
})();

// ----------------------------------------------------------------------------
// Case 3: follow-up migration must succeed. Once the bug fires, a subsequent migration into the
// recipient refuses to copy ("documents but inconsistent indexes"), which is what blocks
// `removeShard` and `transitionToDedicatedConfigServer` in production. This case re-attempts a
// migration after the races above and asserts it succeeds, which is the operational property
// the fix restores.
// ----------------------------------------------------------------------------
(() => {
    jsTestLog("Case 3: follow-up migration must succeed after a concurrent index op");

    const collName = "dropDuringMigration"; // re-use the collection from Case 1.
    const ns = `${dbName}.${collName}`;

    // Move the chunk one more time. With consistent indexes this is a no-op-shaped success;
    // with divergence the recipient's destination manager will refuse and the test fails.
    const res = st.s.adminCommand({moveChunk: ns, find: {_id: 0}, to: st.shard1.shardName});
    assert.commandWorked(
        res,
        "follow-up migration failed -- this is the user-visible symptom of SERVER-99357: " +
            tojson(res),
    );
})();

st.stop();

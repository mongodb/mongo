/**
 * Tests that opening a change stream with a future startAtOperationTime and then adding a new shard
 * before that time arrives does not result in unexpected events from the new shard.
 *
 * @tags: [
 *   requires_sharding,
 *   uses_change_streams,
 *   requires_fcv_90,
 *   # Sanitizer variants are too slow and can not add a shard within the given 'kSetupTimeoutSecs' timeframe.
 *   incompatible_aubsan,
 *   tsan_incompatible,
 *   # Continuous config server stepdowns make addShard/moveChunk slow enough to blow past
 *   # 'kSetupTimeoutSecs', causing the "cluster time already past future start time" setup assert.
 *   does_not_support_stepdowns,
 * ]
 */
import {describe, it, before, after} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {
    assertCreateCollection,
    assertDropCollection,
} from "jstests/libs/collection_drop_recreate.js";
import {
    ChangeStreamTest,
    addShardToCluster,
    getClusterTime,
    withBalancerEnabled,
} from "jstests/libs/query/change_stream_util.js";
import {removeShard} from "jstests/sharding/libs/remove_shard_util.js";

describe("future startAtOperationTime with addShard", function () {
    const rsNodeOptions = {
        setParameter: {writePeriodicNoops: true, periodicNoopIntervalSecs: 1},
    };

    // Budget (seconds) for the first setup attempt to finish before the future start time.
    const kSetupTimeoutSecs = 30;

    // If the first attempt exceeds the budget, one retry derives a fresh budget from the measured
    // setup latency.
    const kRetryMultiplier = 2;
    const kRetrySlackSecs = 10;

    // Slack (seconds) tacked onto a scenario's budget when waiting for cluster time after setup.
    const kWaitSlackSecs = 30;

    // Name of the new shard added to the ShardingTest.
    const newShardName = "newShard";

    let st;
    let db;

    before(function () {
        st = new ShardingTest({
            shards: 1,
            mongos: 1,
            rs: {nodes: 1},
            other: {rsOptions: rsNodeOptions, configOptions: rsNodeOptions, enableBalancer: false},
        });
        db = st.s.getDB(jsTestName());
    });

    after(function () {
        st.stop();
    });

    function withNewShard(fn) {
        let newShard;
        try {
            // Add a new shard while the stream is waiting for the future time.
            newShard = addShardToCluster(st, newShardName, 1, rsNodeOptions);
            fn();
        } finally {
            if (newShard) {
                withBalancerEnabled(st.s, () => {
                    removeShard(st, newShardName);
                    newShard.stopSet();
                });
            }
        }
    }

    for (const version of ["v1", "v2"]) {
        // Runs the scenario with the future start time 'budgetSecs' ahead of the current time.
        // Throws an error carrying 'setupElapsedSecs' if the setup consumes the whole budget.
        function runScenario(budgetSecs) {
            // Set up a sharded collection with all data on shard0.
            const collName = "coll_" + version;
            const coll = assertCreateCollection(db, collName);
            assert.commandWorked(
                db.adminCommand({shardCollection: coll.getFullName(), key: {_id: 1}}),
            );
            assert.commandWorked(db.adminCommand({split: coll.getFullName(), middle: {_id: 0}}));

            // Capture current time and set future start time ahead of it.
            const currentTime = getClusterTime(db);
            const futureStartTime = new Timestamp(currentTime.t + budgetSecs, 0);

            // Open a change stream at the future time.
            const csTest = new ChangeStreamTest(db);
            let csCursor = csTest.startWatchingChanges({
                pipeline: [
                    {$changeStream: {startAtOperationTime: futureStartTime, version: version}},
                ],
                collection: coll,
                aggregateOptions: {cursor: {batchSize: 0}},
            });

            // No events yet since we are in the future.
            csCursor = csTest.assertNoChange(csCursor);

            // Capture the initial PBRT.
            const initialPbrt = csTest.getResumeToken(csCursor);
            const initialPbrtTime = decodeResumeToken(initialPbrt).clusterTime;

            withNewShard(function () {
                // Move a chunk to the new shard so it has data.
                assert.commandWorked(
                    db.adminCommand({
                        moveChunk: coll.getFullName(),
                        find: {_id: 1},
                        to: newShardName,
                    }),
                );

                // Insert a document on the new shard BEFORE the future start time.
                assert.commandWorked(coll.insert({_id: 100, when: "before_future_time"}));

                // If setup consumed the whole budget, the new shard can no longer hold pre-future
                // data, so the scenario cannot be verified. Signal a retry with an adaptive budget.
                const currentClusterTime = getClusterTime(db);
                if (bsonWoCompare(currentClusterTime, futureStartTime) >= 0) {
                    csTest.cleanUp();
                    const err = new Error(`Setup exceeded its ${budgetSecs}s budget`);
                    err.setupElapsedSecs = currentClusterTime.t - currentTime.t;
                    throw err;
                }

                // Wait until the cluster time passes the future start time.
                assert.soon(
                    () => {
                        const configsvrClusterTime = getClusterTime(
                            st.configRS.getPrimary().getDB("admin"),
                        );
                        return configsvrClusterTime.t > futureStartTime.t;
                    },
                    "Timed out waiting for cluster time to reach future start time",
                    (budgetSecs + kWaitSlackSecs) * 1000,
                );

                // Advance the cursor after the future time has passed.
                csTest.assertNoChange(csCursor);

                // Capture PBRT after addShard + moveChunk + pre-future insert and verify PBRT did not regress after addShard.
                const postAddShardPbrt = csTest.getResumeToken(csCursor);
                const postAddShardPbrtTime = decodeResumeToken(postAddShardPbrt).clusterTime;
                assert.gte(
                    bsonWoCompare(postAddShardPbrtTime, initialPbrtTime),
                    0,
                    "PBRT should not regress after addShard",
                );

                // Insert a document on the new shard AFTER the future start time.
                assert.commandWorked(coll.insert({_id: 200, when: "after_future_time"}));

                // Should only see the post-future insert.
                csTest.assertNextChangesEqual({
                    cursor: csCursor,
                    expectedChanges: [
                        {
                            documentKey: {_id: 200},
                            fullDocument: {_id: 200, when: "after_future_time"},
                            ns: {db: db.getName(), coll: coll.getName()},
                            operationType: "insert",
                        },
                    ],
                });

                // No additional events, the pre-future insert must not appear.
                csTest.assertNoChange(csCursor);

                // Cleanup.
                csTest.cleanUp();
                assertDropCollection(db, collName);
            });
        }

        it(`${version} does not return pre-future events from new shard`, function () {
            try {
                runScenario(kSetupTimeoutSecs);
            } catch (e) {
                if (!e.setupElapsedSecs) {
                    throw e;
                }

                assertDropCollection(db, "coll_" + version);

                const retryBudget =
                    Math.ceil(e.setupElapsedSecs * kRetryMultiplier) + kRetrySlackSecs;
                jsTest.log.info(
                    `Setup exceeded its ${kSetupTimeoutSecs}s budget; retrying once with a ` +
                        `${retryBudget}s budget`,
                    {setupElapsedSecs: e.setupElapsedSecs},
                );
                runScenario(retryBudget);
            }
        });
    }
});

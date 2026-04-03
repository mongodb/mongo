/**
 * Tests that query stats are collected for update commands that go through various write dispatch
 * paths on mongos. Different update operations trigger different execution strategies depending on
 * factors like whether the shard key is present in the query filter, whether the write is retryable,
 * and whether the collection is sharded.
 *
 * @tags: [requires_fcv_90]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {after, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {
    assertAggregatedMetricsSingleExec,
    assertExpectedResults,
    getLatestQueryStatsEntry,
    getQueryStatsUpdateCmd,
    resetQueryStatsStore,
} from "jstests/libs/query/query_stats_utils.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

describe("query stats update command metrics (mongos)", function () {
    let st;
    let mongos;
    let testDB;

    // Set up the cluster for the whole test script here. Individual tests are logically grouped
    // into nested describe blocks.
    before(function () {
        const queryStatsParams = {
            internalQueryStatsWriteCmdSampleRate: 1,
        };
        st = new ShardingTest({
            shards: 2,
            mongosOptions: {setParameter: queryStatsParams},
            rsOptions: {setParameter: queryStatsParams},
        });
        mongos = st.s;
        testDB = mongos.getDB("test");
        assert.commandWorked(
            testDB.adminCommand({enableSharding: testDB.getName(), primaryShard: st.shard0.shardName}),
        );
    });

    after(function () {
        st?.stop();
    });

    beforeEach(function () {
        resetQueryStatsStore(mongos, "1MB");
    });

    // Verifies that mongos correctly aggregates query stats metrics from multiple shards when a
    // multi:true update fans out across shards.
    describe("multi-shard aggregation", function () {
        const collName = jsTestName() + "_multi_shard";
        let coll;

        const docsPerShard = 4;
        const totalDocs = docsPerShard * 2;

        function insertDistributedDocs() {
            assert.commandWorked(coll.deleteMany({}));
            const docs = [];
            for (let i = 0; i < docsPerShard; i++) {
                docs.push({_id: -(i + 1), v: i + 1});
            }
            for (let i = 0; i < docsPerShard; i++) {
                docs.push({_id: i + 1, v: docsPerShard + i + 1});
            }
            assert.commandWorked(coll.insertMany(docs));
        }

        before(function () {
            coll = testDB[collName];
            // Shard the collection and move chunks such that the primary has documents where
            // _id > 0 and negative _id documents are on shard1.
            st.shardColl(coll, {_id: 1}, {_id: 0}, {_id: 0});
        });

        beforeEach(function () {
            insertDistributedDocs();
        });

        it("should aggregate modifier update metrics across shards", function () {
            const cmd = {
                update: collName,
                updates: [
                    {
                        q: {},
                        u: {$set: {updated: true}},
                        multi: true,
                    },
                ],
                comment: "multi-shard modifier update",
            };

            assert.commandWorked(testDB.runCommand(cmd));

            const entry = getLatestQueryStatsEntry(testDB.getMongo(), {collName: coll.getName()});
            assert.eq(entry.key.queryShape.command, "update");

            assertAggregatedMetricsSingleExec(entry, {
                keysExamined: 0,
                docsExamined: totalDocs,
                hasSortStage: false,
                usedDisk: false,
                fromMultiPlanner: false,
                fromPlanCache: false,
                writes: {
                    nMatched: totalDocs,
                    nUpserted: 0,
                    nModified: totalDocs,
                    nDeleted: 0,
                    nInserted: 0,
                    nUpdateOps: 1,
                },
            });

            assertExpectedResults({
                results: entry,
                expectedQueryStatsKey: entry.key,
                expectedExecCount: 1,
                expectedDocsReturnedSum: 0,
                expectedDocsReturnedMax: 0,
                expectedDocsReturnedMin: 0,
                expectedDocsReturnedSumOfSq: 0,
            });
        });

        it("should record metrics for targeted single-shard multi update", function () {
            const cmd = {
                update: collName,
                updates: [
                    {
                        q: {_id: {$gt: 0}},
                        u: {$set: {updated: true}},
                        multi: true,
                    },
                ],
                comment: "single-shard targeted multi update",
            };

            assert.commandWorked(testDB.runCommand(cmd));

            const entry = getLatestQueryStatsEntry(testDB.getMongo(), {collName: coll.getName()});
            assert.eq(entry.key.queryShape.command, "update");

            assertAggregatedMetricsSingleExec(entry, {
                keysExamined: docsPerShard,
                docsExamined: docsPerShard,
                hasSortStage: false,
                usedDisk: false,
                fromMultiPlanner: false,
                fromPlanCache: false,
                writes: {
                    nMatched: docsPerShard,
                    nUpserted: 0,
                    nModified: docsPerShard,
                    nDeleted: 0,
                    nInserted: 0,
                    nUpdateOps: 1,
                },
            });

            assertExpectedResults({
                results: entry,
                expectedQueryStatsKey: entry.key,
                expectedExecCount: 1,
                expectedDocsReturnedSum: 0,
                expectedDocsReturnedMax: 0,
                expectedDocsReturnedMin: 0,
                expectedDocsReturnedSumOfSq: 0,
            });
        });
    });

    // Two-phase write protocol: triggered by updateOne on a sharded collection when the query
    // filter does not contain the shard key. Mongos runs a read phase (scatter-gather to find
    // the target document) followed by a write phase (targeted update on the owning shard).
    describe("two-phase write dispatch path", function () {
        const collName = jsTestName() + "_two_phase";
        let coll;

        before(function () {
            coll = testDB[collName];
            // Shard on {sk: 1} so that queries filtering on other fields don't include the shard
            // key, triggering the two-phase write protocol. Split at {sk: 0} and move one chunk
            // to shard1 so data is distributed across both shards.
            st.shardColl(coll, {sk: 1}, {sk: 0}, {sk: 0});
        });

        beforeEach(function () {
            assert.commandWorked(coll.deleteMany({}));
            assert.commandWorked(
                coll.insertMany([
                    {sk: -2, filterField: "a", v: 1},
                    {sk: -1, filterField: "b", v: 2},
                    {sk: 1, filterField: "c", v: 3},
                    {sk: 2, filterField: "d", v: 4},
                ]),
            );
        });

        it("should collect query stats for updateOne without shard key", function () {
            const cmd = {
                update: collName,
                updates: [
                    {
                        q: {filterField: "a"},
                        u: {$set: {updated: true}},
                        multi: false,
                    },
                ],
                comment: "two-phase updateOne",
            };

            assert.commandWorked(testDB.runCommand(cmd));

            const entry = getLatestQueryStatsEntry(testDB.getMongo(), {collName: coll.getName()});
            assert.eq(entry.key.queryShape.command, "update");

            assertExpectedResults({
                results: entry,
                expectedQueryStatsKey: entry.key,
                expectedExecCount: 1,
                expectedDocsReturnedSum: 0,
                expectedDocsReturnedMax: 0,
                expectedDocsReturnedMin: 0,
                expectedDocsReturnedSumOfSq: 0,
            });

            // The two-phase protocol's write phase targets the document by _id on the owning
            // shard, so keysExamined=1 (from the _id index) and docsExamined=1.
            assertAggregatedMetricsSingleExec(entry, {
                keysExamined: 1,
                docsExamined: 1,
                hasSortStage: false,
                usedDisk: false,
                fromMultiPlanner: false,
                fromPlanCache: false,
                writes: {
                    nMatched: 1,
                    nUpserted: 0,
                    nModified: 1,
                    nDeleted: 0,
                    nInserted: 0,
                    nUpdateOps: 1,
                },
            });
        });

        it("should collect query stats for updateOne without shard key when no document matches", function () {
            const cmd = {
                update: collName,
                updates: [
                    {
                        q: {filterField: "nonexistent"},
                        u: {$set: {updated: true}},
                        multi: false,
                    },
                ],
                comment: "two-phase updateOne no match",
            };

            assert.commandWorked(testDB.runCommand(cmd));

            const entry = getLatestQueryStatsEntry(testDB.getMongo(), {collName: coll.getName()});
            assert.eq(entry.key.queryShape.command, "update");

            assertExpectedResults({
                results: entry,
                expectedQueryStatsKey: entry.key,
                expectedExecCount: 1,
                expectedDocsReturnedSum: 0,
                expectedDocsReturnedMax: 0,
                expectedDocsReturnedMin: 0,
                expectedDocsReturnedSumOfSq: 0,
            });

            assertAggregatedMetricsSingleExec(entry, {
                keysExamined: 0,
                docsExamined: 0,
                hasSortStage: false,
                usedDisk: false,
                fromMultiPlanner: false,
                fromPlanCache: false,
                writes: {
                    nMatched: 0,
                    nUpserted: 0,
                    nModified: 0,
                    nDeleted: 0,
                    nInserted: 0,
                    nUpdateOps: 1,
                },
            });
        });

        it("should collect query stats for batched updateOnes without shard key", function () {
            const cmd = {
                update: collName,
                updates: [
                    {
                        q: {filterField: "a"},
                        u: {$set: {updated: true}},
                        multi: false,
                    },
                    {
                        q: {filterField: "c"},
                        u: {$set: {updated: true}},
                        multi: false,
                    },
                ],
                comment: "two-phase batched updateOnes",
            };

            assert.commandWorked(testDB.runCommand(cmd));

            const entry = getLatestQueryStatsEntry(testDB.getMongo(), {collName: coll.getName()});
            assert.eq(entry.key.queryShape.command, "update");

            // Each updateOne in the batch goes through its own two-phase execution, so
            // execCount=2. Both query shapes are identical after normalization, so they
            // accumulate into a single query stats entry.
            assertExpectedResults({
                results: entry,
                expectedQueryStatsKey: entry.key,
                expectedExecCount: 2,
                expectedDocsReturnedSum: 0,
                expectedDocsReturnedMax: 0,
                expectedDocsReturnedMin: 0,
                expectedDocsReturnedSumOfSq: 0,
            });
        });
    });

    // Retryable updateOne with an _id filter on a collection sharded by a different key. This
    // triggers the "broadcast to all shards" dispatch path (kRetryableWriteWithId in UWE,
    // WithoutShardKeyWithId in legacy). Mongos sends the update to every shard; only the shard
    // owning the document applies it.
    describe("retryable updateOne with _id, non-_id shard key", function () {
        const collName = jsTestName() + "_retryable_id";
        let coll;

        before(function () {
            coll = testDB[collName];
            // Shard on {sk: 1} so _id is not the shard key. Split and move so data is on both
            // shards.
            st.shardColl(coll, {sk: 1}, {sk: 0}, {sk: 0});
        });

        beforeEach(function () {
            assert.commandWorked(coll.deleteMany({}));
            assert.commandWorked(
                coll.insertMany([
                    {_id: 1, sk: -1, v: 10},
                    {_id: 2, sk: 1, v: 20},
                ]),
            );
        });

        it("should record query stats for a retryable updateOne by _id", function () {
            const lsid = {id: UUID()};

            const cmd = {
                update: collName,
                updates: [{q: {_id: 1}, u: {$set: {v: 100}}, multi: false}],
                lsid: lsid,
                txnNumber: NumberLong(1),
            };

            const result = assert.commandWorked(testDB.runCommand(cmd));
            assert.eq(result.nModified, 1);
            assert.eq(coll.findOne({_id: 1}).v, 100);

            const entries = getQueryStatsUpdateCmd(mongos, {collName: collName});
            assert.eq(entries.length, 1, "Expected 1 query stats entry: " + tojson(entries));
            assert.eq(entries[0].metrics.execCount, 1);

            assertAggregatedMetricsSingleExec(entries[0], {
                keysExamined: 1,
                docsExamined: 1,
                hasSortStage: false,
                usedDisk: false,
                fromMultiPlanner: false,
                fromPlanCache: false,
                writes: {
                    nMatched: 1,
                    nUpserted: 0,
                    nModified: 1,
                    nDeleted: 0,
                    nInserted: 0,
                    nUpdateOps: 1,
                },
            });
        });

        // TODO SERVER-121325 We double count for this case. Unskip this test case when that is
        // fixed.
        it.skip("retrying the same retryable updateOne by _id should not double-count", function () {
            const lsid = {id: UUID()};
            const txnNumber = NumberLong(1);

            const cmd = {
                update: collName,
                updates: [{q: {_id: 2}, u: {$set: {v: 200}}, multi: false}],
                lsid: lsid,
                txnNumber: txnNumber,
            };

            // Initial execution.
            const result = assert.commandWorked(testDB.runCommand(cmd));
            assert.eq(result.nModified, 1);
            assert.eq(coll.findOne({_id: 2}).v, 200);

            let entries = getQueryStatsUpdateCmd(mongos, {collName: collName});
            assert.eq(entries.length, 1, "Expected 1 entry after initial exec: " + tojson(entries));
            assert.eq(entries[0].metrics.execCount, 1);

            // Retry with the same lsid/txnNumber — the server recognises this as already-executed
            // and returns the cached result without re-executing. Query stats should not increment.
            const retryResult = assert.commandWorked(testDB.runCommand(cmd));
            assert.eq(retryResult.nModified, 1);

            entries = getQueryStatsUpdateCmd(mongos, {collName: collName});
            assert.eq(entries.length, 1, "Expected still 1 entry after retry: " + tojson(entries));
            assert.eq(entries[0].metrics.execCount, 1, "execCount should stay at 1 after retry");
        });
    });

    // StaleConfig retry: when a shard returns StaleConfig, mongos retries the write internally.
    // Query stats should only record the successful execution, not the failed attempt.
    describe("StaleConfig retried update", function () {
        const collName = jsTestName() + "_stale_config";
        let coll;
        let shard0Primary;

        before(function () {
            coll = testDB[collName];
            assert.commandWorked(testDB.adminCommand({shardCollection: coll.getFullName(), key: {_id: 1}}));
            assert.commandWorked(
                coll.insert([
                    {_id: 1, v: 1},
                    {_id: 2, v: 2},
                ]),
            );
            shard0Primary = st.rs0.getPrimary();
        });

        it("should record query stats once despite StaleConfig retry", function () {
            resetQueryStatsStore(st.shard0, "1MB");

            // Wait for any pending range deletions on shard0 to complete before activating the
            // failpoint. The alwaysThrowStaleConfigInfo failpoint fires for all namespaces, so a
            // background range deletion task could consume the {times: 1} activation before the
            // intended update command does, leaving the range deletion stuck in "processing" state
            // and causing st.stop() to time out waiting for config.rangeDeletions to drain.
            assert.soon(
                () => shard0Primary.getDB("config").rangeDeletions.find().itcount() === 0,
                "Timed out waiting for range deletions on shard0 to complete",
            );

            // Force shard0 to return StaleConfig on the next metadata check, which triggers a
            // mongos-level retry. The failpoint expires after one activation, so the retry
            // succeeds.
            const fp = configureFailPoint(shard0Primary, "alwaysThrowStaleConfigInfo", {}, {times: 1});

            const result = assert.commandWorked(
                testDB.runCommand({
                    update: collName,
                    updates: [{q: {_id: 1}, u: {$set: {v: 100}}, multi: false}],
                }),
            );
            assert.eq(result.nModified, 1);
            assert.eq(coll.findOne({_id: 1}).v, 100);

            assert(fp.waitWithTimeout(1000), "alwaysThrowStaleConfigInfo failpoint was never triggered");

            // Mongos should show exactly 1 execution despite the internal retry.
            const mongosEntries = getQueryStatsUpdateCmd(mongos, {collName: collName});
            assert.eq(mongosEntries.length, 1, "Expected 1 mongos query stats entry: " + tojson(mongosEntries));
            assert.eq(mongosEntries[0].metrics.execCount, 1);

            // The shard should also show exactly 1 execution (the successful one).
            const shardEntries = getQueryStatsUpdateCmd(st.shard0, {collName: collName});
            assert.eq(shardEntries.length, 1, "Expected 1 shard query stats entry: " + tojson(shardEntries));
            assert.eq(shardEntries[0].metrics.execCount, 1);

            fp.off();
        });
    });

    // WouldChangeOwningShard (WCOS): triggered when a targeted updateOne modifies the shard key
    // such that the document would move to a different shard. The owning shard returns a WCOS
    // error, and mongos handles it by deleting the old document and inserting the new one on the
    // target shard via an internal transaction.
    //
    // TODO SERVER-121267 This doesn't work right yet. We should update this test once it does.
    describe.skip("update that triggers WouldChangeOwningShard", function () {
        const collName = jsTestName() + "_wcos";
        let coll;

        before(function () {
            coll = testDB[collName];
            st.shardColl(coll, {sk: 1}, {sk: 0}, {sk: 0});
        });

        beforeEach(function () {
            assert.commandWorked(coll.deleteMany({}));
            assert.commandWorked(
                coll.insertMany([
                    {_id: 1, sk: -1, v: 1},
                    {_id: 2, sk: 1, v: 2},
                ]),
            );
        });

        it("should collect query stats when update changes shard key ownership", function () {
            // Update sk from -1 to 5, moving the document from shard0 to shard1.
            // Shard key updates require retryWrites: true or a multi-statement transaction.
            const lsid = {id: UUID()};
            const cmd = {
                update: collName,
                updates: [
                    {
                        q: {sk: -1},
                        u: {$set: {sk: 5}},
                        multi: false,
                    },
                ],
                lsid: lsid,
                txnNumber: NumberLong(1),
            };

            const result = assert.commandWorked(testDB.runCommand(cmd));
            assert.eq(result.nModified, 1);

            // Verify the document actually moved.
            assert.eq(coll.findOne({_id: 1}).sk, 5);

            const entry = getLatestQueryStatsEntry(testDB.getMongo(), {collName: coll.getName()});
            assert.eq(entry.key.queryShape.command, "update");

            assertExpectedResults({
                results: entry,
                expectedQueryStatsKey: entry.key,
                expectedExecCount: 1,
                expectedDocsReturnedSum: 0,
                expectedDocsReturnedMax: 0,
                expectedDocsReturnedMin: 0,
                expectedDocsReturnedSumOfSq: 0,
            });

            // TODO SERVER-121267: The initial targeted update finds the document but returns a
            // WCOS error instead of modifying it, so its execution metrics (docsExamined,
            // keysExamined) are not propagated. The actual data movement happens via an internal
            // transaction (delete + insert), whose metrics also aren't surfaced. As a result,
            // all execution and write metrics are 0 even though the outer response reports
            // nModified=1.
            assertAggregatedMetricsSingleExec(entry, {
                keysExamined: 0,
                docsExamined: 0,
                hasSortStage: false,
                usedDisk: false,
                fromMultiPlanner: false,
                fromPlanCache: false,
                writes: {
                    nMatched: 0,
                    nUpserted: 0,
                    nModified: 0,
                    nDeleted: 0,
                    nInserted: 0,
                    nUpdateOps: 1,
                },
            });
        });
    });
});

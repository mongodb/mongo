/**
 * Shared test helpers for write command (insert/update/delete) query stats tests.
 * Import this file for mocha-style metrics tests and one-way tokenization tests.
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {after, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {
    assertAggregatedMetricsSingleExec,
    assertExpectedResults,
    getLatestQueryStatsEntry,
    getQueryExecMetrics,
    resetQueryStatsStore,
} from "jstests/libs/query/query_stats_utils.js";
import newMongoWithRetry from "jstests/libs/retryable_mongo.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {DiscoverTopology, Topology} from "jstests/libs/discover_topology.js";

const getDB = (primaryConnection) => primaryConnection.getDB(jsTestName());

/**
 * Asserts that the most recent query stats entry for `coll` matches a single-execution write
 * command. Callers supply only the fields that vary per test; the boolean planner flags all
 * default to false and the docs-returned metrics default to 0.
 */
export function assertWriteCmdQueryStatsSingleExec(
    testDB,
    coll,
    {command, keysExamined, docsExamined, writes},
) {
    const entry = getLatestQueryStatsEntry(testDB.getMongo(), {collName: coll.getName()});
    assert.eq(entry.key.queryShape.command, command);
    assertAggregatedMetricsSingleExec(entry, {
        keysExamined,
        docsExamined,
        hasSortStage: false,
        usedDisk: false,
        fromMultiPlanner: false,
        fromPlanCache: false,
        writes,
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
}

/**
 * Drops and re-populates a collection with 8 documents {v: 1} through {v: 8}, used as a standard
 * starting state for write command query stats tests.
 */
export function resetQueryStatsCollection(coll) {
    coll.drop();
    assert.commandWorked(
        coll.insert([{v: 1}, {v: 2}, {v: 3}, {v: 4}, {v: 5}, {v: 6}, {v: 7}, {v: 8}]),
    );
}

/**
 * Resets test collections for query stats tests on a sharded cluster.
 *
 * Drops and re-populates both an unsharded and sharded collection, creates indexes,
 * and optionally splits/moves chunks for multi-shard tests.
 *
 * @param {object} options
 * @param {object} options.routerDB - The router (mongos) database connection.
 * @param {string} options.unshardedCollName - The unsharded collection name.
 * @param {string} options.shardedCollName - The sharded collection name.
 * @param {Array} options.testDocuments - Documents to insert into both collections.
 * @param {object} [options.shardKey={v: 1}] - The shard key to use.
 * @param {object} [options.st=null] - The ShardingTest instance (required if splitMiddle is set).
 * @param {object} [options.splitMiddle=null] - The split point (e.g., {v: 4}). If set, chunks will
 *     be split and the upper chunk moved to shard1.
 * @param {object} [options.moveChunkFind=null] - The find query for moveChunk (e.g., {v: 5}).
 *     Required if splitMiddle is set.
 */
export function resetTestCollectionsShardedCluster({
    routerDB,
    unshardedCollName,
    shardedCollName,
    testDocuments,
    shardKey = {v: 1},
    st = null,
    splitMiddle = null,
    moveChunkFind = null,
}) {
    // Reset unsharded collection.
    const unshardedColl = routerDB[unshardedCollName];
    unshardedColl.drop();
    assert.commandWorked(unshardedColl.insert(testDocuments));
    assert.commandWorked(
        routerDB.adminCommand({untrackUnshardedCollection: unshardedColl.getFullName()}),
    );

    // Reset sharded collection.
    const shardedColl = routerDB[shardedCollName];
    shardedColl.drop();
    assert.commandWorked(shardedColl.insert(testDocuments));

    // Create indexes.
    unshardedColl.createIndex(shardKey);
    shardedColl.createIndex(shardKey);

    // Shard the sharded collection.
    assert.commandWorked(
        routerDB.adminCommand({shardCollection: shardedColl.getFullName(), key: shardKey}),
    );

    // Optionally split and move chunks to distribute data across shards.
    if (splitMiddle && st) {
        assert.commandWorked(
            routerDB.adminCommand({split: shardedColl.getFullName(), middle: splitMiddle}),
        );
        assert.commandWorked(
            routerDB.adminCommand({
                moveChunk: shardedColl.getFullName(),
                find: moveChunkFind,
                to: st.shard1.shardName,
            }),
        );
    }
}

/**
 * Creates a standard write command query stats test suite on a single-node replica set.
 * Sets up the ReplSet fixture and query stats store / collection reset in hooks, then calls
 * bodyFn with a ctxFn accessor so inner tests can reach {conn, testDB, coll, collName}.
 *
 * @param {string} label - outer describe label
 * @param {Function} bodyFn - function(ctxFn) where ctxFn() => {conn, testDB, coll, collName}
 */
export function describeWriteCmdQueryStatsReplicaSetTests(label, bodyFn) {
    describe(label, function () {
        let rst, conn, testDB, coll;
        const collName = jsTestName() + "_metrics";

        before(function () {
            rst = new ReplSetTest({
                nodes: 1,
                nodeOptions: {
                    setParameter: {
                        internalQueryStatsWriteCmdSampleRate: 1,
                    },
                },
            });
            rst.startSet();
            rst.initiate();
            conn = rst.getPrimary();
            testDB = conn.getDB("test");
            coll = testDB[collName];
        });

        after(function () {
            rst?.stopSet();
        });

        beforeEach(function () {
            resetQueryStatsStore(conn, "1MB");
            resetQueryStatsCollection(coll);
        });

        bodyFn(() => ({conn, testDB, coll, collName}));
    });
}

/**
 * Creates a standard write command query stats test suite on a sharded cluster.
 * Sets up the ShardingTest fixture and query stats store / collection reset in hooks, then calls
 * bodyFn with a ctxFn accessor so inner tests can reach {conn, testDB, coll, collName}.
 *
 * @param {string} label - outer describe label
 * @param {Function} bodyFn - function(ctxFn) where ctxFn() => {conn, testDB, coll, collName}
 */
export function describeWriteCmdQueryStatsShardedTests(label, bodyFn) {
    describe(label, function () {
        let st, testDB, coll;
        const collName = jsTestName() + "_sharded";

        before(function () {
            st = new ShardingTest({
                shards: 2,
                mongosOptions: {
                    setParameter: {
                        internalQueryStatsWriteCmdSampleRate: 1,
                    },
                },
            });
            testDB = st.s.getDB("test");
            coll = testDB[collName];
            st.shardColl(coll, {_id: 1}, {_id: 1});
        });

        after(function () {
            st?.stop();
        });

        beforeEach(function () {
            resetQueryStatsCollection(coll);
            resetQueryStatsStore(st.s, "1MB");
        });

        bodyFn(() => ({st, testDB, coll, collName}));
    });
}

/**
 * Describes the retryable write query stats tests inside the current describe block:
 * (1) retried already-executed statements should not double-count execCount,
 * (2) a failed attempt should not record stats; the successful retry should.
 *
 * @param {string} label - inner describe label
 * @param {Function} ctxFn - function() => {conn, testDB}
 * @param {object} opts
 *   makeOp(val)            - builds one write op with a given argument
 *   opsField               - "deletes" | "updates"
 *   cmdName                - "delete" | "update"
 *   getCount(result)       - extracts the write count from a result
 *   getQueryStatsCmd       - e.g. getQueryStatsDeleteCmd
 *   assertDocModified(coll, id) - asserts expected doc state after a successful op
 */
export function describeRetryableWriteQueryStatsTests(
    label,
    ctxFn,
    {makeOp, opsField, cmdName, getCount, getQueryStatsCmd, assertDocModified},
) {
    describe(label, function () {
        const collName = jsTestName() + "_retries";
        let coll;

        before(function () {
            coll = ctxFn().testDB[collName];
        });

        beforeEach(function () {
            coll.drop();
            assert.commandWorked(
                coll.insert([
                    {_id: 1, a: 1, b: "abc"},
                    {_id: 2, a: 2, b: "def"},
                    {_id: 3, a: 3, b: "geh"},
                ]),
            );
        });

        // When retryable writes are active (indicated by the presence of a logical session ID and a transaction ID),
        // we should only record query stats when the write is actually executed, even if it's retried several times.

        it("retried already-executed statements in a batch should not record query stats", function () {
            const {conn, testDB} = ctxFn();
            const lsid = {id: UUID()};
            const txnNumber = NumberLong(1);

            const firstResult = assert.commandWorked(
                testDB.runCommand({
                    [cmdName]: collName,
                    [opsField]: [makeOp(1), makeOp(2)],
                    lsid,
                    txnNumber,
                }),
            );
            assert.eq(getCount(firstResult), 2);

            let entries = getQueryStatsCmd(conn, {collName});
            assert.eq(entries.length, 1, "Expected 1 query stats entry after initial batch");
            assert.eq(entries[0].metrics.execCount, 2);

            const retryResult = assert.commandWorked(
                testDB.runCommand({
                    [cmdName]: collName,
                    [opsField]: [makeOp(1), makeOp(2), makeOp(3)],
                    lsid,
                    txnNumber,
                }),
            );
            assert.eq(retryResult.retriedStmtIds, [0, 1], "Expected retriedStmtIds [0, 1]", {
                retriedStmtIds: retryResult.retriedStmtIds,
            });

            entries = getQueryStatsCmd(conn, {collName});
            assert.eq(entries.length, 1, "Expected still 1 query stats entry after partial retry");
            assert.eq(
                entries[0].metrics.execCount,
                3,
                "execCount should be 3 (2 original + 1 new; retries not counted)",
            );
        });

        it("failed initial attempt should not record query stats; successful retry should", function () {
            const {conn, testDB} = ctxFn();
            const lsid = {id: UUID()};
            const cmd = {
                [cmdName]: collName,
                [opsField]: [makeOp(2)],
                lsid,
                txnNumber: NumberLong(1),
            };

            const fp = configureFailPoint(
                conn,
                "failCommand",
                {
                    errorCode: ErrorCodes.OperationFailed,
                    failCommands: [cmdName],
                    namespace: "test." + collName,
                },
                {times: 1},
            );

            assert.commandFailedWithCode(testDB.runCommand(cmd), ErrorCodes.OperationFailed);
            assert.eq(
                coll.findOne({_id: 2}).a,
                2,
                "Document should not be affected after failed attempt",
            );

            let entries = getQueryStatsCmd(conn, {collName});
            assert.eq(entries.length, 0, "Expected no query stats after failed attempt");

            const retryResult = assert.commandWorked(testDB.runCommand(cmd));
            assert.eq(getCount(retryResult), 1);
            assertDocModified(coll, 2);

            entries = getQueryStatsCmd(conn, {collName});
            assert.eq(entries.length, 1, "Expected 1 query stats entry after successful retry");
            assert.eq(entries[0].metrics.execCount, 1);

            fp.off();
        });
    });
}

/**
 * Scaffolds a one-way tokenization test suite for a write command against a specific topology.
 * Handles fixture lifecycle (before/after) and query stats store reset (beforeEach).
 *
 * @param {string} label - describe block label
 * @param {Function} setupFn - function() => {fixture, testDB}
 * @param {Function} teardownFn - function(fixture) => void
 * @param {string} collName - collection name
 * @param {Array|object} initialDocs - documents inserted before tests run
 * @param {Function} testBodyFn - function(ctxFn) that registers it() blocks;
 *   ctxFn() => {testDB, coll}, valid only at test execution time (after before())
 */
export function runTokenizationTestsForTopology(
    label,
    setupFn,
    teardownFn,
    {collName, initialDocs},
    testBodyFn,
) {
    describe(label, function () {
        let fixture, testDB, coll;

        before(function () {
            const res = setupFn();
            fixture = res.fixture;
            testDB = res.testDB;
            coll = testDB[collName];
            coll.drop();
            assert.commandWorked(coll.insert(initialDocs));
        });

        after(function () {
            if (fixture) {
                teardownFn(fixture);
            }
        });

        beforeEach(function () {
            resetQueryStatsStore(testDB.getMongo(), "1MB");
        });

        testBodyFn(() => ({testDB, coll}));
    });
}

/**
 * Runs all four mongos write-command metrics scenarios for one spec (update or delete):
 *   1. Multi-shard fanout
 *   2. Two-phase write (filter doesn't include shard key)
 *   3. Retryable write with _id, non-_id shard key
 *   4. StaleConfig retry
 *
 * `validateWriteMetricsFn(nAffected, keysPerDoc)` must return the expected `writes` object for a
 * single execution, given the number of affected documents and `keysPerDoc` (the number of indexes
 * on the scenario's collection, i.e. how many index keys are maintained per affected document).
 * Use both arguments to compute `keysInserted`/`keysDeleted`: a delete removes `nAffected *
 * keysPerDoc` keys; an indexed-field update inserts and deletes that many. A spec whose writes never
 * touch an index (e.g. updates of non-indexed fields) legitimately returns `keysInserted: 0,
 * keysDeleted: 0` regardless of `keysPerDoc` -- note that in the spec so the unused argument reads as
 * intentional rather than an oversight.
 */
export function runMongosWriteMetricsTests({
    label,
    commands,
    validateWriteMetricsFn,
    validateCmdFn,
    getQueryStatsFn,
    extraTests,
    docsExaminedOverride,
}) {
    describe(label, function () {
        let st;
        let mongos;
        let testDB;

        // keysPerDoc is the number of index keys touched per affected document (i.e. the number of
        // indexes on the collection), used to compute the expected keysInserted/keysDeleted. It is
        // required (no default) so every scenario states its collection's index count explicitly --
        // a silent default would hide a wrong expectation if a collection's index set changed.
        function assertExecMetrics(entry, {keysExamined, docsExamined, nAffected, keysPerDoc}) {
            assert.neq(
                keysPerDoc,
                undefined,
                "assertExecMetrics requires keysPerDoc (index count on the collection)",
            );

            // On sharded clusters, deletes can double-count docsExamined: the COLLSCAN phase counts
            // each matching document once, and then the DELETE stage could re-fetch it from the
            // shard, counting it a second time. So docsExamined may be anywhere in [expected,
            // expected * 2]. When docsExaminedOverride is set, we assert the range rather than an
            // exact value.
            let resolvedDocsExamined = docsExamined;
            if (docsExaminedOverride) {
                const actual = getQueryExecMetrics(entry.metrics).docsExamined.sum;
                assert.gte(actual, docsExamined);
                assert.lte(actual, docsExamined * 2);
                resolvedDocsExamined = actual;
            }

            // We validate docsExamined above, for assertAggregatedMetricsSingleExec to pass we will
            // pass in the actual docsExamined value.
            assertAggregatedMetricsSingleExec(entry, {
                keysExamined,
                docsExamined: resolvedDocsExamined,
                hasSortStage: false,
                usedDisk: false,
                fromMultiPlanner: false,
                fromPlanCache: false,
                writes: validateWriteMetricsFn(nAffected, keysPerDoc),
            });
        }

        before(function () {
            const queryStatsParams = {internalQueryStatsWriteCmdSampleRate: 1};
            st = new ShardingTest({
                shards: 2,
                mongosOptions: {setParameter: queryStatsParams},
                rsOptions: {setParameter: queryStatsParams},
            });
            mongos = st.s;
            testDB = mongos.getDB("test");
            assert.commandWorked(
                testDB.adminCommand({
                    enableSharding: testDB.getName(),
                    primaryShard: st.shard0.shardName,
                }),
            );
        });

        after(function () {
            st?.stop();
        });

        beforeEach(function () {
            resetQueryStatsStore(mongos, "1MB");
        });

        // -----------------------------------------------------------------------
        // Scenario 1: Multi-shard fanout
        //
        // A multi:true write with no shard-key filter fans out to all shards.
        // mongos aggregates the metrics returned from each shard.
        // -----------------------------------------------------------------------
        describe("multi-shard fanout", function () {
            const collName = label + "_fanout";
            let coll;
            const docsPerShard = 4;
            const totalDocs = docsPerShard * 2;

            before(function () {
                coll = testDB[collName];
                // Shard the collection and move chunks such that the primary has documents where
                // _id > 0 and negative _id documents are on shard1.
                st.shardColl(coll, {_id: 1}, {_id: 0}, {_id: 0});
            });

            beforeEach(function () {
                assert.commandWorked(coll.deleteMany({}));
                const docs = [];
                for (let i = 0; i < docsPerShard; i++) docs.push({_id: -(i + 1), v: i + 1});
                for (let i = 0; i < docsPerShard; i++)
                    docs.push({_id: i + 1, v: docsPerShard + i + 1});
                assert.commandWorked(coll.insertMany(docs));
                resetQueryStatsStore(mongos, "1MB");
            });

            it(`aggregates metrics across all shards`, function () {
                assert.commandWorked(testDB.runCommand(commands.multiAll(collName)));

                const entry = getLatestQueryStatsEntry(testDB.getMongo(), {collName});
                assert.eq(entry.key.queryShape.command, label);
                assertExecMetrics(entry, {
                    keysExamined: 0,
                    docsExamined: totalDocs,
                    nAffected: totalDocs,
                    // This collection is sharded on {_id: 1}, so it has a single index.
                    keysPerDoc: 1,
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

            it(`targeted single-shard multi-write`, function () {
                assert.commandWorked(testDB.runCommand(commands.multiTargeted(collName)));

                const entry = getLatestQueryStatsEntry(testDB.getMongo(), {collName});
                assert.eq(entry.key.queryShape.command, label);
                assertExecMetrics(entry, {
                    keysExamined: docsPerShard,
                    docsExamined: docsPerShard,
                    nAffected: docsPerShard,
                    // Sharded on {_id: 1}: single index.
                    keysPerDoc: 1,
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

        // -----------------------------------------------------------------------
        // Scenario 2: Two-phase write
        //
        // A single-doc write whose filter doesn't include the shard key triggers mongos to run a
        // read phase (scatter-gather to find the target document) followed by a write phase
        // (targeted update on the owning shard).
        // -----------------------------------------------------------------------
        describe("two-phase write (filter doesn't include shard key)", function () {
            const collName = label + "_two_phase";
            let coll;

            before(function () {
                coll = testDB[collName];
                // Shard on {sk: 1} so that queries filtering on other fields don't include the
                // shard key, triggering the two-phase write protocol. Split at {sk: 0} and move
                // one chunk to shard1 so data is distributed across both shards.
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
                resetQueryStatsStore(mongos, "1MB");
            });

            it(`match found — write phase targets by _id (keysExamined=1)`, function () {
                assert.commandWorked(testDB.runCommand(commands.singleOp(collName, "a")));

                const entry = getLatestQueryStatsEntry(testDB.getMongo(), {collName});
                assert.eq(entry.key.queryShape.command, label);
                // The two-phase protocol's write phase targets the document by _id on the owning
                // shard, so keysExamined=1 (from the _id index) and docsExamined=1. The collection
                // is sharded on {sk: 1}, so it has two indexes (_id and sk) — a delete removes a key
                // from each.
                assertExecMetrics(entry, {
                    keysExamined: 1,
                    docsExamined: 1,
                    nAffected: 1,
                    keysPerDoc: 2,
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

            it(`no document matches`, function () {
                assert.commandWorked(testDB.runCommand(commands.noMatch(collName)));

                const entry = getLatestQueryStatsEntry(testDB.getMongo(), {collName});
                assert.eq(entry.key.queryShape.command, label);
                assertExecMetrics(entry, {
                    keysExamined: 0,
                    docsExamined: 0,
                    nAffected: 0,
                    keysPerDoc: 2,
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

            it(`batched ops each go through two-phase, accumulate execCount`, function () {
                assert.commandWorked(testDB.runCommand(commands.batchOp(collName, "a", "c")));

                const entry = getLatestQueryStatsEntry(testDB.getMongo(), {collName});
                assert.eq(entry.key.queryShape.command, label);
                // Each op in the batch goes through its own two-phase execution, so execCount=2.
                // Both query shapes are identical after normalization, so they accumulate into a
                // single query stats entry.
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

        // -----------------------------------------------------------------------
        // Scenario 3: Retryable write with _id, non-_id shard key
        //
        // Retryable single-doc op with an _id filter on a collection sharded by a different key.
        // This triggers the "broadcast to all shards" dispatch path (kRetryableWriteWithId in UWE,
        // WithoutShardKeyWithId in legacy). Mongos sends the update to every shard; only the shard
        // owning the document applies it.
        // -----------------------------------------------------------------------
        describe("retryable write with _id (non-_id shard key)", function () {
            const collName = label + "_retryable";
            let coll;

            before(function () {
                coll = testDB[collName];
                // Shard on {sk: 1} so _id is not the shard key. Split and move so data is on
                // both shards.
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
                resetQueryStatsStore(mongos, "1MB");
            });

            it(`by _id: single execution recorded`, function () {
                const cmd = {
                    ...commands.byId(collName),
                    lsid: {id: UUID()},
                    txnNumber: NumberLong(1),
                };
                const result = assert.commandWorked(testDB.runCommand(cmd));
                validateCmdFn(result);

                const entries = getQueryStatsFn(mongos, {collName});
                assert.eq(entries.length, 1, "Expected 1 query stats entry", {entries});
                assert.eq(entries[0].metrics.execCount, 1);
                // Sharded on {sk: 1}, so two indexes (_id and sk) — a delete removes a key from each.
                assertExecMetrics(entries[0], {
                    keysExamined: 1,
                    docsExamined: 1,
                    nAffected: 1,
                    keysPerDoc: 2,
                });
            });

            // TODO SERVER-121325 We double count for this case. Unskip this test case when that
            // is fixed.
            it.skip(`One by _id: double execution should not double count`, function () {
                const cmd = {
                    ...commands.byId(collName),
                    lsid: {id: UUID()},
                    txnNumber: NumberLong(1),
                };

                // Initial execution.
                const result = assert.commandWorked(testDB.runCommand(cmd));
                validateCmdFn(result);

                let entries = getQueryStatsFn(mongos, {collName});
                assert.eq(entries.length, 1, "Expected 1 entry after initial exec", {entries});
                assert.eq(entries[0].metrics.execCount, 1);

                // Retry with the same lsid/txnNumber — the server recognises this as
                // already-executed and returns the cached result without re-executing. Query stats
                // should not increment.
                const retryResult = assert.commandWorked(testDB.runCommand(cmd));
                assert.eq(retryResult.nModified, 1);

                entries = getQueryStatsFn(mongos, {collName: collName});
                assert.eq(entries.length, 1, "Expected still 1 entry after retry", {entries});
                assert.eq(
                    entries[0].metrics.execCount,
                    1,
                    "execCount should stay at 1 after retry",
                );
            });
        });

        // -----------------------------------------------------------------------
        // Scenario 4: StaleConfig retry
        //
        // When a shard returns StaleConfig, mongos retries the write internally.
        // Query stats should record exactly one execution (the successful retry).
        // -----------------------------------------------------------------------
        describe("StaleConfig retry", function () {
            const collName = label + "_stale_config";
            let coll;
            let shard0Primary;

            before(function () {
                coll = testDB[collName];
                assert.commandWorked(
                    testDB.adminCommand({shardCollection: coll.getFullName(), key: {_id: 1}}),
                );
                assert.commandWorked(
                    coll.insert([
                        {_id: 1, v: 1},
                        {_id: 2, v: 2},
                    ]),
                );
                shard0Primary = st.rs0.getPrimary();
            });

            it(`exactly one entry recorded despite internal retry`, function () {
                // Restore documents modified by a previous spec's test in the same before().
                assert.commandWorked(coll.deleteMany({}));
                assert.commandWorked(
                    coll.insert([
                        {_id: 1, v: 1},
                        {_id: 2, v: 2},
                    ]),
                );

                // Clear entries created by the data-reset deleteMany above on both the shard and
                // mongos so they don't appear as additional query stats entries in the assertions.
                resetQueryStatsStore(st.shard0, "1MB");
                resetQueryStatsStore(mongos, "1MB");

                // Wait for any pending range deletions on shard0 to complete before activating
                // the failpoint. The alwaysThrowStaleConfigInfo failpoint fires for all
                // namespaces, so a background range deletion task could consume the {times: 1}
                // activation before the intended update command does, leaving the range deletion
                // stuck in "processing" state and causing st.stop() to time out waiting for
                // config.rangeDeletions to drain.
                assert.soon(
                    () => shard0Primary.getDB("config").rangeDeletions.find().itcount() === 0,
                    "Timed out waiting for range deletions on shard0 to complete",
                );

                // Force shard0 to return StaleConfig on the next metadata check, which triggers a
                // mongos-level retry. The failpoint expires after one activation, so the retry
                // succeeds.
                const fp = configureFailPoint(
                    shard0Primary,
                    "alwaysThrowStaleConfigInfo",
                    {},
                    {times: 1},
                );
                const result = assert.commandWorked(testDB.runCommand(commands.byId(collName)));
                validateCmdFn(result);
                assert(
                    fp.waitWithTimeout(1000),
                    "alwaysThrowStaleConfigInfo failpoint was never triggered",
                );

                // Mongos should show exactly 1 execution despite the internal retry.
                const mongosEntries = getQueryStatsFn(mongos, {collName});
                assert.eq(mongosEntries.length, 1, "Expected 1 mongos entry", {mongosEntries});
                assert.eq(mongosEntries[0].metrics.execCount, 1);

                // The shard should also show exactly 1 execution (the successful one).
                const shardEntries = getQueryStatsFn(st.shard0, {collName});
                assert.eq(shardEntries.length, 1, "Expected 1 shard entry", {shardEntries});
                assert.eq(shardEntries[0].metrics.execCount, 1);

                fp.off();
            });
        });

        if (extraTests) {
            extraTests(() => ({st, mongos, testDB}));
        }
    });
}

/**
 * Given an initial connection to the cluster, applies the given function on all shard servers.
 * @param {*} primaryConn
 * @param {*} perShardFn Function to apply on each shard server connection.
 */
export function applyOnShardServers(primaryConn, perShardFn) {
    const topology = DiscoverTopology.findConnectedNodes(primaryConn);
    assert.eq(topology.type, Topology.kShardedCluster);

    // Sharded cluster - run on all shard nodes as well.
    for (let shardName of Object.keys(topology.shards)) {
        const shard = topology.shards[shardName];
        if (shard.type === Topology.kReplicaSet) {
            // Await replication to ensure all of the shards are queryable.
            const rst = new ReplSetTest(shard.primary);
            rst.awaitReplication();
            perShardFn(newMongoWithRetry(shard.primary));
        } else if (shard.type === Topology.kStandalone) {
            perShardFn(newMongoWithRetry(shard.mongod));
        }
    }
}

/**
 * Asserts that runCmdFn is not recorded in query stats. Note that if FCV is downgraded without restarting
 * the server binaries, the previously recorded stats will still persist in query store. Thus, we reset the stores
 * at the beginning of this function to ensure a clean state.
 */
export function assertCommandNotRecorded(conn, runCmdFn, getQueryStatsFn) {
    let queryStats;
    const db = getDB(conn);
    // Retry on transient errors: during a rolling restart a node can be briefly unavailable.
    assert.soonNoExcept(() => {
        resetQueryStatsStore(conn, "1MB");
        assert.commandWorked(runCmdFn(db));
        queryStats = getQueryStatsFn(db);
        return true;
    });
    assert.eq(queryStats, [], "Expected no query stats entries, but found some");
}

/**
 * Asserts that runCmdFn is recorded in query stats.
 */
export function assertCommandRecorded(conn, runCmdFn, getQueryStatsFn, assertQueryStatsMetricsFn) {
    let queryStats;
    const db = getDB(conn);
    // Retry on transient errors: during a rolling restart a node can be briefly unavailable.
    assert.soonNoExcept(() => {
        resetQueryStatsStore(conn, "1MB");
        assert.commandWorked(runCmdFn(db));
        queryStats = getQueryStatsFn(db);
        return true;
    });
    assert.neq(queryStats, [], "Expected query stats entries, but found none");
    assert.eq(queryStats.length, 1, "Expected exactly one query stats entry but found more");
    const entry = queryStats[0];
    assertQueryStatsMetricsFn(entry);
}

/**
 * Asserts that commands are recorded in query stats on all shard servers but not the router.
 * Resets each shard's store, runs all ops in runCmdFns, then checks every shard has exactly one entry.
 *
 * @param {Mongo} primaryConnection - primary connection, used to discover shards
 * @param {Function[]} runCmdFns - array of (db) => commandResult, one op targeting each shard
 * @param {Function} getQueryStatsFn - (db) => queryStats array for the given shard db
 * @param {Function} assertQueryStatsMetricsFn - (entry) => void
 */
export function assertCommandRecordedOnShardsExceptRouter(
    primaryConn,
    runCmdFns,
    getQueryStatsFn,
    assertQueryStatsMetricsFn,
) {
    // Retry on transient errors: during a rolling restart a node can be briefly unavailable.
    const db = getDB(primaryConn);
    assert.soonNoExcept(() => {
        applyOnShardServers(primaryConn, (conn) => {
            resetQueryStatsStore(conn, "1MB");
        });

        for (const runCmdFn of runCmdFns) {
            assert.commandWorked(runCmdFn(db));
        }

        applyOnShardServers(primaryConn, (conn) => {
            const db = getDB(conn);
            const queryStats = getQueryStatsFn(db);
            assert.neq(queryStats, [], "Expected query stats entries on shard, but found none");
            assert.eq(queryStats.length, 1, "Expected exactly one query stats entry on shard");
            assertQueryStatsMetricsFn(queryStats[0]);
        });

        return true;
    });
}

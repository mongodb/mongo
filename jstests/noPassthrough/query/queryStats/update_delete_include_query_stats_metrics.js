/**
 * Tests that includeQueryStatsMetricsForOpIndex is processed correctly for write commands (update
 * and delete), and that query stats metrics are included in the command response when requested.
 *
 * Delete tests run only when featureFlagQueryStatsDelete is enabled.
 *
 * @tags: [requires_fcv_90]
 */
import {after, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {
    assertAggregatedMetricsSingleExec,
    getQueryExecMetrics,
    getQueryStats,
    getQueryStatsDeleteCmd,
    getQueryStatsUpdateCmd,
    resetQueryStatsStore,
} from "jstests/libs/query/query_stats_utils.js";
import {resetQueryStatsCollection} from "jstests/libs/query/query_stats_write_cmd_utils.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const collName = jsTestName();

function enableQueryStatsSampling(conn) {
    assert.commandWorked(
        conn.adminCommand({setParameter: 1, internalQueryStatsWriteCmdSampleRate: 1}),
    );
}

function disableQueryStatsSampling(conn) {
    assert.commandWorked(
        conn.adminCommand({setParameter: 1, internalQueryStatsWriteCmdSampleRate: 0}),
    );
}

/**
 * Spec object describing how to build and validate a write command for a single command type.
 *
 *   featureFlag       - Optional flag name; tests skip at runtime when this flag is disabled.
 *   getQueryStats     - Function(conn, opts) that fetches query stats for this command type.
 *   makeCmd(opIndex)  - Returns the write command object with includeQueryStatsMetricsForOpIndex
 *                       set to 'opIndex'. The command should match all 8 docs in the collection.
 *   makeShardCmd(opIndex) - Returns the inner command for _shardsvrCoordinateMultiUpdate.
 *   expectedWrites    - Expected writes metrics object for a single execution over 8 docs.
 *   keysInserted/keysDeleted - Expected index-key maintenance counts for a single execution. The
 *                       shard reports these back via the singleWriteResult metrics, so the
 *                       router-side entry matches the standalone counts.
 *   verifyShardMetrics(metrics) - Asserts the per-op metrics returned from the primary shard.
 */
const updateSpec = {
    featureFlag: null,
    getQueryStats: getQueryStatsUpdateCmd,
    makeCmd: (opIndex) => ({
        update: collName,
        updates: [
            {
                q: {},
                u: {$set: {v: "newValue", documentUpdated: true, count: 42}},
                multi: true,
                includeQueryStatsMetricsForOpIndex: NumberInt(opIndex),
            },
        ],
        comment: "running modifier update",
    }),
    makeShardCmd: (opIndex) => ({
        update: collName,
        updates: [
            {
                q: {v: 1},
                u: {$set: {points: 50}},
                multi: true,
                includeQueryStatsMetricsForOpIndex: NumberInt(opIndex),
            },
        ],
    }),
    expectedWrites: {
        nMatched: 8,
        nUpserted: 0,
        nModified: 8,
        nDeleted: 0,
        nInserted: 0,
        nUpdateOps: 1,
        nDeleteOps: 0,
    },
    // The update modifies only non-indexed fields, so no index keys are inserted or deleted.
    keysInserted: 0,
    keysDeleted: 0,
    verifyShardMetrics: (metrics) => {
        assert.eq(metrics.nModified, 1);
        assert.eq(metrics.nMatched, 1);
    },
};

const deleteSpec = {
    featureFlag: "featureFlagQueryStatsDelete",
    getQueryStats: getQueryStatsDeleteCmd,
    makeCmd: (opIndex) => ({
        delete: collName,
        deletes: [{q: {}, limit: 0, includeQueryStatsMetricsForOpIndex: NumberInt(opIndex)}],
        comment: "running delete with metrics",
    }),
    makeShardCmd: (opIndex) => ({
        delete: collName,
        deletes: [{q: {v: 1}, limit: 0, includeQueryStatsMetricsForOpIndex: NumberInt(opIndex)}],
    }),
    expectedWrites: {
        nMatched: 0,
        nUpserted: 0,
        nModified: 0,
        nDeleted: 8,
        nInserted: 0,
        nUpdateOps: 0,
        nDeleteOps: 1,
    },
    // Deleting all 8 documents removes one _id index key each.
    keysInserted: 0,
    keysDeleted: 8,
    verifyShardMetrics: (metrics) => {
        assert.eq(metrics.nDeleted, 1);
    },
};

/**
 * Runs the command built by spec.makeCmd(opIndex) and verifies:
 *   - Standalone: queryStatsMetrics is returned in the command response with the right opIndex.
 *   - Router:     queryStatsMetrics is NOT returned (mongos ignores the client-side field).
 *   - If sampling is enabled, verifies the aggregated metrics in the query stats store.
 */
function runIncludeMetricsTest(testDB, opIndex, isStandalone, enabledSampling, spec) {
    if (spec.featureFlag && !FeatureFlagUtil.isEnabled(testDB, spec.featureFlag)) {
        jsTest.log.info(
            "Skipping runIncludeMetricsTest because this feature flag is off: ",
            spec.featureFlag,
        );
        return;
    }

    const res = assert.commandWorked(testDB.runCommand(spec.makeCmd(opIndex)));

    if (isStandalone) {
        assert(res.hasOwnProperty("queryStatsMetrics"), res);
        assert.eq(res.queryStatsMetrics.length, 1, res);
        assert.eq(res.queryStatsMetrics[0].originalOpIndex, opIndex, res);
    } else {
        // Router ignores includeQueryStatsMetricsForOpIndex from the client.
        assert(!res.hasOwnProperty("queryStatsMetrics"), res);
    }

    if (enabledSampling) {
        const entry = spec.getQueryStats(testDB.getMongo(), {collName})[0];
        // TODO SERVER-128278 remove special handling for deletes on sharded clusters. On sharded
        // clusters, docsExamined may exceed 8 (the number of documents in the collection) but should never exceed double.
        const isShardedDelete = !isStandalone && spec.featureFlag !== null;
        const docsExamined = isShardedDelete
            ? getQueryExecMetrics(entry.metrics).docsExamined.sum
            : 8;
        if (isShardedDelete) {
            assert.gte(docsExamined, 8, "docsExamined should be >= 8 for sharded delete", {entry});
            assert.lte(docsExamined, 16, "docsExamined should be <= 16 for sharded delete", {
                entry,
            });
        }
        // The index-key maintenance counts are recorded on the shard and propagated back to mongos
        // via the singleWriteResult metrics, so the router-side entry matches the standalone counts.
        const expectedWrites = {
            ...spec.expectedWrites,
            keysInserted: spec.keysInserted,
            keysDeleted: spec.keysDeleted,
        };
        // We validate docsExamined above, for assertAggregatedMetricsSingleExec to pass we will pass in the actual docsExamined value.
        assertAggregatedMetricsSingleExec(entry, {
            keysExamined: 0,
            docsExamined: docsExamined,
            hasSortStage: false,
            usedDisk: false,
            fromMultiPlanner: false,
            fromPlanCache: false,
            writes: expectedWrites,
        });
    } else {
        assert.eq(getQueryStats(testDB.getMongo(), {collName}), []);
    }
}

function registerIncludeMetricsTests(getConn, getDB, isStandalone, spec) {
    describe(`Enable sampling (${spec.featureFlag ?? "update"})`, () => {
        before(() => enableQueryStatsSampling(getConn()));
        beforeEach(() => resetQueryStatsStore(getConn(), "1MB"));

        it(`opIndex=0 is returned in response`, () =>
            runIncludeMetricsTest(getDB(), 0, isStandalone, true, spec));
        it(`opIndex=42 is returned in response`, () =>
            runIncludeMetricsTest(getDB(), 42, isStandalone, true, spec));
    });

    describe(`Disable sampling (${spec.featureFlag ?? "update"})`, () => {
        before(() => disableQueryStatsSampling(getConn()));

        it(`opIndex=0: metrics absent from query stats store`, () =>
            runIncludeMetricsTest(getDB(), 0, isStandalone, false, spec));
        it(`opIndex=42: metrics absent from query stats store`, () =>
            runIncludeMetricsTest(getDB(), 42, isStandalone, false, spec));
    });
}

/**
 * Sends the command directly to the primary shard via _shardsvrCoordinateMultiUpdate and verifies
 * that the shard returns queryStatsMetrics with the right opIndex and write metrics.
 */
function runPrimaryShardTest(fixture, testDB, opIndex, spec) {
    if (spec.featureFlag && !FeatureFlagUtil.isEnabled(testDB, spec.featureFlag)) {
        jsTest.log.info(
            "Skipping runPrimaryShardTest because this feature flag is disabled: ",
            spec.featureFlag,
        );
        return;
    }

    const dbName = testDB.getName();
    const databaseVersion = assert.commandWorked(
        fixture.s.adminCommand({getDatabaseVersion: dbName}),
    ).dbVersion;
    const res = fixture.getPrimaryShard(dbName).adminCommand({
        _shardsvrCoordinateMultiUpdate: `${dbName}.${collName}`,
        uuid: UUID(),
        databaseVersion,
        command: spec.makeShardCmd(opIndex),
    });
    assert.commandWorked(res);
    assert(res.result.hasOwnProperty("queryStatsMetrics"), res);
    assert.eq(res.result.queryStatsMetrics.length, 1, res);
    assert.eq(res.result.queryStatsMetrics[0].originalOpIndex, opIndex, res);
    spec.verifyShardMetrics(res.result.queryStatsMetrics[0].metrics);
}

describe("Standalone", () => {
    let conn;
    let testDB;

    before(() => {
        conn = MongoRunner.runMongod();
        testDB = conn.getDB("test");
        resetQueryStatsCollection(testDB[collName]);
    });

    beforeEach(() => {
        resetQueryStatsCollection(testDB[collName]);
        resetQueryStatsStore(testDB, "1MB");
    });

    after(() => MongoRunner.stopMongod(conn));

    registerIncludeMetricsTests(
        () => conn,
        () => testDB,
        true,
        updateSpec,
    );
    registerIncludeMetricsTests(
        () => conn,
        () => testDB,
        true,
        deleteSpec,
    );
});

describe("Sharded", () => {
    let st, testDB;

    before(() => {
        st = new ShardingTest({shards: 2});
        testDB = st.s.getDB("test");
        resetQueryStatsCollection(testDB[collName]);
        st.shardColl(testDB[collName], {_id: 1}, {_id: 1});
    });

    beforeEach(() => {
        resetQueryStatsCollection(testDB[collName]);
        resetQueryStatsStore(st.s, "1MB");
    });

    after(() => st.stop());

    for (const pauseMigrations of [true, false]) {
        describe(`pauseMigrationsDuringMultiUpdates=${pauseMigrations}`, () => {
            before(() => {
                assert.commandWorked(
                    st.s.adminCommand({
                        setClusterParameter: {
                            pauseMigrationsDuringMultiUpdates: {enabled: pauseMigrations},
                        },
                    }),
                );
                assert.commandWorked(
                    st.s.adminCommand({getClusterParameter: "pauseMigrationsDuringMultiUpdates"}),
                );
            });

            registerIncludeMetricsTests(
                () => st.s,
                () => testDB,
                false, // isStandalone
                updateSpec,
            );
            registerIncludeMetricsTests(
                () => st.s,
                () => testDB,
                false, // isStandalone
                deleteSpec,
            );

            describe("_shardsvrCoordinateMultiUpdate (update)", () => {
                before(() => enableQueryStatsSampling(st.s));

                it("op 0 returns metrics from primary shard", () =>
                    runPrimaryShardTest(st, testDB, 0, updateSpec));
                it("op 42 returns metrics from primary shard", () =>
                    runPrimaryShardTest(st, testDB, 42, updateSpec));
            });

            describe("_shardsvrCoordinateMultiUpdate (delete)", () => {
                before(() => enableQueryStatsSampling(st.s));

                it("op 0 returns metrics from primary shard", () =>
                    runPrimaryShardTest(st, testDB, 0, deleteSpec));
                it("op 42 returns metrics from primary shard", () =>
                    runPrimaryShardTest(st, testDB, 42, deleteSpec));
            });
        });
    }
});

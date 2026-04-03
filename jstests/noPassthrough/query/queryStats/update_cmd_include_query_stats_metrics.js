/**
 * This test confirms that includeQueryStatsMetricsForOpIndex field is properly processed for update
 * command and the query stats metrics are included in the command response when requested.
 *
 * @tags: [requires_fcv_90]
 */
import {after, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {
    assertAggregatedMetricsSingleExec,
    getQueryStats,
    getQueryStatsUpdateCmd,
    resetQueryStatsStore,
} from "jstests/libs/query/query_stats_utils.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const collName = jsTestName();

function enableQueryStatsSampling(conn) {
    assert.commandWorked(conn.adminCommand({setParameter: 1, internalQueryStatsWriteCmdSampleRate: 1}));
}

function disableQueryStatsSampling(conn) {
    assert.commandWorked(conn.adminCommand({setParameter: 1, internalQueryStatsWriteCmdSampleRate: 0}));
}

function runIncludeMetricsTest(testDB, opIndex, isStandalone = true, enabledSampling = true) {
    const modifierUpdateCommandObj = {
        update: collName,
        updates: [
            {
                q: {}, // Should match all docs in collection.
                u: {$set: {v: "newValue", documentUpdated: true, count: 42}},
                multi: true,
                includeQueryStatsMetricsForOpIndex: NumberInt(opIndex),
            },
        ],
        comment: "running modifier update!!",
    };

    let res = testDB.runCommand(modifierUpdateCommandObj);

    assert.commandWorked(res);
    if (isStandalone) {
        // For standalone, the command should return query stats metrics directly.
        assert(res.hasOwnProperty("queryStatsMetrics"), res);
        assert.eq(res.queryStatsMetrics.length, 1, res);
        assert.eq(res.queryStatsMetrics[0].originalOpIndex, opIndex, res);
    } else {
        // For router, the command ignore the 'includeQueryStatsMetricsForOpIndex' field and
        // should not return query stats metrics.
        assert(!res.hasOwnProperty("queryStatsMetrics"), res);
    }

    if (enabledSampling) {
        const entry = getQueryStatsUpdateCmd(testDB.getMongo(), {collName: collName})[0];
        assertAggregatedMetricsSingleExec(entry, {
            keysExamined: 0,
            docsExamined: 8,
            hasSortStage: false,
            usedDisk: false,
            fromMultiPlanner: false,
            fromPlanCache: false,
            writes: {nMatched: 8, nUpserted: 0, nModified: 8, nDeleted: 0, nInserted: 0, nUpdateOps: 1},
        });
    } else {
        const entry = getQueryStats(testDB.getMongo(), {collName: collName});
        assert.eq(entry, [], entry);
    }
}

function runPrimaryShardTest(fixture, testDB, opIndex) {
    const dbName = testDB.getName();
    const databaseVersion = assert.commandWorked(fixture.s.adminCommand({getDatabaseVersion: dbName})).dbVersion;
    const conn = fixture.getPrimaryShard(dbName);
    const res = conn.adminCommand({
        _shardsvrCoordinateMultiUpdate: `${dbName}.${collName}`,
        uuid: UUID(),
        databaseVersion,
        command: {
            update: collName,
            updates: [
                {
                    q: {v: 1},
                    u: {$set: {points: 50}},
                    multi: true,
                    includeQueryStatsMetricsForOpIndex: NumberInt(opIndex),
                },
            ],
        },
    });
    assert.commandWorked(res);
    assert(res.result.hasOwnProperty("queryStatsMetrics"), res);
    assert.eq(res.result.queryStatsMetrics.length, 1, res);
    assert.eq(res.result.queryStatsMetrics[0].originalOpIndex, opIndex, res);
    const metrics = res.result.queryStatsMetrics[0].metrics;
    assert.eq(metrics.nModified, 1, res);
    assert.eq(metrics.nMatched, 1, res);
}

function resetCollection(coll) {
    coll.drop();
    assert.commandWorked(coll.insert([{v: 1}, {v: 2}, {v: 3}, {v: 4}, {v: 5}, {v: 6}, {v: 7}, {v: 8}]));
}

describe("Standalone", () => {
    let conn;
    let testDB;

    before(() => {
        conn = MongoRunner.runMongod();
        testDB = conn.getDB("test");
        testDB[collName].drop();
        resetCollection(testDB[collName]);
    });

    beforeEach(() => {
        resetCollection(testDB[collName]);
        resetQueryStatsStore(testDB, "1MB");
    });

    after(() => MongoRunner.stopMongod(conn));

    describe("Enable sampling", () => {
        before(() => enableQueryStatsSampling(conn));

        // Setting an opIndex value 0 which collides with the update op's position inside the update batch.
        it("should work when includeQueryStatsMetricsForOpIndex = 0 is requested", () =>
            runIncludeMetricsTest(testDB, 0, true, true));

        // Setting an arbitrary opIndex value 42 which differs from the update op's position inside the update batch.
        it("should work when includeQueryStatsMetricsForOpIndex = 42 is requested", () =>
            runIncludeMetricsTest(testDB, 42, true, true));
    });
    describe("Disable sampling", () => {
        before(() => disableQueryStatsSampling(conn));

        it("should work when includeQueryStatsMetricsForOpIndex = 0 is requested", () =>
            runIncludeMetricsTest(testDB, 0, true, false));

        it("should work when includeQueryStatsMetricsForOpIndex = 42 is requested", () =>
            runIncludeMetricsTest(testDB, 42, true, false));
    });
});

describe("Sharded", () => {
    let fixture;
    let testDB;

    before(() => {
        const st = new ShardingTest({shards: 2});
        testDB = st.s.getDB("test");
        st.shardColl(testDB[collName], {_id: 1}, {_id: 1});
        fixture = st;
        resetCollection(testDB[collName]);
    });

    beforeEach(() => {
        resetCollection(testDB[collName]);
        resetQueryStatsStore(fixture.s, "1MB");
    });

    after(() => fixture.stop());

    /**
     * Tests for triggering mongos to dispatch update comands using _shardsvrCoordinateMultiUpdate to the primary shard
     * and verifying that mongos processes the query stats metrics sent from the primary shard and stores them in the
     * query stats store.
     */
    describe("Enable pauseMigrationsDuringMultiUpdates", () => {
        before(() => {
            assert.commandWorked(
                fixture.s.adminCommand({setClusterParameter: {pauseMigrationsDuringMultiUpdates: {enabled: true}}}),
            );
            // getClusterParamter will refresh the cluster parameter cache so mongos is able to detect that
            // pauseMigrationsDuringMultiUpdates is enabled.
            assert.commandWorked(fixture.s.adminCommand({getClusterParameter: "pauseMigrationsDuringMultiUpdates"}));
        });

        describe("Enable sampling", () => {
            before(() => enableQueryStatsSampling(fixture.s));

            it("should work when includeQueryStatsMetricsForOpIndex = 0 is requested", () =>
                runIncludeMetricsTest(testDB, 0, false, true));

            it("should work when includeQueryStatsMetricsForOpIndex = 42 is requested", () =>
                runIncludeMetricsTest(testDB, 42, false, true));

            it("should return metrics for op 0 for _shardsvrCoordinateMultiUpdate", () =>
                runPrimaryShardTest(fixture, testDB, 0));

            it("should return metrics for op 42 for _shardsvrCoordinateMultiUpdate", () =>
                runPrimaryShardTest(fixture, testDB, 42));
        });

        describe("Disable sampling", () => {
            before(() => disableQueryStatsSampling(fixture.s));

            it("should work when includeQueryStatsMetricsForOpIndex = 0 is requested", () =>
                runIncludeMetricsTest(testDB, 0, false, false));

            it("should work when includeQueryStatsMetricsForOpIndex = 42 is requested", () =>
                runIncludeMetricsTest(testDB, 42, false, false));

            it("should return metrics for op 0 for _shardsvrCoordinateMultiUpdate", () =>
                runPrimaryShardTest(fixture, testDB, 0));

            it("should return metrics for op 42 for _shardsvrCoordinateMultiUpdate", () =>
                runPrimaryShardTest(fixture, testDB, 42));
        });
    });

    describe("Disable pauseMigrationsDuringMultiUpdates", () => {
        before(() => {
            assert.commandWorked(
                fixture.s.adminCommand({setClusterParameter: {pauseMigrationsDuringMultiUpdates: {enabled: false}}}),
            );
            // getClusterParamter will refresh the cluster parameter cache.
            assert.commandWorked(fixture.s.adminCommand({getClusterParameter: "pauseMigrationsDuringMultiUpdates"}));
        });

        describe("Enable sampling", () => {
            before(() => enableQueryStatsSampling(fixture.s));

            it("should work when includeQueryStatsMetricsForOpIndex = 0 is requested", () =>
                runIncludeMetricsTest(testDB, 0, false, true));

            it("should work when includeQueryStatsMetricsForOpIndex = 42 is requested", () =>
                runIncludeMetricsTest(testDB, 42, false, true));

            it("should return metrics for op 0 for _shardsvrCoordinateMultiUpdate", () =>
                runPrimaryShardTest(fixture, testDB, 0));

            it("should return metrics for op 42 for _shardsvrCoordinateMultiUpdate", () =>
                runPrimaryShardTest(fixture, testDB, 42));
        });

        describe("Disable sampling", () => {
            before(() => disableQueryStatsSampling(fixture.s));

            it("should work when includeQueryStatsMetricsForOpIndex = 0 is requested", () =>
                runIncludeMetricsTest(testDB, 0, false, false));

            it("should work when includeQueryStatsMetricsForOpIndex = 42 is requested", () =>
                runIncludeMetricsTest(testDB, 42, false, false));

            it("should return metrics for op 0 for _shardsvrCoordinateMultiUpdate", () =>
                runPrimaryShardTest(fixture, testDB, 0));

            it("should return metrics for op 42 for _shardsvrCoordinateMultiUpdate", () =>
                runPrimaryShardTest(fixture, testDB, 42));
        });
    });
});

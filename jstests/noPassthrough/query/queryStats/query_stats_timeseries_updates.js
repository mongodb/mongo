/**
 * Test that query stats properly handles time-series collections:
 * 1. Inserts on time-series collections should NOT be recorded as update commands in query stats.
 * 2. Explicit updates on time-series collections should be properly recorded in query stats.
 *
 * @tags: [
 *   requires_fcv_90,
 * ]
 */
import {after, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {
    assertAggregatedMetricsSingleExec,
    assertExpectedResults,
    getQueryStatsUpdateCmd,
    resetQueryStatsStore,
} from "jstests/libs/query/query_stats_utils.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const timeField = "time";
const metaField = "meta";

function createTimeseriesCollectionWithData(testDB, collName) {
    const coll = testDB[collName];
    coll.drop();
    assert.commandWorked(testDB.createCollection(collName, {timeseries: {timeField: timeField, metaField: metaField}}));
    assert.commandWorked(coll.insert({[timeField]: ISODate("2021-05-18T00:00:00.000Z"), v: 1, [metaField]: "a"}));
    assert.commandWorked(coll.insert({[timeField]: ISODate("2021-05-18T01:00:00.000Z"), v: 2, [metaField]: "a"}));
    assert.commandWorked(coll.insert({[timeField]: ISODate("2021-05-18T02:00:00.000Z"), v: 3, [metaField]: "b"}));
    return coll;
}

function testInsertNotRecorded(testDB, coll, collName) {
    assert.commandWorked(coll.insert({[timeField]: ISODate("2021-05-18T03:00:00.000Z"), v: 4, [metaField]: "c"}));

    const updateStats = getQueryStatsUpdateCmd(testDB, {collName: collName});
    assert.eq(
        0,
        updateStats.length,
        "Time-series inserts should NOT be recorded as update commands. Found: " + tojson(updateStats),
    );
}

function testBulkInsertNotRecorded(testDB, coll, collName) {
    assert.commandWorked(
        testDB.adminCommand({
            bulkWrite: 1,
            ops: [
                {
                    insert: 0,
                    document: {[timeField]: ISODate("2021-05-18T04:00:00.000Z"), v: 5, [metaField]: "c"},
                },
                {
                    insert: 0,
                    document: {[timeField]: ISODate("2021-05-18T05:00:00.000Z"), v: 6, [metaField]: "c"},
                },
            ],
            nsInfo: [{ns: coll.getFullName()}],
        }),
    );

    const updateStats = getQueryStatsUpdateCmd(testDB, {collName: collName});
    assert.eq(
        0,
        updateStats.length,
        "Time-series bulk inserts should NOT be recorded as update commands. Found: " + tojson(updateStats),
    );
}

// On standalone mongod, the timeseries rewrite transforms the update into an internal operation,
// so it should not appear in query stats.
function testMetaFieldUpdateNotRecordedOnMongod(testDB, coll, collName) {
    assert.commandWorked(
        coll.insertMany([
            {[timeField]: ISODate("2021-05-18T06:00:00.000Z"), v: 7, [metaField]: "updateMetaField"},
            {[timeField]: ISODate("2021-05-18T07:00:00.000Z"), v: 8, [metaField]: "updateMetaField"},
        ]),
    );
    const updateResult = testDB.runCommand({
        update: collName,
        updates: [
            {
                q: {[metaField]: "updateMetaField"},
                u: {$set: {[metaField]: "didUpdateMetaField"}},
                multi: true,
            },
        ],
    });
    assert.commandWorked(updateResult);

    const updateStats = getQueryStatsUpdateCmd(testDB, {collName: collName});
    assert.eq(0, updateStats.length);
}

// On mongos, the update is forwarded to the shard as a regular update command, so it should
// appear in query stats with correct metrics.
function testMetaFieldUpdateRecordedOnMongos(testDB, coll, collName) {
    assert.commandWorked(
        coll.insertMany([
            {[timeField]: ISODate("2021-05-18T06:00:00.000Z"), v: 7, [metaField]: "updateMetaField"},
            {[timeField]: ISODate("2021-05-18T07:00:00.000Z"), v: 8, [metaField]: "updateMetaField"},
        ]),
    );
    const updateResult = testDB.runCommand({
        update: collName,
        updates: [
            {
                q: {[metaField]: "updateMetaField"},
                u: {$set: {[metaField]: "didUpdateMetaField"}},
                multi: true,
            },
        ],
    });
    assert.commandWorked(updateResult);

    const updateStats = getQueryStatsUpdateCmd(testDB, {collName: collName});
    assert.eq(1, updateStats.length);
    const entry = updateStats[0];

    assertAggregatedMetricsSingleExec(entry, {
        keysExamined: 2,
        // Extra documents are examined due to write_stage_common::ensureStillMatches
        docsExamined: 4,
        hasSortStage: false,
        usedDisk: false,
        fromMultiPlanner: false,
        fromPlanCache: false,
        writes: {nMatched: 2, nUpserted: 0, nModified: 2, nDeleted: 0, nInserted: 0, nUpdateOps: 1},
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

describe("time-series query stats (standalone)", function () {
    const collName = jsTestName() + "_standalone";
    let conn;
    let testDB;
    let coll;

    before(function () {
        conn = MongoRunner.runMongod({
            setParameter: {internalQueryStatsRateLimit: -1, internalQueryStatsWriteCmdSampleRate: 1},
        });
        testDB = conn.getDB("test");
        coll = createTimeseriesCollectionWithData(testDB, collName);
    });

    after(function () {
        MongoRunner.stopMongod(conn);
    });

    beforeEach(function () {
        resetQueryStatsStore(conn, "1MB");
    });

    it("should not record time-series inserts as update commands", function () {
        testInsertNotRecorded(testDB, coll, collName);
    });

    it("should not record bulk time-series inserts as update commands", function () {
        testBulkInsertNotRecorded(testDB, coll, collName);
    });

    // TODO SERVER-119643 Combine Standalone test with sharded test.
    it("shouldn't record explicit meta field update in query stats on mongod", function () {
        testMetaFieldUpdateNotRecordedOnMongod(testDB, coll, collName);
    });
});

describe("time-series query stats (sharded)", function () {
    let st;
    let testDB;

    before(function () {
        const queryStatsParams = {
            internalQueryStatsRateLimit: -1,
            internalQueryStatsWriteCmdSampleRate: 1,
        };
        const tsUpdatesParam = {featureFlagTimeseriesUpdatesSupport: true};
        st = new ShardingTest({
            shards: 2,
            mongosOptions: {setParameter: Object.merge(queryStatsParams, tsUpdatesParam)},
            rsOptions: {setParameter: Object.merge(queryStatsParams, tsUpdatesParam)},
            configOptions: {setParameter: tsUpdatesParam},
        });
        testDB = st.s.getDB("test");
    });

    after(function () {
        st?.stop();
    });

    beforeEach(function () {
        resetQueryStatsStore(st.s, "1MB");
    });

    describe("inserts and updates", function () {
        const collName = jsTestName() + "_ts";
        let coll;

        before(function () {
            coll = createTimeseriesCollectionWithData(testDB, collName);
        });

        it("should not record time-series inserts as update commands", function () {
            testInsertNotRecorded(testDB, coll, collName);
        });

        it("should not record bulk time-series inserts as update commands", function () {
            testBulkInsertNotRecorded(testDB, coll, collName);
        });

        it("should record explicit meta field update in query stats", function () {
            testMetaFieldUpdateRecordedOnMongos(testDB, coll, collName);
        });
    });

    // Retryable updates on sharded time-series collections use a dedicated dispatch path
    // (WriteType::TimeseriesRetryableUpdate in legacy, AnalysisType::kInternalTransaction in
    // UWE). The update is wrapped in an internal transaction to guarantee exactly-once semantics.
    //
    // TODO SERVER-121266 We don't correctly handle this case yet. Unskip this test when we do.
    describe.skip("retryable updates", function () {
        const collName = jsTestName() + "_retryable_ts";
        let coll;

        before(function () {
            coll = testDB[collName];
            assert.commandWorked(
                testDB.createCollection(collName, {
                    timeseries: {timeField: timeField, metaField: metaField},
                }),
            );
            assert.commandWorked(
                st.s.adminCommand({
                    shardCollection: coll.getFullName(),
                    key: {[metaField]: 1},
                }),
            );
            // Each test case uses a dedicated meta value to avoid interference, since updating
            // the meta field changes the document and we don't reset data between tests.
            assert.commandWorked(
                coll.insertMany([
                    {[timeField]: ISODate("2021-05-18T00:00:00Z"), v: 1, [metaField]: "retryable_basic"},
                    {[timeField]: ISODate("2021-05-18T01:00:00Z"), v: 2, [metaField]: "retryable_dedup"},
                ]),
            );
        });

        it("should record query stats for a retryable updateOne on time-series meta field", function () {
            const lsid = {id: UUID()};

            const cmd = {
                update: collName,
                updates: [
                    {
                        q: {[metaField]: "retryable_basic"},
                        u: {$set: {[metaField]: "retryable_basic_done"}},
                        multi: false,
                    },
                ],
                lsid: lsid,
                txnNumber: NumberLong(1),
            };

            const result = assert.commandWorked(testDB.runCommand(cmd));
            assert.eq(result.nModified, 1);

            const entries = getQueryStatsUpdateCmd(st.s, {collName: collName});
            assert.eq(entries.length, 1, "Expected 1 query stats entry: " + tojson(entries));
            assert.eq(entries[0].metrics.execCount, 1);

            // TODO SERVER-121266 The internal transaction path does not propagate any execution
            // or write metrics from the shard back to the router. All of docsExamined,
            // keysExamined, nMatched, and nModified are reported as 0, which is incorrect.
            assertAggregatedMetricsSingleExec(entries[0], {
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

        it("retrying the same retryable time-series update should not double-count", function () {
            const lsid = {id: UUID()};
            const txnNumber = NumberLong(1);

            const cmd = {
                update: collName,
                updates: [
                    {
                        q: {[metaField]: "retryable_dedup"},
                        u: {$set: {[metaField]: "retryable_dedup_done"}},
                        multi: false,
                    },
                ],
                lsid: lsid,
                txnNumber: txnNumber,
            };

            // Initial execution.
            const result = assert.commandWorked(testDB.runCommand(cmd));
            assert.eq(result.nModified, 1);

            let entries = getQueryStatsUpdateCmd(st.s, {collName: collName});
            assert.eq(entries.length, 1, "Expected 1 entry after initial exec: " + tojson(entries));
            assert.eq(entries[0].metrics.execCount, 1);

            // Retry with the same lsid/txnNumber. The server will see this and not execute the
            // update a second time.
            const retryResult = assert.commandWorked(testDB.runCommand(cmd));
            assert.eq(retryResult.nModified, 1);

            entries = getQueryStatsUpdateCmd(st.s, {collName: collName});
            assert.eq(entries.length, 1, "Expected still 1 entry after retry: " + tojson(entries));
            // TODO SERVER-121266 The retry currently increments execCount because the internal
            // transaction path does not properly deduplicate retried statements for query stats.
            // Once fixed, this should assert that execCount does not increase.
            assert.eq(entries[0].metrics.execCount, 1, "execCount incremented on retry");
        });
    });
});

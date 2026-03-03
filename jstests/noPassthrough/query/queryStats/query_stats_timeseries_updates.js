/**
 * Test that query stats properly handles time-series collections:
 * 1. Inserts on time-series collections should NOT be recorded as update commands in query stats.
 * 2. Explicit updates on time-series collections should be properly recorded in query stats.
 *
 * @tags: [
 *   featureFlagQueryStatsUpdateCommand,
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

const collName = jsTestName();
const timeField = "time";
const metaField = "meta";

/**
 * Runs the time-series query stats test suite against a specific topology.
 *
 * @param {string} topologyName - Name of the topology (e.g. "Standalone")
 * @param {Function} setupFn - Returns {fixture, conn}
 * @param {Function} teardownFn - Takes fixture and cleans up
 */
function runTimeseriesQueryStatsTests(topologyName, setupFn, teardownFn) {
    describe(`time-series query stats (${topologyName})`, function () {
        let fixture;
        let testDB;
        let coll;
        const timeseriesOptions = {timeField: timeField, metaField: metaField};

        before(function () {
            const setupRes = setupFn();
            fixture = setupRes.fixture;
            testDB = setupRes.testDB;
            coll = testDB[collName];

            // Create the time-series collection.
            coll.drop();
            assert.commandWorked(testDB.createCollection(collName, {timeseries: timeseriesOptions}));

            // Insert initial documents.
            assert.commandWorked(
                coll.insert({[timeField]: ISODate("2021-05-18T00:00:00.000Z"), v: 1, [metaField]: "a"}),
            );
            assert.commandWorked(
                coll.insert({[timeField]: ISODate("2021-05-18T01:00:00.000Z"), v: 2, [metaField]: "a"}),
            );
            assert.commandWorked(
                coll.insert({[timeField]: ISODate("2021-05-18T02:00:00.000Z"), v: 3, [metaField]: "b"}),
            );
        });

        after(function () {
            if (fixture) {
                teardownFn(fixture);
            }
        });

        beforeEach(function () {
            resetQueryStatsStore(testDB.getMongo(), "1MB");
        });

        //Make sure inserts aren't recorded.
        it("should not record time-series inserts as update commands", function () {
            assert.commandWorked(
                coll.insert({[timeField]: ISODate("2021-05-18T03:00:00.000Z"), v: 4, [metaField]: "c"}),
            );

            const updateStats = getQueryStatsUpdateCmd(testDB, {collName: collName});
            assert.eq(
                0,
                updateStats.length,
                "Time-series inserts should NOT be recorded as update commands. Found: " + tojson(updateStats),
            );
        });

        it("should not record bulk time-series inserts as update commands", function () {
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
        });

        // TODO SERVER-119643 Combine Standalone test with sharded test.
        // Make sure timeseries updates are not being recorded. This is only for standalone since the timeseries rewrite happens on mongod.
        if (topologyName === "Standalone") {
            it("shouldn't record explicit meta field update in query stats on mongod", function () {
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
            });
        }

        if (topologyName === "Sharded") {
            it("should record explicit meta field update in query stats", function () {
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
                    docsExamined: 2,
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
            });
        }
    });
}

runTimeseriesQueryStatsTests(
    "Standalone",
    () => {
        const conn = MongoRunner.runMongod({
            setParameter: {internalQueryStatsRateLimit: -1, internalQueryStatsWriteCmdSampleRate: 1},
        });
        const testDB = conn.getDB("test");
        testDB[collName].drop();
        return {fixture: conn, testDB};
    },
    (fixture) => MongoRunner.stopMongod(fixture),
);

runTimeseriesQueryStatsTests(
    "Sharded",
    () => {
        const st = new ShardingTest({
            shards: 2,
            mongosOptions: {setParameter: {internalQueryStatsRateLimit: -1, internalQueryStatsWriteCmdSampleRate: 1}},
        });
        const testDB = st.s.getDB("test");
        st.shardColl(testDB[collName], {_id: 1}, {_id: 1});
        return {fixture: st, testDB};
    },
    (st) => st.stop(),
);

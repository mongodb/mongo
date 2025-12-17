/**
 * This test confirms that query stats store metrics fields for an update command (where the
 * update modification is specified as a replacement document or pipeline) are correct when
 * inserting a new query stats store entry.
 *
 * @tags: [featureFlagQueryStatsUpdateCommand]
 */
import {after, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {
    assertAggregatedMetricsSingleExec,
    assertExpectedResults,
    getLatestQueryStatsEntry,
} from "jstests/libs/query/query_stats_utils.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const collName = jsTestName();

/**
 * Run test suite against a specific topology.
 *
 * @param {string} topologyName - Name of the topology (e.g. "Standalone", "Sharded")
 * @param {Function} setupFn - Returns {fixture, testDB}
 * @param {Function} teardownFn - Takes {fixture} and cleans up
 */
function runUpdateCmdMetricsTests(topologyName, setupFn, teardownFn) {
    describe(`query stats update command metrics (${topologyName})`, function () {
        let fixture;
        let testDB;
        let coll;

        function resetCollection() {
            coll.drop();
            assert.commandWorked(coll.insert([{v: 1}, {v: 2}, {v: 3}, {v: 4}, {v: 5}, {v: 6}, {v: 7}, {v: 8}]));
        }

        before(function () {
            const setupRes = setupFn();
            fixture = setupRes.fixture;
            testDB = setupRes.testDB;
            coll = testDB[collName];

            resetCollection();
        });

        after(function () {
            if (fixture) {
                teardownFn(fixture);
            }
        });

        beforeEach(function () {
            resetCollection();
        });

        it("should record replacement update metrics", function () {
            const replacementUpdateCommandObj = {
                update: collName,
                updates: [
                    {
                        q: {$or: [{v: {$lt: 3}}, {v: {$eq: 4}}]},
                        u: {v: 1000, updated: true},
                        multi: false,
                    },
                ],
                comment: "running replacement update!!",
            };

            assert.commandWorked(testDB.runCommand(replacementUpdateCommandObj));

            const entry = getLatestQueryStatsEntry(testDB.getMongo(), {collName: coll.getName()});
            assert.eq(entry.key.queryShape.command, "update");

            assertAggregatedMetricsSingleExec(entry, {
                keysExamined: 0,
                docsExamined: 1,
                hasSortStage: false,
                usedDisk: false,
                fromMultiPlanner: false,
                fromPlanCache: false,
                writes: {nMatched: 1, nUpserted: 0, nModified: 1, nDeleted: 0, nInserted: 0},
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

        // Test that a simple _id query update generates the expected query stats.
        // Simple _id queries skip parsing during normal update processing (IDHACK
        // optimization), but should still record metrics correctly.
        it("should record simple _id update metrics", function () {
            // Insert a document with a specific _id to update.
            assert.commandWorked(coll.insert({_id: 999, v: 1}));

            const simpleIdUpdateCommandObj = {
                update: collName,
                updates: [{q: {_id: 999}, u: {_id: 999, v: 2000}, multi: false}],
                comment: "running update filtered on _id!!",
            };

            assert.commandWorked(testDB.runCommand(simpleIdUpdateCommandObj));

            const entry = getLatestQueryStatsEntry(testDB.getMongo(), {collName: coll.getName()});
            assert.eq(entry.key.queryShape.command, "update");

            assertAggregatedMetricsSingleExec(entry, {
                keysExamined: 1, // Should use _id index.
                docsExamined: 1,
                hasSortStage: false,
                usedDisk: false,
                fromMultiPlanner: false,
                fromPlanCache: false,
                writes: {nMatched: 1, nUpserted: 0, nModified: 1, nDeleted: 0, nInserted: 0},
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

            assert.commandWorked(coll.remove({_id: 999}));
        });

        it("should record modifier update metrics", function () {
            const modifierUpdateCommandObj = {
                update: collName,
                updates: [
                    {
                        q: {}, // Should match all docs in collection.
                        u: {$set: {v: "newValue", documentUpdated: true, count: 42}},
                        multi: true,
                    },
                ],
                comment: "running modifier update!!",
            };

            assert.commandWorked(testDB.runCommand(modifierUpdateCommandObj));

            const entry = getLatestQueryStatsEntry(testDB.getMongo(), {collName: coll.getName()});
            assert.eq(entry.key.queryShape.command, "update");

            assertAggregatedMetricsSingleExec(entry, {
                keysExamined: 0,
                docsExamined: 8,
                hasSortStage: false,
                usedDisk: false,
                fromMultiPlanner: false,
                fromPlanCache: false,
                writes: {nMatched: 8, nUpserted: 0, nModified: 8, nDeleted: 0, nInserted: 0},
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

        it("should record pipeline update metrics", function () {
            const pipelineUpdateCommandObj = {
                update: collName,
                updates: [
                    {
                        q: {}, // Should match all docs in collection.
                        u: [
                            {$set: {v: "$$newValue", pipelineUpdated: true, count: 42}},
                            {$unset: "oldField"},
                            {$replaceWith: {newDoc: "$$ROOT", timestamp: "$$NOW", processed: true}},
                        ],
                        c: {newValue: 3000},
                        multi: true,
                    },
                ],
                comment: "running pipeline update!!",
            };

            assert.commandWorked(testDB.runCommand(pipelineUpdateCommandObj));

            const entry = getLatestQueryStatsEntry(testDB.getMongo(), {collName: coll.getName()});
            assert.eq(entry.key.queryShape.command, "update");

            assertAggregatedMetricsSingleExec(entry, {
                keysExamined: 0,
                docsExamined: 8,
                hasSortStage: false,
                usedDisk: false,
                fromMultiPlanner: false,
                fromPlanCache: false,
                writes: {nMatched: 8, nUpserted: 0, nModified: 8, nDeleted: 0, nInserted: 0},
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
}

runUpdateCmdMetricsTests(
    "Standalone",
    () => {
        const conn = MongoRunner.runMongod({setParameter: {internalQueryStatsRateLimit: -1}});
        const testDB = conn.getDB("test");
        testDB[collName].drop();
        return {fixture: conn, testDB};
    },
    (fixture) => MongoRunner.stopMongod(fixture),
);

// TODO SERVER-112050 Enable this when we support sharded clusters for update.
describe.skip("Sharded", function () {
    runUpdateCmdMetricsTests(
        "Sharded",
        () => {
            const st = new ShardingTest({
                shards: 2,
                mongosOptions: {setParameter: {internalQueryStatsRateLimit: -1}},
            });
            const testDB = st.s.getDB("test");
            st.shardColl(testDB[collName], {_id: 1}, {_id: 1});
            return {fixture: st, testDB};
        },
        (st) => st.stop(),
    );
});

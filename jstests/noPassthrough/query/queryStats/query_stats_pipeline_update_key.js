/**
 * This test confirms that query stats store key fields for pipeline update commands
 * are properly nested and none are missing.
 *
 * @tags: [featureFlagQueryStatsUpdateCommand]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {
    queryShapeUpdateFieldsRequired,
    runCommandAndValidateQueryStats,
    updateKeyFieldsComplex,
    updateKeyFieldsRequired,
} from "jstests/libs/query/query_stats_utils.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const collName = jsTestName();

/**
 * Runs the test suite against a specific topology.
 *
 * @param {string} topologyName - Name of the topology (e.g. "Standalone", "Sharded")
 * @param {Function} setupFn - Returns {fixture, testDB}
 * @param {Function} teardownFn - Takes {fixture} and cleans up
 */
function runPipelineUpdateKeyTests(topologyName, setupFn, teardownFn) {
    describe(`query stats pipeline update key (${topologyName})`, function () {
        let fixture;
        let testDB;
        let coll;

        before(function () {
            const setupRes = setupFn();
            fixture = setupRes.fixture;
            testDB = setupRes.testDB;
            coll = testDB[collName];

            // Have to create an index for hint not to fail.
            assert.commandWorked(coll.createIndex({v: 1}));
        });

        after(function () {
            if (fixture) {
                teardownFn(fixture);
            }
        });

        it("should validate simple pipeline update key fields", function () {
            const pipelineUpdateCommandObjSimple = {
                update: collName,
                updates: [{q: {v: 3}, u: [{$set: {v: 4, pipelineUpdated: true}}]}],
            };

            runCommandAndValidateQueryStats({
                coll: coll,
                commandName: "update",
                commandObj: pipelineUpdateCommandObjSimple,
                shapeFields: queryShapeUpdateFieldsRequired,
                keyFields: updateKeyFieldsRequired,
            });
        });

        it("should validate complex pipeline update key fields", function () {
            const queryShapePipelineUpdateFieldsComplex = [...queryShapeUpdateFieldsRequired, "collation", "c"];
            const pipelineUpdateCommandObjComplex = {
                update: collName,
                updates: [
                    {
                        q: {v: {$gt: 5}},
                        u: [{$set: {v: "$$newValue", processed: true, timestamp: "$$now"}}],
                        c: {newValue: 100, now: new Date()},
                        multi: true,
                        upsert: false,
                        collation: {locale: "en_US", strength: 2},
                        hint: {"v": 1},
                    },
                ],
                ordered: false,
                bypassDocumentValidation: true,
                comment: "pipeline update test!!!",
                readConcern: {level: "local"},
                maxTimeMS: 50 * 1000,
                apiDeprecationErrors: false,
                apiVersion: "1",
                apiStrict: false,
                $readPreference: {"mode": "primary"},
            };

            runCommandAndValidateQueryStats({
                coll: coll,
                commandName: "update",
                commandObj: pipelineUpdateCommandObjComplex,
                shapeFields: queryShapePipelineUpdateFieldsComplex,
                keyFields: updateKeyFieldsComplex,
            });
        });
    });
}

runPipelineUpdateKeyTests(
    "Standalone",
    () => {
        const conn = MongoRunner.runMongod({setParameter: {internalQueryStatsRateLimit: -1}});
        const testDB = conn.getDB("test");
        testDB[collName].drop();
        return {fixture: conn, testDB};
    } /*setupFn*/,
    (fixture) => MongoRunner.stopMongod(fixture) /*teardownFn*/,
);

// TODO SERVER-112050 Enable this when we support sharded clusters for update.
describe.skip("Sharded", function () {
    runPipelineUpdateKeyTests(
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

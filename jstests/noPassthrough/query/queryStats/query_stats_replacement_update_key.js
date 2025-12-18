/**
 * This test confirms that query stats store key fields for a replacement update command are
 * properly nested and none are missing.
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
function runReplacementUpdateKeyTests(topologyName, setupFn, teardownFn) {
    describe(`query stats replacement update key (${topologyName})`, function () {
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

        it("should validate simple replacement update key fields", function () {
            const replacementUpdateCommandObjRequired = {
                update: collName,
                updates: [{q: {v: 1}, u: {v: 1, updated: true}}],
            };

            runCommandAndValidateQueryStats({
                coll: coll,
                commandName: "update",
                commandObj: replacementUpdateCommandObjRequired,
                shapeFields: queryShapeUpdateFieldsRequired,
                keyFields: updateKeyFieldsRequired,
            });
        });

        it("should validate complex replacement update key fields", function () {
            const queryShapeUpdateFieldsComplex = [...queryShapeUpdateFieldsRequired, "collation"];
            const replacementUpdateCommandObjComplex = {
                update: collName,
                updates: [
                    {
                        q: {v: {$gt: 2}},
                        u: {v: 10, processed: true, timestamp: new Date(), status: "completed"},
                        multi: false, // Note: replacement updates cannot use multi: true
                        upsert: false,
                        collation: {locale: "en_US", strength: 2},
                        hint: {"v": 1},
                    },
                ],
                ordered: false,
                bypassDocumentValidation: true,
                comment: "replacement update test!!!",
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
                commandObj: replacementUpdateCommandObjComplex,
                shapeFields: queryShapeUpdateFieldsComplex,
                keyFields: updateKeyFieldsComplex,
            });
        });

        it("should validate empty doc replacement update key fields", function () {
            const replacementUpdateCommandObjEmpty = {
                update: collName,
                updates: [{q: {v: 1}, u: {}}],
            };

            runCommandAndValidateQueryStats({
                coll: coll,
                commandName: "update",
                commandObj: replacementUpdateCommandObjEmpty,
                shapeFields: queryShapeUpdateFieldsRequired,
                keyFields: updateKeyFieldsRequired,
            });
        });
    });
}

runReplacementUpdateKeyTests(
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
    runReplacementUpdateKeyTests(
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

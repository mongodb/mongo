/**
 * This test confirms that query stats store key fields for delete commands are properly nested
 * and none are missing.
 *
 * @tags: [requires_fcv_90]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {
    deleteKeyFieldsComplex,
    deleteKeyFieldsRequired,
    queryShapeDeleteFieldsRequired,
    runCommandAndValidateQueryStats,
} from "jstests/libs/query/query_stats_utils.js";
import {
    shardedWriteCmdQueryStatsFixture,
    standaloneWriteCmdQueryStatsFixture,
} from "jstests/libs/query/query_stats_write_cmd_utils.js";

const collName = jsTestName();

function runDeleteKeyTests(topologyName, setupFn, teardownFn) {
    describe(`query stats delete key (${topologyName})`, function () {
        let fixture;
        let testDB;
        let coll;

        before(function () {
            const setupRes = setupFn();
            fixture = setupRes.fixture;
            testDB = setupRes.testDB;
            coll = testDB[collName];
            assert.commandWorked(coll.createIndex({v: 1}));
        });

        after(function () {
            if (fixture) {
                teardownFn(fixture);
            }
        });

        it("should validate simple delete key fields", function () {
            const deleteCommandObjSimple = {
                delete: collName,
                deletes: [{q: {v: 3}, limit: 1}],
            };

            runCommandAndValidateQueryStats({
                coll: coll,
                commandName: "delete",
                commandObj: deleteCommandObjSimple,
                shapeFields: queryShapeDeleteFieldsRequired,
                keyFields: deleteKeyFieldsRequired,
            });
        });

        it("should validate complex delete key fields with collation and hint", function () {
            const queryShapeDeleteFieldsComplex = [...queryShapeDeleteFieldsRequired, "collation"];

            const deleteCommandObjComplex = {
                delete: collName,
                deletes: [
                    {
                        q: {v: {$gt: 5}},
                        limit: 0,
                        hint: {v: 1},
                        collation: {locale: "en", strength: 2},
                    },
                ],
                comment: "complex delete",
                maxTimeMS: 5000,
                bypassDocumentValidation: true,
                readConcern: {level: "local"},
                writeConcern: {w: "majority", wtimeout: 5000},
                apiDeprecationErrors: false,
                apiVersion: "1",
                apiStrict: false,
                $readPreference: {"mode": "primary"},
            };

            runCommandAndValidateQueryStats({
                coll: coll,
                commandName: "delete",
                commandObj: deleteCommandObjComplex,
                shapeFields: queryShapeDeleteFieldsComplex,
                keyFields: deleteKeyFieldsComplex,
            });
        });
    });
}

const standalone = standaloneWriteCmdQueryStatsFixture();
runDeleteKeyTests("Standalone", standalone.setupFn, standalone.teardownFn);

const sharded = shardedWriteCmdQueryStatsFixture(collName, {moveChunk: false});
runDeleteKeyTests("Sharded", sharded.setupFn, sharded.teardownFn);

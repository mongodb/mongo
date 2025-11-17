/**
 * This test confirms that query stats store key fields for pipeline update commands
 * are properly nested and none are missing.
 *
 * @tags: [requires_fcv_83]
 */
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {
    runCommandAndValidateQueryStats,
    withQueryStatsEnabled,
    queryShapeUpdateFieldsRequired,
    updateKeyFieldsRequired,
    updateKeyFieldsComplex,
} from "jstests/libs/query/query_stats_utils.js";

const collName = jsTestName();

function testPipelineUpdateSimple(coll) {
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
}

function testPipelineUpdateComplex(coll) {
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
}

withQueryStatsEnabled(collName, (coll) => {
    const testDB = coll.getDB();

    if (testDB.getMongo().isMongos()) {
        // TODO SERVER-112050 Unskip this when we support sharded clusters for update.
        jsTest.log.info("Skipping update key validation test on sharded cluster.");
        return;
    }

    if (!FeatureFlagUtil.isPresentAndEnabled(testDB, "QueryStatsUpdateCommand")) {
        jsTest.log.info("Skipping update key validation because feature flag is not set.");
        return;
    }

    // Have to create an index for hint not to fail.
    assert.commandWorked(coll.createIndex({v: 1}));

    testPipelineUpdateSimple(coll);
    testPipelineUpdateComplex(coll);
});

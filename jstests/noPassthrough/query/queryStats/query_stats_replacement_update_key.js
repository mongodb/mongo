/**
 * This test confirms that query stats store key fields for a replacement update command are
 * properly nested and none are missing.
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

function testReplacementUpdateSimple(coll) {
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
}

function testReplacementUpdateComplex(coll) {
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

    testReplacementUpdateSimple(coll);
    testReplacementUpdateComplex(coll);
});

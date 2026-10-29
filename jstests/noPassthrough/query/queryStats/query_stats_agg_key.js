/**
 * This test confirms that query stats store key fields for an aggregate command are properly nested
 * and none are missing.
 * @tags: [requires_fcv_72]
 */
import {
    runCommandAndValidateQueryStats,
    withQueryStatsEnabled,
    getLatestQueryStatsEntry,
} from "jstests/libs/query/query_stats_utils.js";

const collName = jsTestName();
const aggregateCommandObj = {
    aggregate: collName,
    pipeline: [{"$out": "collOut"}],
    allowDiskUse: false,
    cursor: {batchSize: 2},
    maxTimeMS: 50 * 1000,
    bypassDocumentValidation: false,
    readConcern: {level: "local"},
    writeConcern: {w: 1},
    collation: {locale: "en_US", strength: 2},
    hint: {"v": 1},
    comment: "",
    let: {},
    apiDeprecationErrors: false,
    apiVersion: "1",
    apiStrict: false,
};

const queryShapeAggregateFields = [
    "cmdNs",
    "command",
    "pipeline",
    "allowDiskUse",
    "collation",
    "let",
];

// The outer fields not nested inside queryShape.
const queryStatsAggregateKeyFields = [
    "queryShape",
    "cursor",
    "maxTimeMS",
    "bypassDocumentValidation",
    "comment",
    "otherNss",
    "apiDeprecationErrors",
    "apiVersion",
    "apiStrict",
    "collectionType",
    "client",
    "hint",
    "readConcern",
    "writeConcern",
    "cursor.batchSize",
];

function validateSystemVariables(coll) {
    const collName = "system_var";
    const testDB = coll.getDB();
    let varColl = testDB[collName];
    varColl.drop();
    // Insert one document, so the aggregation has to do some work.
    assert.commandWorked(varColl.insertOne({a: 1}));
    assert.commandWorked(
        testDB.runCommand({
            aggregate: collName,
            pipeline: [{$addFields: {falseField: "$$REMOVE"}}],
            cursor: {},
        }),
    );
    const entry = getLatestQueryStatsEntry(testDB.getMongo(), {collName: collName});
    assert.eq(entry.key.queryShape.pipeline, [{$addFields: {falseField: "$$REMOVE"}}]);
}

// allowPartialResults is not supported for aggregations with write stages (e.g. $out), so it
// can't be included in 'aggregateCommandObj' above. Rather than initialize a new
// `aggregateCommandObj` that exercises a $match stage in the pipeline, this validation function
// narrows the test coverage to just the allowPartialResults behavior.
function validateAllowPartialResults(coll) {
    const testDB = coll.getDB();

    assert.commandWorked(
        testDB.runCommand({
            aggregate: coll.getName(),
            pipeline: [{$match: {v: 1}}],
            cursor: {},
            allowPartialResults: true,
        }),
    );
    const entry = getLatestQueryStatsEntry(testDB.getMongo(), {collName: coll.getName()});
    assert.eq(entry.key.allowPartialResults, true);
}

withQueryStatsEnabled(collName, (coll) => {
    // Have to create an index for hint not to fail.
    assert.commandWorked(coll.createIndex({v: 1}));

    runCommandAndValidateQueryStats({
        coll: coll,
        commandName: "aggregate",
        commandObj: aggregateCommandObj,
        shapeFields: queryShapeAggregateFields,
        keyFields: queryStatsAggregateKeyFields,
        checkExplain: false, // writeConcern with explain not supported
    });

    validateSystemVariables(coll);
    validateAllowPartialResults(coll);
});

/**
 * This test confirms that query stats store key fields for an aggregate command are properly nested
 * and none are missing when running an explain query.
 * @tags: [requires_fcv_72]
 */
import {
    runCommandAndValidateQueryStats,
    withQueryStatsEnabled
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
    // This flag sets the explain verbosity to 'queryPlanner'.
    explain: true,
    collation: {locale: "en_US", strength: 2},
    hint: {"v": 1},
    comment: "",
    let : {},
    apiDeprecationErrors: false,
    apiVersion: "1",
    apiStrict: false,
};

const queryShapeAggregateFields =
    ["cmdNs", "command", "pipeline", "allowDiskUse", "collation", "let"];

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
    "explain",
    "cursor.batchSize",
];

// Copy the command object to test {explain: false}.
const aggregateCommandObjExplainFalse = Object.assign({}, aggregateCommandObj);
aggregateCommandObjExplainFalse.explain = false;

// Setting the explain flag to 'false' has the same behavior as having no explain flag at all,
// making it not appear on the key.
const queryStatsAggregateKeyFieldsExplainFalse =
    queryStatsAggregateKeyFields.filter(e => e !== "explain");

withQueryStatsEnabled(collName, (coll) => {
    // Create an index for hint not to fail.
    assert.commandWorked(coll.createIndex({v: 1}));

    // Run an aggregate with {explain: true} and make sure that the 'explain'
    // field shows up in the query stats store key.
    runCommandAndValidateQueryStats({
        coll: coll,
        commandName: "aggregate",
        commandObj: aggregateCommandObj,
        shapeFields: queryShapeAggregateFields,
        keyFields: queryStatsAggregateKeyFields
    });

    // Run an aggregate with {explain: false} and make sure that the 'explain'
    // field doesn't shows up in the query stats store key.
    runCommandAndValidateQueryStats({
        coll: coll,
        commandName: "aggregate",
        commandObj: aggregateCommandObjExplainFalse,
        // The shape remains the same since 'explain' is at the key level.
        shapeFields: queryShapeAggregateFields,
        keyFields: queryStatsAggregateKeyFieldsExplainFalse
    });
});

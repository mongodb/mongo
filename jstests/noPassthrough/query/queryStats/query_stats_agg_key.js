/**
 * This test confirms that query stats store key fields for an aggregate command are properly nested
 * and none are missing.
 * @tags: [requires_fcv_72]
 */
import {
    runCommandAndValidateQueryStats,
    withQueryStatsEnabled
} from "jstests/libs/query_stats_utils.js";

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
    "writeConcern",
    "cursor.batchSize",
];

withQueryStatsEnabled(collName, (coll) => {
    // Have to create an index for hint not to fail.
    assert.commandWorked(coll.createIndex({v: 1}));

    runCommandAndValidateQueryStats({
        coll: coll,
        commandName: "aggregate",
        commandObj: aggregateCommandObj,
        shapeFields: queryShapeAggregateFields,
        keyFields: queryStatsAggregateKeyFields
    });
});

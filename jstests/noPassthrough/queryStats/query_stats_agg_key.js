/**
 * This test confirms that query stats store key fields for an aggregate command are properly nested
 * and none are missing.
 * @tags: [featureFlagQueryStats]
 */
import {runCommandAndValidateQueryStats} from "jstests/libs/query_stats_utils.js";

let aggregateCommandObj = {
    aggregate: jsTestName(),
    pipeline: [{"$out": "collOut"}],
    explain: true,
    allowDiskUse: false,
    cursor: {batchSize: 2},
    maxTimeMS: 500,
    bypassDocumentValidation: false,
    readConcern: {level: "local"},
    collation: {locale: "en_US", strength: 2},
    hint: {"v": 1},
    comment: "",
    let : {},
    apiDeprecationErrors: false,
    apiVersion: "1",
    apiStrict: false,
};

const queryShapeAggregateFields =
    ["cmdNs", "command", "pipeline", "explain", "allowDiskUse", "collation", "let"];

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
    "cursor.batchSize",
];

runCommandAndValidateQueryStats({
    commandName: "aggregate",
    commandObj: aggregateCommandObj,
    shapeFields: queryShapeAggregateFields,
    keyFields: queryStatsAggregateKeyFields
});

// Can't have writeConcern with explain, so checking separately.
let writeConcernCommandObj =
    {aggregate: jsTestName(), pipeline: [{"$out": "collOut"}], cursor: {}, writeConcern: {w: 1}};

const writeConcernFields = ["cmdNs", "command", "pipeline"];

// The outer fields not nested inside queryShape.
const writeConcernKeyFields = [
    "queryShape",
    "cursor",
    "writeConcern",
    "client",
    "collectionType",
    "otherNss",
];

runCommandAndValidateQueryStats({
    commandName: "aggregate",
    commandObj: writeConcernCommandObj,
    shapeFields: writeConcernFields,
    keyFields: writeConcernKeyFields
});

/**
 * This test confirms that query stats store key fields for an aggregate command are properly nested
 * and none are missing.
 * @tags: [featureFlagQueryStats]
 */
load("jstests/libs/query_stats_utils.js");  // For runCommandAndValidateQueryStats and
                                            // withQueryStatsEnabled
(function() {
"use strict";

const collName = jsTestName();
const aggregateCommandObj = {
    aggregate: collName,
    pipeline: [{"$out": "collOut"}],
    explain: true,
    allowDiskUse: false,
    cursor: {batchSize: 2},
    maxTimeMS: 50 * 1000,
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

// TODO SERVER-87575 Replace below with commented out section. This replicates what the commented
// out portion would do without the shardingTest within withQueryStatsEnabled().
const options = {
    setParameter: {internalQueryStatsRateLimit: -1},
};
const conn = MongoRunner.runMongod(options);
const testDB = conn.getDB("test");
var coll = testDB[collName];
coll.drop();
assert.commandWorked(coll.createIndex({v: 1}));

runCommandAndValidateQueryStats({
    coll: coll,
    commandName: "aggregate",
    commandObj: aggregateCommandObj,
    shapeFields: queryShapeAggregateFields,
    keyFields: queryStatsAggregateKeyFields
});

runCommandAndValidateQueryStats({
    coll: coll,
    commandName: "aggregate",
    commandObj: writeConcernCommandObj,
    shapeFields: writeConcernFields,
    keyFields: writeConcernKeyFields
});
MongoRunner.stopMongod(conn);

// withQueryStatsEnabled(collName, (coll) => {
//     // Have to create an index for hint not to fail.
//     assert.commandWorked(coll.createIndex({v: 1}));

//     runCommandAndValidateQueryStats({
//         coll: coll,
//         commandName: "aggregate",
//         commandObj: aggregateCommandObj,
//         shapeFields: queryShapeAggregateFields,
//         keyFields: queryStatsAggregateKeyFields
//     });
// });

// withQueryStatsEnabled(collName, (coll) => {
//     // Have to create an index for hint not to fail.
//     assert.commandWorked(coll.createIndex({v: 1}));

//     runCommandAndValidateQueryStats({
//         coll: coll,
//         commandName: "aggregate",
//         commandObj: writeConcernCommandObj,
//         shapeFields: writeConcernFields,
//         keyFields: writeConcernKeyFields
//     });
// });
}());
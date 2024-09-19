/**
 * This test confirms that query stats store key fields for a distinct command are properly nested
 * and none are missing.
 *
 * @tags: [requires_fcv_81]
 */
import {
    runCommandAndValidateQueryStats,
    withQueryStatsEnabled
} from "jstests/libs/query_stats_utils.js";

const collName = jsTestName();

const distinctCommandObjRequired = {
    distinct: collName,
    key: "v",
};

const queryShapeDistinctFieldsRequired = ["cmdNs", "command", "key"];

// The outer fields not nested inside queryShape.
const distinctKeyFieldsRequired = ["queryShape", "collectionType", "client"];

const distinctCommandObjComplex = {
    distinct: collName,
    collation: {locale: "en_US", strength: 2},
    key: "v",
    query: {v: {$gt: 2}},
    comment: "test!!!",
    hint: {"v": 1},
    readConcern: {level: "local"},
    maxTimeMS: 50 * 1000,
    apiDeprecationErrors: false,
    apiVersion: "1",
    apiStrict: false,
    $readPreference: {"mode": "primary"}
};

const queryShapeDistinctFieldsComplex = ["cmdNs", "collation", "command", "key", "query"];

// The outer fields not nested inside queryShape.
const distinctKeyFieldsComplex = [
    "queryShape",
    "comment",
    "readConcern",
    "client",
    "hint",
    "collectionType",
    "apiDeprecationErrors",
    "apiVersion",
    "apiStrict",
    "maxTimeMS",
    "$readPreference"
];

withQueryStatsEnabled(collName, (coll) => {
    // Have to create an index for hint not to fail.
    assert.commandWorked(coll.createIndex({v: 1}));

    runCommandAndValidateQueryStats({
        coll: coll,
        commandName: "distinct",
        commandObj: distinctCommandObjRequired,
        shapeFields: queryShapeDistinctFieldsRequired,
        keyFields: distinctKeyFieldsRequired
    });

    runCommandAndValidateQueryStats({
        coll: coll,
        commandName: "distinct",
        commandObj: distinctCommandObjComplex,
        shapeFields: queryShapeDistinctFieldsComplex,
        keyFields: distinctKeyFieldsComplex
    });
});

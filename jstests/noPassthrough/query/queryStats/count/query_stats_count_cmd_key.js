/**
 * This test confirms that query stats store key fields for a count command are properly nested and
 * none are missing.
 *
 * @tags: [requires_fcv_81]
 */

import {runCommandAndValidateQueryStats, withQueryStatsEnabled} from "jstests/libs/query/query_stats_utils.js";

const collName = jsTestName();
const countCommand = {
    count: collName,
    query: {a: {$eq: 2}},
    limit: 2,
    skip: 1,
    hint: {"v": 1},
    readConcern: {level: "local"},
    comment: "this is a test!!",
    maxTimeMS: 50 * 1000,
    collation: {locale: "en_US", strength: 2},
    apiDeprecationErrors: false,
    apiVersion: "1",
    apiStrict: false,
};
const queryShapeCountFields = ["cmdNs", "command", "query", "limit", "skip", "collation"];
// The outer fields not nested inside queryShape.
const countKeyFields = [
    "queryShape",
    "collectionType",
    "apiDeprecationErrors",
    "apiVersion",
    "apiStrict",
    "client",
    "hint",
    "readConcern",
    "maxTimeMS",
    "comment",
];

withQueryStatsEnabled(collName, (coll) => {
    // Have to create an index for the hint not to fail.
    assert.commandWorked(coll.createIndex({v: 1}));

    runCommandAndValidateQueryStats({
        coll: coll,
        commandName: "count",
        commandObj: countCommand,
        shapeFields: queryShapeCountFields,
        keyFields: countKeyFields,
    });
});

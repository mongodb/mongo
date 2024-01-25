/**
 * This test confirms that query stats store key fields for a find command are properly nested and
 * none are missing.
 * @tags: [requires_fcv_71]
 */
import {
    getLatestQueryStatsEntry,
    runCommandAndValidateQueryStats,
    withQueryStatsEnabled
} from "jstests/libs/query_stats_utils.js";

const collName = jsTestName();

const findCommandObj = {
    find: collName,
    filter: {v: {$eq: 2}},
    oplogReplay: true,
    comment: "this is a test!!",
    min: {"v": 0},
    max: {"v": 4},
    hint: {"v": 1},
    sort: {a: -1},
    returnKey: false,
    noCursorTimeout: true,
    showRecordId: false,
    tailable: false,
    awaitData: false,
    allowPartialResults: true,
    skip: 1,
    limit: 2,
    maxTimeMS: 50 * 1000,
    collation: {locale: "en_US", strength: 2},
    allowDiskUse: true,
    readConcern: {level: "local"},
    batchSize: 2,
    singleBatch: true,
    let : {},
    projection: {_id: 0},
    apiDeprecationErrors: false,
    apiVersion: "1",
    apiStrict: false,
};

const queryShapeFindFields = [
    "cmdNs",
    "command",
    "filter",
    "sort",
    "projection",
    "skip",
    "limit",
    "singleBatch",
    "max",
    "min",
    "returnKey",
    "showRecordId",
    "tailable",
    "oplogReplay",
    "awaitData",
    "collation",
    "allowDiskUse",
    "let"
];

// The outer fields not nested inside queryShape.
const findKeyFields = [
    "queryShape",
    "batchSize",
    "comment",
    "maxTimeMS",
    "noCursorTimeout",
    "readConcern",
    "allowPartialResults",
    "apiDeprecationErrors",
    "apiVersion",
    "apiStrict",
    "collectionType",
    "client",
    "hint"
];

/**
 * Regression test for SERVER-85532: $hint syntax will not be validated if the collection does not
 * exist. Make sure that query stats can still handle an invalid hint. See SERVER-85500.
 *
 * @param testDB
 */
function validateInvalidHint(testDB) {
    const collName = "invalid_hint_coll";
    var coll = testDB[collName];
    coll.drop();
    // $hint is supposed to be a string or object, but this works:
    assert.commandWorked(testDB.runCommand({
        find: collName,
        hint: {$hint: -1.0},
    }));
    const entry = getLatestQueryStatsEntry(testDB.getMongo(), {collName: collName});
    assert.eq(entry.key.hint, {$hint: "?number"});
}

withQueryStatsEnabled(collName, (coll) => {
    // Have to create an index for hint not to fail.
    assert.commandWorked(coll.createIndex({v: 1}));

    runCommandAndValidateQueryStats({
        coll: coll,
        commandName: "find",
        commandObj: findCommandObj,
        shapeFields: queryShapeFindFields,
        keyFields: findKeyFields,
    });

    validateInvalidHint(coll);
});

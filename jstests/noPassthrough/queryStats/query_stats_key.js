/**
 * This test confirms that query stats store key fields are properly nested and none are missing.
 * @tags: [requires_fcv_71]
 */
import {getQueryStats} from "jstests/libs/query_stats_utils.js";

function confirmAllMetaFieldsPresent(clientSubObj) {
    const kApplicationName = "MongoDB Shell";
    assert.eq(clientSubObj.application.name, kApplicationName);

    {
        assert(clientSubObj.hasOwnProperty('driver'), clientSubObj);
        assert(clientSubObj.driver.hasOwnProperty("name"), clientSubObj);
        assert(clientSubObj.driver.hasOwnProperty("version"), clientSubObj);
    }

    {
        assert(clientSubObj.hasOwnProperty('os'), clientSubObj);
        assert(clientSubObj.os.hasOwnProperty("type"), clientSubObj);
        assert(clientSubObj.os.hasOwnProperty("name"), clientSubObj);
        assert(clientSubObj.os.hasOwnProperty("architecture"), clientSubObj);
        assert(clientSubObj.os.hasOwnProperty("version"), clientSubObj);
    }
}

function confirmAllFieldsPresent(queryStatsEntries) {
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
    const queryStatsKeyFields = [
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
        "hint",
    ];

    for (const entry of queryStatsEntries) {
        let fieldCounter = 0;
        assert.eq(entry.key.queryShape.command, "find");
        confirmAllMetaFieldsPresent(entry.key.client);

        for (const field in entry.key.queryShape) {
            assert(queryShapeFindFields.includes(field));
            fieldCounter++;
        }
        assert.eq(fieldCounter, queryShapeFindFields.length);

        fieldCounter = 0;
        for (const field in entry.key) {
            assert(queryStatsKeyFields.includes(field));
            fieldCounter++;
        }
        assert.eq(fieldCounter, queryStatsKeyFields.length, entry.key);
    }
}

// Turn on the collecting of telemetry metrics.
let options = {
    setParameter: {internalQueryStatsRateLimit: -1},
};

const conn = MongoRunner.runMongod(options);
const testDB = conn.getDB('test');
var coll = testDB[jsTestName()];
coll.drop();

// Have to create an index for hint not to fail.
assert.commandWorked(coll.createIndex({v: 1}));

let commandObj = {
    find: coll.getName(),
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
    maxTimeMS: 500,
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

assert.commandWorked(testDB.runCommand(commandObj));
let stats = getQueryStats(conn);
assert.eq(1, stats.length);
confirmAllFieldsPresent(stats);

// $hint can only be string(index name) or object (index spec).
assert.throwsWithCode(() => {
    coll.find({v: {$eq: 2}}).hint({'v': 60, $hint: -128}).itcount();
}, ErrorCodes.BadValue);

MongoRunner.stopMongod(conn);

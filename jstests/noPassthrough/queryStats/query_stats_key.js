/**
 * This test confirms that telemetry store key fields are properly nested and none are missing.
 * @tags: [featureFlagQueryStats]
 */
load("jstests/libs/telemetry_utils.js");
(function() {
"use strict";

function confirmAllFieldsPresent(queryStatsEntries) {
    const kApplicationName = "MongoDB Shell";
    const queryShapeFindFields = [
        "cmdNs",
        "command",
        "filter",
        "sort",
        "projection",
        "hint",
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
        "applicationName"
    ];

    for (const entry of queryStatsEntries) {
        let fieldCounter = 0;
        assert.eq(entry.key.queryShape.command, "find");
        assert.eq(entry.key.applicationName, kApplicationName);

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
        assert.eq(fieldCounter, queryStatsKeyFields.length);
    }
}

// Turn on the collecting of telemetry metrics.
let options = {
    setParameter: {internalQueryStatsSamplingRate: -1},
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
};

assert.commandWorked(testDB.runCommand(commandObj));
let telemetry = getTelemetry(conn);
assert.eq(1, telemetry.length);
confirmAllFieldsPresent(telemetry);

MongoRunner.stopMongod(conn);
}());

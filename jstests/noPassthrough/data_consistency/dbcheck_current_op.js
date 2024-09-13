/**
 * Tests that dbcheck currentOp has the correct information.
 * @tags: [
 *   featureFlagSecondaryIndexChecksInDbCheck
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {
    awaitDbCheckCompletion,
    resetAndInsert,
    runDbCheck
} from "jstests/replsets/libs/dbcheck_utils.js";

(function() {
"use strict";

const rst = new ReplSetTest({nodes: 2});
rst.startSet();
rst.initiate();
const primary = rst.getPrimary();
const testDB = primary.getDB("dbCheck_current_op");
const collName = "coll";

const secondaryIndexCheckParameters =
    ["validateMode", "secondaryIndex", "skipLookupForExtraKeys", "bsonValidateMode"];
const commandFieldNames = [
    "start",
    "end",
    "maxDocsPerBatch",
    "maxBatchTimeMillis",
    "maxCount",
    "maxSize",
    "writeConcern"
];

const getExpectedCurOp = (parameters, uuid) => {
    let indexKey = "_id";
    if (parameters.hasOwnProperty("secondaryIndex")) {
        indexKey = parameters["secondaryIndex"];
    }

    let expectedCurOp = {
        "desc": "dbCheck",
        "ns": "dbCheck_current_op.coll",
        "command": {
            "dbcheck": "dbCheck_current_op.coll",
            "uuid": "",
            "secondaryIndexCheckParameters": {
                "validateMode": "dataConsistency",
                "secondaryIndex": "",
                "skipLookupForExtraKeys": false,
                "bsonValidateMode": "kDefault"
            },
            "start": {[indexKey]: {"$minKey": 1}},
            "end": {[indexKey]: {"$maxKey": 1}},
            "maxDocsPerBatch": NumberLong(5000),
            "maxBatchTimeMillis": NumberLong(1000),
            "maxCount": NumberLong("9223372036854775807"),
            "maxSize": NumberLong("9223372036854775807"),
            "writeConcern": {"w": "majority", "wtimeout": 0}
        },
    };
    expectedCurOp["command"]["uuid"] = uuid;
    secondaryIndexCheckParameters.forEach(field => {
        if (parameters.hasOwnProperty(field)) {
            expectedCurOp["command"]["secondaryIndexCheckParameters"][field] = parameters[field];
        }
    });

    commandFieldNames.forEach(field => {
        if (parameters.hasOwnProperty(field)) {
            expectedCurOp["command"][field] = parameters[field];
        }
    });

    if (parameters.hasOwnProperty("batchWriteConcern")) {
        expectedCurOp["command"]["writeConcern"] = parameters["batchWriteConcern"];
        if (!parameters["batchWriteConcern"].hasOwnProperty("wtimeout")) {
            expectedCurOp["command"]["writeConcern"]["wtimeout"] = 0;
        }
    }

    return expectedCurOp;
};

const verifyCurOp = (expectedCurOp, returnedCurOp) => {
    jsTestLog("Checking command section:\n-ExpectedCurOp:" + tojson(expectedCurOp["command"]) +
              "\n-ReturnedCurOp:" + tojson(returnedCurOp["command"]));
    assert.eq(expectedCurOp["dbcheck"], returnedCurOp["dbcheck"]);
    assert.eq(expectedCurOp["command"]["uuid"], returnedCurOp["command"]["uuid"]);
    secondaryIndexCheckParameters.forEach(field => {
        assert.eq(expectedCurOp["command"]["secondaryIndexCheckParameters"][field],
                  returnedCurOp["command"]["secondaryIndexCheckParameters"][field]);
    });
    commandFieldNames.forEach(field => {
        assert.eq(expectedCurOp["command"][field], returnedCurOp["command"][field]);
    });

    jsTestLog("Checking 'desc' and 'ns' fields.");
    assert.eq(expectedCurOp["desc"], returnedCurOp["desc"], "desc field is not equal");
    assert.eq(expectedCurOp["ns"], returnedCurOp["ns"], "ns field is not equal");
};

const runTest = (parameters) => {
    jsTestLog("Running dbcheck with " + tojson(parameters));
    resetAndInsert(rst, testDB, collName, 10);
    assert.commandWorked(testDB.runCommand({
        createIndexes: collName,
        indexes: [{key: {a: 1}, name: 'a_1'}],
    }));
    rst.awaitReplication();

    const dbcheckFp = configureFailPoint(primary, "hangBeforeProcessingDbCheckRun");
    runDbCheck(rst, testDB, collName, parameters);
    dbcheckFp.wait();
    verifyCurOp(
        getExpectedCurOp(parameters, testDB.getCollectionInfos({name: collName})[0].info.uuid),
        testDB.currentOp().inprog.filter(x => x["desc"] === "dbCheck")[0]);
    dbcheckFp.off();
    awaitDbCheckCompletion(rst, testDB, true /*waitForHealthLogDbCheckStop*/);
};

const dbCheckParameters = [
    {},
    {
        validateMode: "dataConsistency",
        batchWriteConcern: {w: 'majority', wtimeout: 39999},
        maxBatchTimeMillis: 3000,
        maxSize: 4000
    },
    {
        validateMode: "dataConsistencyAndMissingIndexKeysCheck",
        start: {"_id": 10},
        maxCount: 2000,
        bsonValidateMode: "kFull"
    },
    {
        validateMode: "extraIndexKeysCheck",
        secondaryIndex: "a_1",
        start: {"a": 1},
        end: {"a": 4},
        skipLookupForExtraKeys: true,
    },
    {
        validateMode: "extraIndexKeysCheck",
        secondaryIndex: "k_1",
        maxDocsPerBatch: 100,
        batchWriteConcern: {w: 2, j: true}
    },
];
dbCheckParameters.forEach(parameters => runTest(parameters));

rst.stopSet();
}());

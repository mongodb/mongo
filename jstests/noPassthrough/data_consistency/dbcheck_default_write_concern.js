/**
 * Tests that dbcheck correctly applies default write concern if no write concern is set.
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

let rst;
let primary;
const dbName = "dbCheck_default_write_concern";
const collName = "coll";
let testDB;
const W1WC = {
    "w": 1,
    "wtimeout": 0
};
const WMajorityWC = {
    "w": "majority",
    "wtimeout": 0
};

const dbCheckParameters = [
    {},
    {validateMode: "dataConsistency", maxBatchTimeMillis: 3000, maxSize: 4000},
    {
        validateMode: "dataConsistencyAndMissingIndexKeysCheck",
        start: {"_id": 10},
        maxCount: 2000,
        bsonValidateMode: "kFull"
    },
    {
        validateMode: "extraIndexKeysCheck",
        secondaryIndex: "a_1",
        start: {"a": 10},
        end: {"a": 40},
        skipLookupForExtraKeys: true,
    },
    {
        validateMode: "extraIndexKeysCheck",
        secondaryIndex: "k_1",
        maxDocsPerBatch: 100,
    },
];

// Initialize replica set and returns the default wc.
function initializeReplicaSet(isPSASet) {
    const replSetNodes = [{}, {}];
    if (isPSASet) {
        replSetNodes.push({arbiter: true});
    }
    rst = new ReplSetTest({nodes: replSetNodes});
    rst.startSet();
    rst.initiate();
    primary = rst.getPrimary();
    testDB = primary.getDB(dbName);
}

const runTest = (isPSASet) => {
    initializeReplicaSet(isPSASet);
    let defaultWC = isPSASet ? W1WC : WMajorityWC;

    [undefined, W1WC, WMajorityWC].forEach(cwwc => {
        if (cwwc !== undefined) {
            assert.commandWorked(testDB.adminCommand({
                setDefaultRWConcern: 1,
                defaultWriteConcern: cwwc,
                writeConcern: {w: "majority"}
            }));
            defaultWC = cwwc;
        }

        dbCheckParameters.forEach(parameters => {
            jsTestLog("Running RS: (isPSA: " + isPSASet + "), (CWWC: " + cwwc +
                      ") and dbcheck parameters: " + tojson(parameters));
            resetAndInsert(rst, testDB, collName, 10, {a: 1});

            const dbcheckFp = configureFailPoint(primary, "hangBeforeProcessingDbCheckRun");
            runDbCheck(rst, testDB, collName, parameters);
            dbcheckFp.wait();
            const dbcheckWC = testDB.currentOp().inprog.filter(
                x => x["desc"] === "dbCheck")[0]["command"]["writeConcern"];
            // Make sure that default wc is applied.
            assert.eq(defaultWC, dbcheckWC);
            dbcheckFp.off();
            awaitDbCheckCompletion(rst, testDB, true /*waitForHealthLogDbCheckStop*/);
        });
    });
    rst.stopSet();
};

[false, true].forEach(isPSA => runTest(isPSA));
}());

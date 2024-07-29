/**
 * Tests that dbcheck command should fail with 'NamespaceNotFound' if the collection doesn't exist,
 * and fail with 'CommandNotSupportedOnView' if called on a view.
 *
 * After dbcheck command succeeds, If the collection is not present or if there exists a view with
 * an identical name, the 'dbcheck healthLog' must issue a 'NamespaceNotFound' warning and not an
 * inconsistency error.
 *
 * @tags: [featureFlagSecondaryIndexChecksInDbCheck]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {
    assertForDbCheckErrors,
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
const testDB = primary.getDB("dbCheck_deleted_collection");
const collName = "coll_view";

const runTest = (failpoint, mode, dbCheckParameters) => {
    jsTestLog(`Running with failpoint: ${failpoint}, mode: ${mode}, and parameters: ${
        dbCheckParameters}`);
    jsTestLog("Reset the health log and delete the collection/view, then create a new collection.");
    resetAndInsert(rst, testDB, collName, 10);
    assert.commandWorked(testDB.runCommand({
        createIndexes: collName,
        indexes: [{key: {a: 1}, name: 'a_1'}],
    }));

    const dbcheckFp = configureFailPoint(primary, failpoint);
    runDbCheck(rst, testDB, collName);
    dbcheckFp.wait();

    // Drop the collection.
    jsTestLog("Dropping the collection.");
    testDB[collName].drop();
    rst.awaitReplication();

    switch (mode) {
        case "createCollection":
            jsTestLog("Creating another collection with the same name.");
            assert.commandWorked(testDB[collName].insertMany(
                [...Array(10).keys()].map(x => ({a: x})), {ordered: false}));
            break;
        case "createView":
            jsTestLog("Creating a view with the same name.");
            assert.commandWorked(testDB.createCollection(collName, {viewOn: 'nil', pipeline: []}));
            break;
    }

    rst.awaitReplication();
    dbcheckFp.off();
    awaitDbCheckCompletion(rst, testDB, true /*waitForHealthLogDbCheckStop*/);

    // dbcheck shouldn't find any errors and log one warning that the collection doesn't exist.
    let warnings = [];
    try {
        assertForDbCheckErrors(primary, true /*errors*/, true /*warnings*/, warnings);

        // Must always throw
        assert(false);
    } catch (e) {
        assert.eq(warnings.length, 1);
        assert.eq(warnings[0]["severity"], "warning");
        assert.eq(warnings[0]["namespace"], testDB[collName].getFullName());
        assert.includes(warnings[0]["msg"],
                        "abandoning dbCheck batch because collection no longer exists");
        if (mode == "createView" &&
            (failpoint == "hangBeforeProcessingDbCheckRun" ||
             FeatureFlagUtil.isPresentAndEnabled(
                 primary,
                 "SecondaryIndexChecksInDbCheck",
                 ))) {
            // 'acquireCollectionMaybeLockFree' should have failed with 'CommandNotSupportedOnView'.
            assert.includes(warnings[0]["data"]["error"], "is a view, not a collection");
        }
    }
};

jsTestLog("Testing the dbcheck command invocation.");
["doNothing", "createView"].forEach(mode => {
    if (mode == "createView") {
        jsTestLog("Creating a view with the same name.");
        assert.commandWorked(testDB.createCollection(collName, {viewOn: 'nil', pipeline: []}));
    }
    try {
        runDbCheck(rst, testDB, collName);

        // Must always throw
        assert(false);
    } catch (e) {
        if (mode == "createView") {
            assert.eq(e.code, ErrorCodes.CommandNotSupportedOnView);
            assert.includes(e.message, "coll_view is a view hence 'dbcheck' is not supported.");
        } else {
            assert.eq(e.code, ErrorCodes.NamespaceNotFound);
            assert.includes(e.message, "Collection coll_view not found");
        }
    }
});

jsTestLog("Testing the healthLog inconsistency.");
const fps = ["hangBeforeProcessingDbCheckRun", "hangBeforeProcessingFirstBatch"];
const modes = ["doNothing", "createCollection", "createView"];
const params = [
    {},
    {validateMode: "dataConsistencyAndMissingIndexKeysCheck"},
    {validateMode: "extraIndexKeysCheck", indexName: "a_1"}
];
fps.forEach(fb => modes.forEach(mode => params.forEach(param => runTest(fb, mode, param))));

rst.stopSet();
}());

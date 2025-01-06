/**
 * Tests dbCheck with old format index keys and verifies that no inconsistency is found.
 *
 * @tags: [
 *   requires_fcv_80
 * ]
 */

import {
    DbCheckOldFormatKeysTest,
    defaultNumDocs
} from "jstests/multiVersion/libs/dbcheck_old_format_keys_test.js";
import {
    assertCompleteCoverage,
    checkHealthLog,
    clearHealthLog,
    forEachNonArbiterNode,
    logQueries,
    runDbCheck
} from "jstests/replsets/libs/dbcheck_utils.js";

const dbName = "testDB";
const collName = "oldFormatIndexKeyTestColl";

const dbCheckTest = new DbCheckOldFormatKeysTest({});
dbCheckTest.insertOldFormatKeyStrings(dbName, collName);
dbCheckTest.upgradeRst();

const rst = dbCheckTest.getRst();
const primary = dbCheckTest.getPrimary();
clearHealthLog(rst);

jsTestLog("Running dbCheck dataConsistencyAndMissingIndexKeysCheck");
runDbCheck(rst,
           primary.getDB(dbName),
           collName,
           {validateMode: "dataConsistencyAndMissingIndexKeysCheck"},
           true /*awaitCompletion*/);

// Verify that no error or warning health log entries were logged.
forEachNonArbiterNode(rst, function(node) {
    checkHealthLog(node.getDB("local").system.healthlog, logQueries.allErrorsOrWarningsQuery, 0);
});

clearHealthLog(rst);

jsTestLog("Running dbCheck extraIndexKeysCheck");
runDbCheck(rst,
           primary.getDB(dbName),
           collName,
           {validateMode: "extraIndexKeysCheck", secondaryIndex: "a_1"},
           true /*awaitCompletion*/);

// Verify that no error or warning health log entries were logged.
forEachNonArbiterNode(rst, function(node) {
    checkHealthLog(node.getDB("local").system.healthlog, logQueries.allErrorsOrWarningsQuery, 0);
    assertCompleteCoverage(node.getDB("local").system.healthlog,
                           defaultNumDocs,
                           "a" /*indexName*/,
                           null /* docSuffix */,
                           null /* start */,
                           null /* end */);
});

dbCheckTest.stop();

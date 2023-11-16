/**
 * Tests dbCheck with old format index keys and verifies that no inconsistency is found.
 *
 * @tags: [
 *   featureFlagSecondaryIndexChecksInDbCheck
 * ]
 */

import {DbCheckOldFormatKeysTest} from "jstests/multiVersion/libs/dbcheck_old_format_keys_test.js";
import {
    checkHealthLog,
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

jsTestLog("Running dbCheck");
runDbCheck(rst,
           primary.getDB(dbName),
           collName,
           {validateMode: "dataConsistencyAndMissingIndexKeysCheck"},
           true);

// Verify that no error or warning health log entries were logged.
forEachNonArbiterNode(rst, function(node) {
    checkHealthLog(node.getDB("local").system.healthlog, logQueries.allErrorsOrWarningsQuery, 0);
});

dbCheckTest.stop();

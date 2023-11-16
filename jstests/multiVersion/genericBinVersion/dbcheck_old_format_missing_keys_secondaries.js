/**
 * Tests dbCheck with old format index keys that have been deleted. Verifies that dbCheck still
 * detects the inconsistency. Insert inconsistencies on all secondary nodes.
 *
 * @tags: [
 *   featureFlagSecondaryIndexChecksInDbCheck
 * ]
 */

import {
    DbCheckOldFormatKeysTest,
    defaultNumDocs,
} from "jstests/multiVersion/libs/dbcheck_old_format_keys_test.js";
import {
    checkHealthLog,
    forEachNonArbiterSecondary,
    logQueries,
    runDbCheck
} from "jstests/replsets/libs/dbcheck_utils.js";

// Skipping data consistency checks because data is inserted into primary and secondary separately.
TestData.skipCollectionAndIndexValidation = true;
TestData.skipCheckDBHashes = true;

const dbName = "testDB";
const collName = "oldFormatIndexKeyTestColl";

const dbCheckTest = new DbCheckOldFormatKeysTest({});
dbCheckTest.insertOldFormatKeyStrings(dbName, collName);
dbCheckTest.upgradeRst();
dbCheckTest.createMissingKeysOnSecondaries(dbName, collName);

const rst = dbCheckTest.getRst();
const primary = dbCheckTest.getPrimary();

jsTestLog("Running dbCheck");
runDbCheck(rst,
           primary.getDB(dbName),
           collName,
           {validateMode: "dataConsistencyAndMissingIndexKeysCheck"},
           true);

// Verify that dbCheck does not find any inconsistency on the primary.
checkHealthLog(primary.getDB("local").system.healthlog, logQueries.allErrorsOrWarningsQuery, 0);

forEachNonArbiterSecondary(rst, function(node) {
    // Verify that dbCheck detects the inconsistencies for each doc on the secondaries.
    checkHealthLog(
        node.getDB("local").system.healthlog, logQueries.missingIndexKeysQuery, defaultNumDocs);

    // Verify that dbCheck detects the inconsistencies for each doc on the secondaries.
    const missingKeyEntries =
        node.getDB("local").system.healthlog.find(logQueries.missingIndexKeysQuery).toArray();
    for (const missingKeyEntry of missingKeyEntries) {
        const numMissingKeysForRecord = missingKeyEntry.data.context.missingIndexKeys.length;
        const recordId = missingKeyEntry.data.context.recordID;
        assert.eq(2,
                  numMissingKeysForRecord,
                  `Expected to find 2 missing keys for record ${recordId} but found ${
                      numMissingKeysForRecord} instead`);
    }
});

dbCheckTest.stop();

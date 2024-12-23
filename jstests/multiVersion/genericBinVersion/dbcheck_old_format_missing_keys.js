/**
 * Tests dbCheck with old format index keys that have been deleted. Verifies that dbCheck still
 * detects the inconsistency.
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
    forEachNonArbiterNode,
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
dbCheckTest.createMissingKeysOnAllNodes(dbName, collName);

const rst = dbCheckTest.getRst();
const primary = dbCheckTest.getPrimary();

jsTestLog("Running dbCheck");
runDbCheck(rst,
           primary.getDB(dbName),
           collName,
           {validateMode: "dataConsistencyAndMissingIndexKeysCheck"},
           true);

forEachNonArbiterNode(rst, function(node) {
    // Verify that dbCheck detects the inconsistencies for each doc.
    checkHealthLog(
        node.getDB("local").system.healthlog, logQueries.missingIndexKeysQuery, defaultNumDocs);

    // For each doc, verify that there are two missing keys by default.
    const missingKeyEntries =
        node.getDB("local").system.healthlog.find(logQueries.errorQuery).toArray();
    for (const missingKeyEntry of missingKeyEntries) {
        const numMissingKeysForRecord = missingKeyEntry.data.context.missingIndexKeys.length;
        const recordId = missingKeyEntry.data.context.recordID;
        assert.eq(2,
                  numMissingKeysForRecord,
                  `Expected to find 2 missing keys for record ${recordId} but found ${
                      numMissingKeysForRecord} instead`);
    }

    // Verify that there are no other warning or error entries.
    checkHealthLog(
        node.getDB("local").system.healthlog, logQueries.allErrorsOrWarningsQuery, defaultNumDocs);
});

dbCheckTest.stop();

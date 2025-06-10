/**
 * Read a large number of documents from a secondary node. The test expects to read the same number
 * of documents that were inserted without getting unexpected errors. The test focuses on migrations
 * and range deletions that occur during long-running queries. The more 'getMore' commands a query
 * has, the more yield-and-restore events will occur, increasing the likelihood that the query will
 * encounter a range deletion, possibly in combination with various hooks and other factors.
 *
 * @tags: [
 *   requires_getmore,
 *   requires_fcv_82,
 *   # It's not safe to retry any operation if the variable
 *   # TestData.networkErrorAndTxnOverrideConfig.wrapCRUDinTransactions is true.
 *   # TODO remove after SERVER-103880 fixed.
 *   does_not_support_transactions
 * ]
 */

import {arrayDiff, arrayEq} from "jstests/aggregation/extras/utils.js";

const session = db.getMongo().startSession({causalConsistency: true});
const sessionDb = session.getDatabase(db.getName());
const sessionColl = sessionDb.find_on_secondary;
sessionColl.drop();

const documentsToInsert = 100;
const expectedDocuments = Array.from({length: documentsToInsert}).map((_, i) => ({_id: i, x: i}));

assert.commandWorked(sessionColl.insertMany(expectedDocuments));

// Skip the execution of the test from suites that directly set a read concern via
// jstests/libs/override_methods/set_read_and_write_concerns.js if it is not supported in
// causal consistency.
if (TestData.defaultReadConcernLevel === "available" ||
    TestData.defaultReadConcernLevel === "linearizable") {
    jsTestLog("Skipping the test find_on_secondary.js because the read concern " +
              TestData.defaultReadConcernLevel + " is not supported with causal consistency");
    quit();
}

// Disable the setting of a read preference to 'secondary' for suites that directly set it via
// jstests/libs/override_methods/set_read_preference_secondary.js, which causes
// the error: "Cowardly refusing to override read preference to { 'mode' : 'secondary' }".
const testDataDoNotOverrideReadPreferenceOriginal = TestData.doNotOverrideReadPreference;
TestData.doNotOverrideReadPreference = true;

try {
    // The query can be retried on a RetryableError and the following additional errors:
    // * The ErrorCodes.QueryPlanKilled error can occur when a query is terminated on a
    // secondary during a range deletion.
    // * The ErrorCodes.CursorNotFound error can occur during query execution in test suites,
    // typically happening when, for example, the getMore command cannot be continued after
    // simulating a crash.
    //
    // TODO remove retry after SERVER-103880 fixed.
    retryOnRetryableError(
        () => {
            const arr = sessionColl.find().batchSize(5).readPref('secondaryPreferred').toArray();
            assert.gt(arr.length, 0, "Failed to retrieve documents");
            assert(arrayEq(expectedDocuments, arr), () => arrayDiff(expectedDocuments, arr));
        },
        100 /* numRetries */,
        0 /* sleepMs */,
        [ErrorCodes.QueryPlanKilled, ErrorCodes.CursorNotFound]);
    assert(sessionColl.drop());
    session.endSession();
} finally {
    TestData.doNotOverrideReadPreference = testDataDoNotOverrideReadPreferenceOriginal;
}

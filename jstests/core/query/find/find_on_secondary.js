/**
 * Read a large number of documents from a secondary node. The test expects to read the same number
 * of documents that were inserted without getting unexpected errors. The test focuses on migrations
 * and range deletions that occur during long-running queries. The more 'getMore' commands a query
 * has, the more yield-and-restore events will occur, increasing the likelihood that the query will
 * encounter a range deletion, possibly in combination with various hooks and other factors.
 *
 * @tags: [
 *   requires_getmore,
 *   # Error: Cowardly refusing to override read preference to { "mode" : "secondary" }
 *   # TODO (SERVER-102915): Check the possibility to remove these tags.
 *   assumes_read_preference_unchanged,
 *   assumes_read_concern_unchanged,
 *   requires_fcv_82
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

// The query can be retried on a RetryableError and the following additional errors:
// * The ErrorCodes.QueryPlanKilled error can occur when a query is terminated on a
// secondary during a range deletion.
// * The ErrorCodes.CursorNotFound error can occur during query execution in test suites, typically
// happening when, for example, the getMore command cannot be continued after simulating a crash.
retryOnRetryableError(() => {
    const arr = sessionColl.find().batchSize(5).readPref('secondaryPreferred').toArray();
    assert.gt(arr.length, 0, "Failed to retrieve documents");
    assert(arrayEq(expectedDocuments, arr), () => arrayDiff(expectedDocuments, arr));
}, 100 /* numRetries */, 0 /* sleepMs */, [ErrorCodes.QueryPlanKilled, ErrorCodes.CursorNotFound]);

assert(sessionColl.drop());

session.endSession();

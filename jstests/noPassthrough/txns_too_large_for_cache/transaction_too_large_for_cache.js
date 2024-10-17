/**
 * Tests a multi-document transaction requiring more cache than available fails with the expected
 * error code instead of a generic WriteConflictException.
 *
 * @tags: [
 *   requires_persistence,
 *   requires_wiredtiger,
 *   requires_replication,
 * ]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {TransactionsUtil} from "jstests/libs/transactions_util.js";

const replSet = new ReplSetTest({nodes: 1});
replSet.startSet();
replSet.initiate();

const doc1 = {
    x: []
};
for (var j = 0; j < 100000; j++) {
    doc1.x.push("" + Math.random() + Math.random());
}

const db = replSet.getPrimary().getDB(jsTestName());
const session = db.getMongo().startSession();
const sessionDb = session.getDatabase(db.getName());
const coll = sessionDb[jsTestName()];
coll.drop();

// Scale the load in proportion to WT cache size, to reduce test run time.
// A single collection can only have up to 64 indexes. Cap at _id + 1 text index + 62 indexes.
const serverStatus = assert.commandWorked(replSet.getPrimary().getDB('admin').serverStatus());
const wtCacheSizeBytes = serverStatus.wiredTiger.cache["maximum bytes configured"];
const nIndexes = Math.min(Math.ceil(wtCacheSizeBytes * 2 / (1024 * 1024 * 1024)), 62);
assert.commandWorked(coll.createIndex({x: "text"}));
for (let i = 0; i < nIndexes; i++) {
    assert.commandWorked(coll.createIndex({x: 1, ["field" + i]: 1}));
}

// Retry the transaction until we eventually hit the TransactionTooLargeForCache. Only retry on
// WriteConflict error, which is the only expected error besides TransactionTooLargeForCache.
assert.soon(() => {
    session.startTransaction();

    // Keep inserting documents in the transaction until we eventually hit the cache limit.
    let insertCount = 0;
    let result;
    try {
        while (true) {
            try {
                ++insertCount;
                result = coll.insert(doc1);
                assert.commandWorked(result);
            } catch (e) {
                session.abortTransaction();
                assert.commandFailedWithCode(result, ErrorCodes.TransactionTooLargeForCache);
                break;
            }
        }
    } catch (e) {
        assert.commandFailedWithCode(result, ErrorCodes.WriteConflict);
        return false;
    }

    // The error should not have a transient transaction error label. At this point the error must
    // have been TransactionTooLargeForCache. We do this check here to avoid having to check
    // exception types in the outermost catch, in case this assertion fires.
    assert(!TransactionsUtil.isTransientTransactionError(result), result);

    jsTestLog("Iterations until TransactionTooLargeForCache occured: " + insertCount);

    return true;
}, "Expected a transaction to eventually fail with TransactionTooLargeForCache error.");

replSet.stopSet();

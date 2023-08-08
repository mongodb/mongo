/**
 * Tests that a change stream will correctly resume from endOfTransaction event.
 * @tags: [
 *   uses_transactions,
 *   requires_fcv_71,
 *   requires_majority_read_concern,
 *   requires_snapshot_read,
 *   featureFlagEndOfTransactionChangeEvent,
 * ]
 */

import {withTxnAndAutoRetryOnMongos} from "jstests/libs/auto_retry_transaction_in_sharding.js";
import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";

const coll = assertDropAndRecreateCollection(db, "change_stream_resume_from_end_of_transaction");
const sessionOptions = {
    causalConsistency: false
};
const txnOptions = {
    readConcern: {level: "snapshot"},
    writeConcern: {w: "majority"}
};

const session = db.getMongo().startSession(sessionOptions);

// Create these variables before starting the transaction. In sharded passthroughs, accessing
// db[collname] may attempt to implicitly shard the collection, which is not allowed in a txn.
const sessionDb = session.getDatabase(db.getName());
const sessionColl = sessionDb[coll.getName()];

const changeStreamCursor = coll.watch([], {showExpandedEvents: true});

withTxnAndAutoRetryOnMongos(session, () => {
    assert.commandWorked(sessionColl.insert({_id: 1, a: 0}));
    assert.commandWorked(sessionColl.insert({_id: 2, a: 0}));
}, txnOptions);

const expectedOperationTypes = ["insert", "insert", "endOfTransaction"];
const events = [];

const getNextEventAndCheckType = function(expectedOperationType) {
    assert.soon(() => changeStreamCursor.hasNext());
    const event = changeStreamCursor.next();
    assert.eq(event.operationType, expectedOperationType);
    events.push(event);
};

for (let operationType of expectedOperationTypes) {
    getNextEventAndCheckType(operationType);
}

// Write one document after the transaction, so that later we can confirm that resuming after the
// EOT event works.
assert.commandWorked(coll.insert({_id: 3, a: 0}));
getNextEventAndCheckType("insert");

const getNextEvent = function(resumeToken) {
    const resumeCursor = coll.watch([], {resumeAfter: resumeToken, showExpandedEvents: true});
    assert.soon(() => resumeCursor.hasNext());
    return resumeCursor.next();
};

// Test that the resumed stream will produce the EOT event, and that we can successfully resume from
// that EOT.
for (let i = 0; i + 1 < events.length; i += 1) {
    const nextEvent = getNextEvent(events[i]._id);
    assert.eq(nextEvent, events[i + 1]);
}

changeStreamCursor.close();

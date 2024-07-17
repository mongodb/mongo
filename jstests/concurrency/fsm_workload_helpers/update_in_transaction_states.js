
/*
 * update_in_transaction_states.js
 *
 * States to perform updates in transactions for the given database and collection. This includes
 * multi=true updates and multi=false updates with exact _id queries.
 */

import {
    withTxnAndAutoRetry
} from "jstests/concurrency/fsm_workload_helpers/auto_retry_transaction.js";

// The counter values associated with each owned id for a given collection. Used to verify
// updates aren't double applied.
export var expectedCounters = {};

/**
 * Sends a multi=false update with an exact match on _id for a random document assigned to this
 * thread, which should be sent to all shards.
 */
export function exactIdUpdate(db, collName, session, idToUpdate) {
    const collection = session.getDatabase(db.getName()).getCollection(collName);
    withTxnAndAutoRetry(session, () => {
        assert.commandWorked(
            collection.update({_id: idToUpdate}, {$inc: {counter: 1}}, {multi: false}));
    });
    // Update the expected counter for the targeted id.
    expectedCounters[collName][idToUpdate] += 1;
}

/**
 * Sends a multi=true update without the shard key that targets all documents assigned to this
 * thread, which should be sent to all shards.
 */
export function multiUpdate(db, collName, session, tid) {
    const collection = session.getDatabase(db.getName()).getCollection(collName);
    withTxnAndAutoRetry(session, () => {
        assert.commandWorked(collection.update({tid: tid}, {$inc: {counter: 1}}, {multi: true}));
    });

    // The expected counter for every document owned by this thread should be incremented.
    Object.keys(expectedCounters[collName]).forEach(id => {
        expectedCounters[collName][id] += 1;
    });
}

/**
 * Asserts all documents assigned to this thread match their expected values.
 */
export function verifyDocuments(db, collName, tid) {
    const docs = db[collName].find({tid: tid}).toArray();
    docs.forEach(doc => {
        const expectedCounter = expectedCounters[collName][doc._id];
        assert.eq(expectedCounter, doc.counter, () => {
            return 'unexpected counter value for collection ' + db[collName] +
                ', doc: ' + tojson(doc);
        });
    });
}

/**
 * Gives each document assigned to this thread a counter value that is tracked in-memory.
 */
export function initUpdateInTransactionStates(db, collName, tid) {
    expectedCounters[collName] = expectedCounters[collName] || {};
    const session = db.getMongo().startSession({retryWrites: true});
    const collection = session.getDatabase(db.getName()).getCollection(collName);
    // Assign a default counter value to each document owned by this thread.
    db[collName].find({tid: tid}).forEach(doc => {
        expectedCounters[collName][doc._id] = 0;
        assert.commandWorked(collection.update({_id: doc._id}, {$set: {counter: 0}}));
    });
    session.endSession();
}

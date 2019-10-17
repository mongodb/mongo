/*
 * delete_in_transaction_states.js
 *
 * States to perform deletes in transactions without the shard key for the given database and
 * collection. This includes multi=true deletes and multi=false deletes with exact _id queries.
 */

load('jstests/concurrency/fsm_workload_helpers/auto_retry_transaction.js');

// In-memory representation of the documents owned by this thread for all given collections. Used to
// verify the expected documents are deleted in the collection.
let expectedDocuments = {};

// The number of "groups" each document within those assigned to a thread can belong to for a given
// collection. Entire groups will be deleted at once by the multiDelete state function, so this is
// effectively the number of times that stage can be meaningfully run per thread.
const numGroupsWithinThread = $config.data.partitionSize / 5;
let nextGroupId = {};

/**
 * Returns the next groupId for the multiDelete state function to use.
 */
function getNextGroupIdForDelete(collName) {
    const nextId = nextGroupId[collName];
    nextGroupId[collName] = (nextGroupId[collName] + 1) % numGroupsWithinThread;
    return nextId;
}

/**
 * Returns the _id of a random document owned by this thread to be deleted by an exact _id
 * query. Should only be called if this thread hasn't deleted every document assigned to it.
 */
function getIdForDelete(collName) {
    assertAlways.neq(0, expectedDocuments[collName].length);
    const randomIndex = Random.randInt(expectedDocuments[collName].length);
    return expectedDocuments[collName][randomIndex]._id;
}

/**
 * Sends a multi=false delete with an exact match on _id for a random id, which should be sent
 * to all shards.
 */
function exactIdDelete(db, collName, session) {
    // If no documents remain in our partition, there is nothing to do.
    if (!expectedDocuments[collName].length) {
        print('This thread owns no more documents for collection ' + collName +
              ', skipping exactIdDelete stage');
        return;
    }

    const idToDelete = getIdForDelete(collName);

    const collection = session.getDatabase(db.getName()).getCollection(collName);
    withTxnAndAutoRetry(session, () => {
        assertWhenOwnColl.commandWorked(collection.remove({_id: idToDelete}, {multi: false}));
    });

    // Remove the deleted document from the in-memory representation.
    expectedDocuments[collName] = expectedDocuments[collName].filter(obj => {
        return obj._id !== idToDelete;
    });
}

/**
 * Sends a multi=true delete without the shard key that targets all documents assigned to this
 * thread, which should be sent to all shards.
 */
function multiDelete(db, collName, session, tid) {
    // If no documents remain in our partition, there is nothing to do.
    if (!expectedDocuments[collName].length) {
        print('This thread owns no more documents for collection ' + collName +
              ', skipping multiDelete stage');
        return;
    }

    // Delete a group of documents within those assigned to this thread.
    const groupIdToDelete = getNextGroupIdForDelete(collName);

    const collection = session.getDatabase(db.getName()).getCollection(collName);
    withTxnAndAutoRetry(session, () => {
        assertWhenOwnColl.commandWorked(
            collection.remove({tid: tid, groupId: groupIdToDelete}, {multi: true}));
    });

    // Remove the deleted documents from the in-memory representation.
    expectedDocuments[collName] = expectedDocuments[collName].filter(obj => {
        return obj.groupId !== groupIdToDelete;
    });
}

/**
 * Asserts only the expected documents for this thread are present in the collection.
 */
function verifyDocuments(db, collName, tid) {
    const docs = db[collName].find({tid: tid}).toArray();
    assertWhenOwnColl.eq(expectedDocuments[collName].length, docs.length, () => {
        return 'unexpected number of documents for collection ' + collName +
            ', docs: ' + tojson(docs) + ', expected docs: ' + tojson(expectedDocuments[collName]);
    });

    // Verify only the documents we haven't tried to delete were found.
    const expectedDocIds = new Set(expectedDocuments[collName].map(doc => doc._id));
    docs.forEach(doc => {
        assertWhenOwnColl(expectedDocIds.has(doc._id), () => {
            return 'expected document for collection ' + collName +
                ' to be deleted, doc: ' + tojson(doc);
        });
        expectedDocIds.delete(doc._id);
    });

    // All expected document ids should have been found in the collection.
    assertWhenOwnColl.eq(0, expectedDocIds.size, () => {
        return 'did not find all expected documents for collection ' + collName +
            ', _ids not found: ' + tojson(expectedDocIds);
    });
}

/**
 * Gives each document assigned to this thread a group id for multi=true deletes, and loads each
 * document into memory.
 */
function initDeleteInTransactionStates(db, collName, tid) {
    // Assign each document owned by this thread to a different "group" so they can be multi
    // deleted by group later.
    let nextGroupIdForInit = nextGroupId[collName] = 0;
    db[collName].find({tid: tid}).forEach(doc => {
        assert.commandWorked(
            db[collName].update({_id: doc._id}, {$set: {groupId: nextGroupIdForInit}}));
        nextGroupIdForInit = (nextGroupIdForInit + 1) % numGroupsWithinThread;
    });

    // Store the updated documents in-memory so the test can verify the expected ones are
    // deleted.
    expectedDocuments[collName] = db[collName].find({tid: tid}).toArray();
}
'use strict';

/**
 * Performs deletes in transactions without the shard key while chunks are being moved. This
 * includes multi=true deletes and multi=false deletes with exact _id queries.
 *
 * @tags: [requires_sharding, assumes_balancer_off, assumes_autosplit_off,
 * requires_non_retryable_writes, uses_transactions];
 */
load('jstests/concurrency/fsm_libs/extend_workload.js');
load('jstests/concurrency/fsm_workloads/random_moveChunk_base.js');
load('jstests/concurrency/fsm_workload_helpers/auto_retry_transaction.js');

var $config = extendWorkload($config, function($config, $super) {
    $config.threadCount = 5;
    $config.iterations = 50;

    // Number of documents per partition. Note that there is one chunk per partition and one
    // partition per thread.
    $config.data.partitionSize = 100;

    // In-memory representation of the documents owned by this thread. Used to verify the expected
    // documents are deleted in the collection.
    $config.data.expectedDocuments = [];

    // The number of "groups" each document within those assigned to a thread can belong to. Entire
    // groups will be deleted at once by the multiDelete state function, so this is effectively the
    // number of times that stage can be meaningfully run per thread.
    $config.data.numGroupsWithinThread = $config.data.partitionSize / 5;
    $config.data.nextGroupId = 0;

    // A moveChunk may fail with a WriteConflict when clearing orphans on the destination shard if
    // any of them are concurrently written to by a broadcast transaction operation. The error
    // message and top-level code may be different depending on where the failure occurs.
    //
    // TODO SERVER-39141: Don't ignore WriteConflict error message once the range deleter retries on
    // write conflict exceptions.
    //
    // Additionally, because deletes don't have a shard filter stage, a migration may fail if a
    // broadcast delete is operating on orphans from a previous migration in the range being
    // migrated back in. The particular error code is replaced with a more generic one, so this is
    // identified by the failed migration's error message.
    $config.data.isMoveChunkErrorAcceptable = (err) => {
        return err.message &&
            (err.message.indexOf("WriteConflict") > -1 ||
             err.message.indexOf("CommandFailed") > -1 ||
             err.message.indexOf("Documents in target range may still be in use") > -1);
    };

    /**
     * Returns the next groupId for the multiDelete state function to use.
     */
    $config.data.getNextGroupIdForDelete = function getNextGroupIdForDelete() {
        const nextId = this.nextGroupId;
        this.nextGroupId = (this.nextGroupId + 1) % this.numGroupsWithinThread;
        return nextId;
    };

    /**
     * Returns the _id of a random document owned by this thread to be deleted by an exact _id
     * query. Should only be called if this thread hasn't deleted every document assigned to it.
     */
    $config.data.getIdForDelete = function getIdForDelete() {
        assertAlways.neq(0, this.expectedDocuments.length);
        return this.expectedDocuments[Random.randInt(this.expectedDocuments.length)];
    };

    /**
     * Sends a multi=false delete with an exact match on _id for a random id, which should be sent
     * to all shards.
     */
    $config.states.exactIdDelete = function exactIdDelete(db, collName, connCache) {
        // If no documents remain in our partition, there is nothing to do.
        if (!this.expectedDocuments.length) {
            print('This thread owns no more documents, skipping exactIdDelete stage');
            return;
        }

        const idToDelete = this.getIdForDelete();

        const collection = this.session.getDatabase(db.getName()).getCollection(collName);
        withTxnAndAutoRetry(this.session, () => {
            assertWhenOwnColl.writeOK(collection.remove({_id: idToDelete}, {multi: false}));
        });

        // Remove the deleted document from the in-memory representation.
        this.expectedDocuments = this.expectedDocuments.filter(obj => {
            return obj._id !== idToDelete;
        });
    };

    /**
     * Sends a multi=true delete without the shard key that targets all documents assigned to this
     * thread, which should be sent to all shards.
     */
    $config.states.multiDelete = function multiDelete(db, collName, connCache) {
        // If no documents remain in our partition, there is nothing to do.
        if (!this.expectedDocuments.length) {
            print('This thread owns no more documents, skipping multiDelete stage');
            return;
        }

        // Delete a group of documents within those assigned to this thread.
        const groupIdToDelete = this.getNextGroupIdForDelete();

        const collection = this.session.getDatabase(db.getName()).getCollection(collName);
        withTxnAndAutoRetry(this.session, () => {
            assertWhenOwnColl.writeOK(
                collection.remove({tid: this.tid, groupId: groupIdToDelete}, {multi: true}));
        });

        // Remove the deleted documents from the in-memory representation.
        this.expectedDocuments = this.expectedDocuments.filter(obj => {
            return obj.groupId !== groupIdToDelete;
        });
    };

    /**
     * Asserts only the expected documents for this thread are present in the collection.
     */
    $config.states.verifyDocuments = function verifyDocuments(db, collName, connCache) {
        const docs = db[collName].find({tid: this.tid}).toArray();
        assertWhenOwnColl.eq(this.expectedDocuments.length, docs.length, () => {
            return 'unexpected number of documents, docs: ' + tojson(docs) +
                ', expected docs: ' + tojson(this.expectedDocuments);
        });

        // Verify only the documents we haven't tried to delete were found.
        const expectedDocIds = new Set(this.expectedDocuments.map(doc => doc._id));
        docs.forEach(doc => {
            assertWhenOwnColl(expectedDocIds.has(doc._id), () => {
                return 'expected document to be deleted, doc: ' + tojson(doc);
            });
            expectedDocIds.delete(doc._id);
        });

        // All expected document ids should have been found in the collection.
        assertWhenOwnColl.eq(0, expectedDocIds.size, () => {
            return 'did not find all expected documents, _ids not found: ' + tojson(expectedDocIds);
        });
    };

    /**
     * Sets up the base workload, starts a session, gives each document assigned to this thread a
     * group id for multi=true deletes, and loads each document into memory.
     */
    $config.states.init = function init(db, collName, connCache) {
        $super.states.init.apply(this, arguments);

        this.session = db.getMongo().startSession({causalConsistency: false});

        // Assign each document owned by this thread to a different "group" so they can be multi
        // deleted by group later.
        let nextGroupId = 0;
        db[collName].find({tid: this.tid}).forEach(doc => {
            assert.writeOK(db[collName].update({_id: doc._id}, {$set: {groupId: nextGroupId}}));
            nextGroupId = (nextGroupId + 1) % this.numGroupsWithinThread;
        });

        // Store the updated documents in-memory so the test can verify the expected ones are
        // deleted.
        this.expectedDocuments = db[collName].find({tid: this.tid}).toArray();
    };

    $config.transitions = {
        init: {moveChunk: 0.2, exactIdDelete: 0.4, multiDelete: 0.4},
        moveChunk: {moveChunk: 0.2, exactIdDelete: 0.3, multiDelete: 0.3, verifyDocuments: 0.2},
        exactIdDelete: {moveChunk: 0.2, exactIdDelete: 0.3, multiDelete: 0.3, verifyDocuments: 0.2},
        multiDelete: {moveChunk: 0.2, exactIdDelete: 0.3, multiDelete: 0.3, verifyDocuments: 0.2},
        verifyDocuments: {moveChunk: 0.2, exactIdDelete: 0.4, multiDelete: 0.4},
    };

    return $config;
});

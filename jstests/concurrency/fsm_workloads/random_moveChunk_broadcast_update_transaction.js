'use strict';

/**
 * Performs updates in transactions without the shard key while chunks are being moved. This
 * includes multi=true updates and multi=false updates with exact _id queries.
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

    // The counter values associated with each owned id. Used to verify updates aren't double
    // applied.
    $config.data.expectedCounters = {};

    // A moveChunk may fail with a WriteConflict when clearing orphans on the destination shard if
    // any of them are concurrently written to by a broadcast transaction operation. The error
    // message and top-level code may be different depending on where the failure occurs.
    //
    // TODO SERVER-39141: Don't ignore WriteConflict error message once the range deleter retries on
    // write conflict exceptions.
    //
    // Additionally, because updates don't have a shard filter stage, a migration may fail if a
    // broadcast update is operating on orphans from a previous migration in the range being
    // migrated back in. The particular error code is replaced with a more generic one, so this is
    // identified by the failed migration's error message.
    $config.data.isMoveChunkErrorAcceptable = (err) => {
        return err.message &&
            (err.message.indexOf("WriteConflict") > -1 ||
             err.message.indexOf("CommandFailed") > -1 ||
             err.message.indexOf("Documents in target range may still be in use") > -1);
    };

    /**
     * Sends a multi=false update with an exact match on _id for a random document assigned to this
     * thread, which should be sent to all shards.
     */
    $config.states.exactIdUpdate = function exactIdUpdate(db, collName, connCache) {
        const idToUpdate = this.getIdForThread();

        const collection = this.session.getDatabase(db.getName()).getCollection(collName);
        withTxnAndAutoRetry(this.session, () => {
            assertWhenOwnColl.writeOK(
                collection.update({_id: idToUpdate}, {$inc: {counter: 1}}, {multi: false}));
        });

        // Update the expected counter for the targeted id.
        this.expectedCounters[idToUpdate] += 1;
    };

    /**
     * Sends a multi=true update without the shard key that targets all documents assigned to this
     * thread, which should be sent to all shards.
     */
    $config.states.multiUpdate = function multiUpdate(db, collName, connCache) {
        const collection = this.session.getDatabase(db.getName()).getCollection(collName);
        withTxnAndAutoRetry(this.session, () => {
            assertWhenOwnColl.writeOK(
                collection.update({tid: this.tid}, {$inc: {counter: 1}}, {multi: true}));
        });

        // The expected counter for every document owned by this thread should be incremented.
        Object.keys(this.expectedCounters).forEach(id => {
            this.expectedCounters[id] += 1;
        });
    };

    /**
     * Asserts all documents assigned to this thread match their expected values.
     */
    $config.states.verifyDocuments = function verifyDocuments(db, collName, connCache) {
        const docs = db[collName].find({tid: this.tid}).toArray();
        docs.forEach(doc => {
            const expectedCounter = this.expectedCounters[doc._id];
            assertWhenOwnColl.eq(expectedCounter, doc.counter, () => {
                return 'unexpected counter value, doc: ' + tojson(doc);
            });
        });
    };

    /**
     * Sets up the base workload, starts a session, and gives each document assigned to this thread
     * a counter value that is tracked in-memory.
     */
    $config.states.init = function init(db, collName, connCache) {
        $super.states.init.apply(this, arguments);

        this.session = db.getMongo().startSession({causalConsistency: false});

        // Assign a default counter value to each document owned by this thread.
        db[collName].find({tid: this.tid}).forEach(doc => {
            this.expectedCounters[doc._id] = 0;
            assert.writeOK(db[collName].update({_id: doc._id}, {$set: {counter: 0}}));
        });
    };

    $config.transitions = {
        init: {moveChunk: 0.2, exactIdUpdate: 0.4, multiUpdate: 0.4},
        moveChunk: {moveChunk: 0.2, exactIdUpdate: 0.3, multiUpdate: 0.3, verifyDocuments: 0.2},
        exactIdUpdate: {moveChunk: 0.2, exactIdUpdate: 0.3, multiUpdate: 0.3, verifyDocuments: 0.2},
        multiUpdate: {moveChunk: 0.2, exactIdUpdate: 0.3, multiUpdate: 0.3, verifyDocuments: 0.2},
        verifyDocuments: {moveChunk: 0.2, exactIdUpdate: 0.4, multiUpdate: 0.4},
    };

    return $config;
});

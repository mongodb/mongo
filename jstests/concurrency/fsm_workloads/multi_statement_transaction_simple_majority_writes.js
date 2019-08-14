'use strict';

/**
 * Performs concurrent majority writes alongside transactions to verify both will eventually
 * complete as expected.
 *
 * The base workload assumes transactions run at snapshot read concern, so the tag is copied here.
 * @tags: [uses_transactions, assumes_snapshot_transactions]
 */

load('jstests/concurrency/fsm_libs/extend_workload.js');  // for extendWorkload
load('jstests/concurrency/fsm_workloads/multi_statement_transaction_simple.js');  // for $config
load('jstests/concurrency/fsm_workload_helpers/server_types.js');                 // for isMongos

var $config = extendWorkload($config, function($config, $super) {
    $config.data.majorityWriteCollName = 'majority_writes';
    $config.data.counter = 0;

    /**
     * Runs the base workload's init state function and inserts a document to be majority updated
     * later by this thread.
     */
    $config.states.init = function init(db, collName) {
        $super.states.init.apply(this, arguments);

        assertWhenOwnColl.commandWorked(db[this.majorityWriteCollName].insert(
            {_id: this.tid, counter: this.counter}, {writeConcern: {w: 'majority'}}));
    };

    /**
     * Updates a document unrelated to the transaction run in the base workload using majority write
     * concern and verifies the update is immediately visible in the majority snapshot once the
     * write returns.
     */
    $config.states.majorityWriteUnrelatedDoc = function majorityWriteUnrelatedDoc(db, collName) {
        this.counter += 1;
        assertWhenOwnColl.commandWorked(db[this.majorityWriteCollName].update(
            {_id: this.tid}, {$set: {counter: this.counter}}, {writeConcern: {w: 'majority'}}));

        // As soon as the write returns, its effects should be visible in the majority snapshot.
        const doc = db[this.majorityWriteCollName].findOne({_id: this.tid});
        assertWhenOwnColl.eq(
            this.counter, doc.counter, 'unexpected counter value, doc: ' + tojson(doc));
    };

    /**
     * Updates a document that may be written to by the transaction run in the base workload using
     * majority write concern and verifies the update is immediately visible in the majority
     * snapshot once the write returns.
     */
    $config.states.majorityWriteTxnDoc = function majorityWriteTxnDoc(db, collName) {
        this.counter += 1;

        // Choose a random document that may be written to by the base workload. The base collection
        // contains documents with _id ranging from 0 to the number of accounts. Update a field
        // based on the thread's id, since threads may concurrently write to the same document.
        const transactionDocId = Random.randInt(this.numAccounts);
        const threadUniqueField = 'thread' + this.tid;
        assertWhenOwnColl.commandWorked(
            db[collName].update({_id: transactionDocId},
                                {$set: {[threadUniqueField]: this.counter}},
                                {writeConcern: {w: 'majority'}}));

        // As soon as the write returns, its effects should be visible in the majority snapshot.
        const doc = db[collName].findOne({_id: transactionDocId});
        assertWhenOwnColl.eq(
            this.counter,
            doc[threadUniqueField],
            'unexpected thread unique field value, thread: ' + this.tid + ', doc: ' + tojson(doc));
    };

    /**
     * Runs the base workload's setup and, if necessary, shards the collection that is majority
     * written to by this workload.
     */
    $config.setup = function setup(db, collName, cluster) {
        $super.setup.apply(this, arguments);

        if (isMongos(db)) {
            // The database will already have had sharding enabled by the fsm infrastructure.
            db.adminCommand({
                shardCollection: db[this.majorityWriteCollName].getFullName(),
                key: {_id: 'hashed'}
            });
        }
    };

    $config.transitions = {
        init: {transferMoney: 1},
        transferMoney: {
            transferMoney: 0.5,
            checkMoneyBalance: 0.1,
            majorityWriteUnrelatedDoc: 0.2,
            majorityWriteTxnDoc: 0.2
        },
        checkMoneyBalance:
            {transferMoney: 0.5, majorityWriteUnrelatedDoc: 0.25, majorityWriteTxnDoc: 0.25},
        majorityWriteUnrelatedDoc:
            {transferMoney: 0.5, majorityWriteUnrelatedDoc: 0.25, majorityWriteTxnDoc: 0.25},
        majorityWriteTxnDoc:
            {transferMoney: 0.5, majorityWriteUnrelatedDoc: 0.25, majorityWriteTxnDoc: 0.25},
    };

    return $config;
});

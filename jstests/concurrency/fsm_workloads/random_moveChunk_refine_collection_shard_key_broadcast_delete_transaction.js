'use strict';

/**
 * Performs these actions in parallel:
 * 1. Refine a collection's shard key.
 * 2. Perform deletes in transactions without the shard key.
 * 3. Move random chunks.
 *
 * @tags: [requires_sharding, assumes_balancer_off, assumes_autosplit_off,
 * requires_non_retryable_writes, uses_transactions];
 */
load('jstests/concurrency/fsm_libs/extend_workload.js');
load('jstests/concurrency/fsm_workloads/random_moveChunk_refine_collection_shard_key.js');
load('jstests/concurrency/fsm_workload_helpers/delete_in_transaction_states.js');

var $config = extendWorkload($config, function($config, $super) {
    $config.iterations = 10;

    $config.states.exactIdDelete = function(db, collName, connCache) {
        exactIdDelete(db, this.getCurrentOrPreviousLatchCollName(collName), this.session);
    };
    $config.states.multiDelete = function(db, collName, connCache) {
        multiDelete(db, this.getCurrentOrPreviousLatchCollName(collName), this.session, this.tid);
    };
    $config.states.verifyDocuments = function(db, collName, connCache) {
        verifyDocuments(db, this.getCurrentOrPreviousLatchCollName(collName), this.tid);
    };

    /**
     * 1. Set up the base workloads.
     * 2. Start a session.
     * 3. Initialize the state necessary for each latch collection to update documents inside
     *    transactions.
     */
    $config.states.init = function init(db, collName, connCache) {
        $super.states.init.apply(this, arguments);
        this.session = db.getMongo().startSession({causalConsistency: false});

        for (let i = this.latchCount; i >= 0; --i) {
            const latchCollName = collName + '_' + i;
            initDeleteInTransactionStates(db, latchCollName, this.tid);
        }
    };

    $config.transitions = {
        init: {moveChunk: 0.2, refineCollectionShardKey: 0.2, exactIdDelete: 0.3, multiDelete: 0.3},
        moveChunk: {
            moveChunk: 0.2,
            refineCollectionShardKey: 0.2,
            exactIdDelete: 0.2,
            multiDelete: 0.2,
            verifyDocuments: 0.2
        },
        refineCollectionShardKey: {
            moveChunk: 0.3,
            refineCollectionShardKey: 0.1,
            exactIdDelete: 0.2,
            multiDelete: 0.2,
            verifyDocuments: 0.2
        },
        exactIdDelete: {
            moveChunk: 0.2,
            refineCollectionShardKey: 0.2,
            exactIdDelete: 0.2,
            multiDelete: 0.2,
            verifyDocuments: 0.2
        },
        multiDelete: {
            moveChunk: 0.2,
            refineCollectionShardKey: 0.2,
            exactIdDelete: 0.2,
            multiDelete: 0.2,
            verifyDocuments: 0.2
        },
        verifyDocuments:
            {moveChunk: 0.2, refineCollectionShardKey: 0.2, exactIdDelete: 0.3, multiDelete: 0.3},
    };

    return $config;
});

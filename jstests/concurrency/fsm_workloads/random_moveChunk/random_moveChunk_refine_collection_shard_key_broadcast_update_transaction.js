/**
 * Performs these actions in parallel:
 * 1. Refine a collection's shard key.
 * 2. Perform updates in transactions without the shard key.
 * 3. Move random chunks.
 * 4. Flushes the router's cached metadata for all sharded collections.
 *
 * @tags: [
 *   assumes_balancer_off,
 *   # The init() state function populates each document owned by a particular thread with a
 *   # "counter" value. Doing so may take longer than the configured stepdown interval. It is
 *   # therefore unsafe to automatically run inside a multi-statement transaction because its
 *   # progress will continually be interrupted.
 *   operations_longer_than_stepdown_interval_in_txns,
 *   requires_non_retryable_writes,
 *   requires_sharding,
 *   uses_transactions,
 * ]
 */
import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {
    exactIdUpdate,
    initUpdateInTransactionStates,
    multiUpdate,
    verifyDocuments,
} from "jstests/concurrency/fsm_workload_helpers/update_in_transaction_states.js";
import {$config as $baseConfig} from "jstests/concurrency/fsm_workloads/random_moveChunk/random_moveChunk_refine_collection_shard_key.js";

export const $config = extendWorkload($baseConfig, function ($config, $super) {
    $config.threadCount = 5;
    $config.iterations = 10;

    $config.states.exactIdUpdate = function (db, collName, connCache) {
        const latchCollName = this.getCurrentOrPreviousLatchCollName(collName);
        exactIdUpdate(db, latchCollName, this.session, this.getIdForThread(latchCollName));
    };

    $config.states.multiUpdate = function (db, collName, connCache) {
        multiUpdate(db, this.getCurrentOrPreviousLatchCollName(collName), this.session, this.tid);
    };

    $config.states.verifyDocuments = function (db, collName, connCache) {
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
            const latchCollName = collName + "_" + i;
            initUpdateInTransactionStates(db, latchCollName, this.tid);
        }
    };

    $config.transitions = {
        init: {
            moveChunk: 0.2,
            refineCollectionShardKey: 0.2,
            exactIdUpdate: 0.25,
            multiUpdate: 0.25,
            flushRouterConfig: 0.1,
        },
        moveChunk: {
            moveChunk: 0.18,
            refineCollectionShardKey: 0.18,
            exactIdUpdate: 0.18,
            multiUpdate: 0.18,
            verifyDocuments: 0.18,
            flushRouterConfig: 0.1,
        },
        refineCollectionShardKey: {
            moveChunk: 0.2,
            refineCollectionShardKey: 0.1,
            exactIdUpdate: 0.2,
            multiUpdate: 0.2,
            verifyDocuments: 0.2,
            flushRouterConfig: 0.2,
        },
        exactIdUpdate: {
            moveChunk: 0.18,
            refineCollectionShardKey: 0.18,
            exactIdUpdate: 0.18,
            multiUpdate: 0.18,
            verifyDocuments: 0.18,
            flushRouterConfig: 0.1,
        },
        multiUpdate: {
            moveChunk: 0.18,
            refineCollectionShardKey: 0.18,
            exactIdUpdate: 0.18,
            multiUpdate: 0.18,
            verifyDocuments: 0.18,
            flushRouterConfig: 0.1,
        },
        verifyDocuments: {
            moveChunk: 0.2,
            refineCollectionShardKey: 0.2,
            exactIdUpdate: 0.2,
            multiUpdate: 0.2,
            flushRouterConfig: 0.2,
        },
        flushRouterConfig: {
            moveChunk: 0.2,
            refineCollectionShardKey: 0.2,
            exactIdUpdate: 0.2,
            multiUpdate: 0.2,
            verifyDocuments: 0.2,
        },
    };

    return $config;
});

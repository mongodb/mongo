/**
 * Performs updates in transactions without the shard key while chunks are being moved. This
 * includes multi=true updates and multi=false updates with exact _id queries.
 *
 * @tags: [
 *  requires_sharding,
 *  assumes_balancer_off,
 *  requires_non_retryable_writes,
 *  uses_transactions,
 * ];
 */
import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {
    exactIdUpdate,
    initUpdateInTransactionStates,
    multiUpdate,
    verifyDocuments,
} from "jstests/concurrency/fsm_workload_helpers/update_in_transaction_states.js";
import {$config as $baseConfig} from "jstests/concurrency/fsm_workloads/random_moveChunk/random_moveChunk_base.js";

export const $config = extendWorkload($baseConfig, function ($config, $super) {
    $config.threadCount = 5;
    $config.iterations = 50;

    // Number of documents per partition. Note that there is one chunk per partition and one
    // partition per thread.
    $config.data.partitionSize = 100;

    // Because updates don't have a shard filter stage, a migration may fail if a
    // broadcast update is operating on orphans from a previous migration in the range being
    // migrated back in. The particular error code is replaced with a more generic one, so this is
    // identified by the failed migration's error message.
    $config.data.isMoveChunkErrorAcceptable = (err) => {
        return (
            err.message &&
            (err.message.indexOf("CommandFailed") > -1 ||
                err.message.indexOf("Documents in target range may still be in use") > -1)
        );
    };

    $config.states.exactIdUpdate = function (db, collName, connCache) {
        exactIdUpdate(db, collName, this.session, this.getIdForThread(collName));
    };
    $config.states.multiUpdate = function (db, collName, connCache) {
        multiUpdate(db, collName, this.session, this.tid);
    };
    $config.states.verifyDocuments = function (db, collName, connCache) {
        verifyDocuments(db, collName, this.tid);
    };

    /**
     * Sets up the base workload, starts a session, and initializes the state necessary to update
     * documents inside transactions.
     */
    $config.states.init = function init(db, collName, connCache) {
        $super.states.init.apply(this, arguments);
        this.session = db.getMongo().startSession({causalConsistency: false});
        initUpdateInTransactionStates(db, collName, this.tid);
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

/**
 * Performs deletes in transactions without the shard key while chunks are being moved. This
 * includes multi=true deletes and multi=false deletes with exact _id queries.
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
    exactIdDelete,
    initDeleteInTransactionStates,
    multiDelete,
    verifyDocuments
} from "jstests/concurrency/fsm_workload_helpers/delete_in_transaction_states.js";
import {
    $config as $baseConfig
} from "jstests/concurrency/fsm_workloads/random_moveChunk/random_moveChunk_base.js";

export const $config = extendWorkload($baseConfig, function($config, $super) {
    $config.threadCount = 5;
    $config.iterations = 50;

    // Number of documents per partition. Note that there is one chunk per partition and one
    // partition per thread.
    $config.data.partitionSize = 100;

    // Because deletes don't have a shard filter stage, a migration may fail if a
    // broadcast delete is operating on orphans from a previous migration in the range being
    // migrated back in. The particular error code is replaced with a more generic one, so this is
    // identified by the failed migration's error message.
    $config.data.isMoveChunkErrorAcceptable = (err) => {
        return err.message &&
            (err.message.indexOf("CommandFailed") > -1 ||
             err.message.indexOf("Documents in target range may still be in use") > -1);
    };

    $config.states.exactIdDelete = function(db, collName, connCache) {
        exactIdDelete(db, collName, this.session);
    };
    $config.states.multiDelete = function(db, collName, connCache) {
        multiDelete(db, collName, this.session, this.tid, this.partitionSize);
    };
    $config.states.verifyDocuments = function(db, collName, connCache) {
        verifyDocuments(db, collName, this.tid);
    };

    /**
     * Sets up the base workload, starts a session, and initializes the state necessary to
     * delete documents inside transactions.
     */
    $config.states.init = function init(db, collName, connCache) {
        $super.states.init.apply(this, arguments);
        this.session = db.getMongo().startSession({causalConsistency: false});
        initDeleteInTransactionStates(db, collName, this.tid, this.partitionSize);
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

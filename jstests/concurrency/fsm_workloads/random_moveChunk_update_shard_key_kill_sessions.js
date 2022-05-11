'use strict';

/**
 * Performs updates that will change a document's shard key while migrating chunks and killing
 * sessions. Only runs updates that cause a document to change shards to increase the odds of
 * killing an internal transaction.
 *
 * @tags: [
 *  requires_sharding,
 *  assumes_balancer_off,
 *  uses_transactions,
 * ]
 */
load('jstests/concurrency/fsm_libs/extend_workload.js');
load('jstests/concurrency/fsm_workload_helpers/kill_session.js');  // for killSession
load('jstests/concurrency/fsm_workloads/random_moveChunk_update_shard_key.js');
load('jstests/libs/override_methods/retry_on_killed_session.js');

// By default retry_on_killed_session.js will only retry known retryable operations like reads and
// retryable writes, but the moveChunks in this test may be interrupted and are safe to retry so opt
// into always retrying killed operations.
TestData.alwaysRetryOnKillSessionErrors = true;

var $config = extendWorkload($config, function($config, $super) {
    $config.data.retryOnKilledSession = true;

    // The base workload uses connCache, so wrap killSessions so the fsm runner doesn't complain
    // that it only expects 2 arguments.
    $config.states.killSession = function wrappedKillSession(db, collName, connCache) {
        return killSession(db, collName);
    };

    $config.transitions = {
        init: {
            killSession: 0.1,
            moveChunk: 0.1,
            findAndModifyWithRetryableWriteAcrossChunks: 0.2,
            findAndModifyWithTransactionAcrossChunks: 0.2,
            updateWithRetryableWriteAcrossChunks: 0.2,
            updateWithTransactionAcrossChunks: 0.2,
        },
        killSession: {
            killSession: 0.1,
            moveChunk: 0.1,
            findAndModifyWithRetryableWriteAcrossChunks: 0.15,
            findAndModifyWithTransactionAcrossChunks: 0.15,
            updateWithRetryableWriteAcrossChunks: 0.15,
            updateWithTransactionAcrossChunks: 0.15,
            verifyDocuments: 0.2
        },
        moveChunk: {
            killSession: 0.1,
            moveChunk: 0.1,
            findAndModifyWithRetryableWriteAcrossChunks: 0.15,
            findAndModifyWithTransactionAcrossChunks: 0.15,
            updateWithRetryableWriteAcrossChunks: 0.15,
            updateWithTransactionAcrossChunks: 0.15,
            verifyDocuments: 0.2
        },
        findAndModifyWithRetryableWriteAcrossChunks: {
            killSession: 0.1,
            moveChunk: 0.1,
            findAndModifyWithRetryableWriteAcrossChunks: 0.15,
            findAndModifyWithTransactionAcrossChunks: 0.15,
            updateWithRetryableWriteAcrossChunks: 0.15,
            updateWithTransactionAcrossChunks: 0.15,
            verifyDocuments: 0.2
        },
        findAndModifyWithTransactionAcrossChunks: {
            killSession: 0.1,
            moveChunk: 0.1,
            findAndModifyWithRetryableWriteAcrossChunks: 0.15,
            findAndModifyWithTransactionAcrossChunks: 0.15,
            updateWithRetryableWriteAcrossChunks: 0.15,
            updateWithTransactionAcrossChunks: 0.15,
            verifyDocuments: 0.2
        },
        updateWithRetryableWriteAcrossChunks: {
            killSession: 0.1,
            moveChunk: 0.1,
            findAndModifyWithRetryableWriteAcrossChunks: 0.15,
            findAndModifyWithTransactionAcrossChunks: 0.15,
            updateWithRetryableWriteAcrossChunks: 0.15,
            updateWithTransactionAcrossChunks: 0.15,
            verifyDocuments: 0.2
        },
        updateWithTransactionAcrossChunks: {
            killSession: 0.1,
            moveChunk: 0.1,
            findAndModifyWithRetryableWriteAcrossChunks: 0.15,
            findAndModifyWithTransactionAcrossChunks: 0.15,
            updateWithRetryableWriteAcrossChunks: 0.15,
            updateWithTransactionAcrossChunks: 0.15,
            verifyDocuments: 0.2
        },
        verifyDocuments: {
            killSession: 0.1,
            moveChunk: 0.1,
            findAndModifyWithRetryableWriteAcrossChunks: 0.15,
            findAndModifyWithTransactionAcrossChunks: 0.15,
            updateWithRetryableWriteAcrossChunks: 0.15,
            updateWithTransactionAcrossChunks: 0.15,
            verifyDocuments: 0.2
        },
    };

    return $config;
});

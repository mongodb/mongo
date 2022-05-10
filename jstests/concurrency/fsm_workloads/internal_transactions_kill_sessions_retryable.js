'use strict';

/**
 * Performs CRUD commands using retryable internal transactions while simultaneously killing
 * sessions.
 *
 * @tags: [
 *  requires_fcv_60,
 *  uses_transactions,
 * ]
 */

load('jstests/concurrency/fsm_libs/extend_workload.js');
load('jstests/concurrency/fsm_workload_helpers/kill_session.js');  // for killSession
load('jstests/concurrency/fsm_workloads/internal_transactions_unsharded.js');
load('jstests/libs/override_methods/retry_on_killed_session.js');

var $config = extendWorkload($config, function($config, $super) {
    $config.data.retryOnKilledSession = true;

    $config.states.killSession = function(db, collName) {
        return killSession(db, collName);
    };

    $config.data.generateRandomExecutionContext = function generateRandomExecutionContext() {
        return this.generateRandomInt(3, 4);
    };

    $config.transitions = {
        init: {
            killSession: 0.2,
            internalTransactionForInsert: 0.2,
            internalTransactionForUpdate: 0.2,
            internalTransactionForDelete: 0.2,
            internalTransactionForFindAndModify: 0.2,
        },
        killSession: {
            internalTransactionForInsert: 0.2,
            internalTransactionForUpdate: 0.2,
            internalTransactionForDelete: 0.2,
            internalTransactionForFindAndModify: 0.2,
            verifyDocuments: 0.2
        },
        internalTransactionForInsert: {
            killSession: 0.4,
            internalTransactionForInsert: 0.12,
            internalTransactionForUpdate: 0.12,
            internalTransactionForDelete: 0.12,
            internalTransactionForFindAndModify: 0.12,
            verifyDocuments: 0.12
        },
        internalTransactionForUpdate: {
            killSession: 0.4,
            internalTransactionForInsert: 0.12,
            internalTransactionForUpdate: 0.12,
            internalTransactionForDelete: 0.12,
            internalTransactionForFindAndModify: 0.12,
            verifyDocuments: 0.12
        },
        internalTransactionForDelete: {
            killSession: 0.4,
            internalTransactionForInsert: 0.12,
            internalTransactionForUpdate: 0.12,
            internalTransactionForDelete: 0.12,
            internalTransactionForFindAndModify: 0.12,
            verifyDocuments: 0.12
        },
        internalTransactionForFindAndModify: {
            killSession: 0.4,
            internalTransactionForInsert: 0.12,
            internalTransactionForUpdate: 0.12,
            internalTransactionForDelete: 0.12,
            internalTransactionForFindAndModify: 0.12,
            verifyDocuments: 0.12
        },
        verifyDocuments: {
            killSession: 0.4,
            internalTransactionForInsert: 0.12,
            internalTransactionForUpdate: 0.12,
            internalTransactionForDelete: 0.12,
            internalTransactionForFindAndModify: 0.12,
            verifyDocuments: 0.12
        }
    };

    return $config;
});

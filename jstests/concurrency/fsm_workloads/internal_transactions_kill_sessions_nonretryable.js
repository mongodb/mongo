'use strict';

/**
 * Performs CRUD commands using non-retryable internal transactions while simultaneously killing
 * sessions.
 *
 * @tags: [
 *  requires_fcv_60,
 *  uses_transactions,
 * ]
 */

load('jstests/concurrency/fsm_libs/extend_workload.js');
load('jstests/concurrency/fsm_workloads/internal_transactions_kill_sessions_retryable.js');
load('jstests/libs/fail_point_util.js');

var $config = extendWorkload($config, function($config, $super) {
    $config.data.generateRandomExecutionContext = function generateRandomExecutionContext() {
        if (this.shouldUseCausalConsistency) {
            // Exclude kNoClientSession since a casually consistent session is required.
            return 2;
        }
        return this.generateRandomInt(1, 2);
    };

    $config.data.runInternalTransaction = function runInternalTransaction(
        db, collection, executionCtxType, writeCmdObj, checkResponseFunc, checkDocsFunc) {
        try {
            $super.data.runInternalTransaction.apply(this, arguments);
        } catch (e) {
            if (e.code == ErrorCodes.Interrupted) {
                // killSession commands generate interrupted errors, so this is not a reason to
                // abort the workload. Continue to the next iteration.
                return;
            }
            throw e;
        }
    };

    $config.setup = function setup(db, collName, cluster) {
        $super.setup.apply(this, arguments);
        if (cluster.isSharded()) {
            cluster.executeOnMongosNodes((db) => {
                configureFailPoint(db, "skipTransactionApiRetryCheckInHandleError");
            });
        } else if (cluster.isReplication()) {
            configureFailPoint(db, "skipTransactionApiRetryCheckInHandleError");
        }
    };

    // Sessions may be killed after transactions are already in the prepared state, so local
    // document state may not accurately reflect the document's state in the database.
    $config.data.expectDirtyDocs["skipCheckDocs"] = true;

    $config.teardown = function teardown(db, collName, cluster) {
        if (cluster.isSharded()) {
            cluster.executeOnMongosNodes((db) => {
                configureFailPoint(db, "skipTransactionApiRetryCheckInHandleError", {}, "off");
            });
        } else if (cluster.isReplication()) {
            configureFailPoint(db, "skipTransactionApiRetryCheckInHandleError");
        }
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
            internalTransactionForInsert: 0.25,
            internalTransactionForUpdate: 0.25,
            internalTransactionForDelete: 0.25,
            internalTransactionForFindAndModify: 0.25,
        },
        internalTransactionForInsert: {
            killSession: 0.4,
            internalTransactionForInsert: 0.15,
            internalTransactionForUpdate: 0.15,
            internalTransactionForDelete: 0.15,
            internalTransactionForFindAndModify: 0.15,
        },
        internalTransactionForUpdate: {
            killSession: 0.4,
            internalTransactionForInsert: 0.15,
            internalTransactionForUpdate: 0.15,
            internalTransactionForDelete: 0.15,
            internalTransactionForFindAndModify: 0.15,
        },
        internalTransactionForDelete: {
            killSession: 0.4,
            internalTransactionForInsert: 0.15,
            internalTransactionForUpdate: 0.15,
            internalTransactionForDelete: 0.15,
            internalTransactionForFindAndModify: 0.15,
        },
        internalTransactionForFindAndModify: {
            killSession: 0.4,
            internalTransactionForInsert: 0.15,
            internalTransactionForUpdate: 0.15,
            internalTransactionForDelete: 0.15,
            internalTransactionForFindAndModify: 0.15,
        },
        verifyDocuments: {
            killSession: 0.4,
            internalTransactionForInsert: 0.15,
            internalTransactionForUpdate: 0.15,
            internalTransactionForDelete: 0.15,
            internalTransactionForFindAndModify: 0.15,
        }
    };

    return $config;
});

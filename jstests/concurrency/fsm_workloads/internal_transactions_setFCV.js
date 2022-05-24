'use strict';

/**
 * Runs insert, update, delete and findAndModify commands against in internal transactions using all
 * the available client session settings, and occasionally runs setFCV to downgrade or upgrade the
 * standalone replica set or sharded cluster that this workload runs on.
 *
 * @tags: [
 *  uses_transactions,
 *  # Requires all nodes to be running the latest binary.
 *  multiversion_incompatible,
 * ]
 */
load('jstests/concurrency/fsm_libs/extend_workload.js');
load('jstests/concurrency/fsm_workloads/internal_transactions_unsharded.js');
// Operations that run concurrently with setFCV may get interrupted due to setFCV issuing a
// killSession command for any sessions with unprepared transactions during an FCV change. We want
// to have to support retrying operations that are idempotent in such scenarios.
load('jstests/libs/override_methods/retry_on_killed_session.js');

var $config = extendWorkload($config, function($config, $super) {
    $config.data.retryOnKilledSession = true;

    $config.data.expectDirtyDocs = {
        // The client is either not using a session or is using a session without retryable writes
        // enabled. Therefore, when a write is interrupted, they cannot retry the write to verify if
        // it has been executed or not.
        [$super.data.executionContextTypes.kNoClientSession]: true,
        [$super.data.executionContextTypes.kClientSession]: true,
        // The client is using a session with retryable writes enabled, but a retryable write that
        // is executed using an internal transaction is expected to fail with
        // InternalTransactionNotSupported if it is retried after a downgrade with a different
        // internal lsid and txnNumber. So when a write is interrupted, the client also cannot
        // verify if it has been executed or not.
        [$super.data.executionContextTypes.kClientRetryableWrite]: true,
        // The withTxnAndAutoRetry wrapper handles retrying transactions upon interrupt errors (by
        // retrying just the commit or the entire transaction).
        [$super.data.executionContextTypes.kClientTransaction]: false,
    };

    $config.data.isAcceptableRetryError = function isAcceptableRetryError(res) {
        // A retryable write that is executed using an internal transaction and is retried with
        // the same internal lsid and txnNumber (after the internal transaction has committed) after
        // a downgrade is expected to fail with IncompleteTransactionHistory.
        return (res.code == ErrorCodes.IncompleteTransactionHistory) &&
            res.errmsg.includes("Incomplete history detected for transaction");
    };

    $config.data.runInternalTransaction = function runInternalTransaction(
        db, collection, executionCtxType, writeCmdObj, checkResponseFunc, checkDocsFunc) {
        try {
            $super.data.runInternalTransaction.apply(this, arguments);
        } catch (e) {
            if (e.code == ErrorCodes.InternalTransactionNotSupported) {
                // For the client transaction and no client session case, the transaction should
                // never fail with InternalTransactionNotSupported since its session id is not in
                // the new format.
                assert.neq(executionCtxType, this.executionContextTypes.kClientTransaction);
                assert.neq(executionCtxType, this.executionContextTypes.kNoClientSession);
                return;
            }
            if (e.code == ErrorCodes.Interrupted) {
                // For the client retryable write case, interrupt errors should be handled by
                // retry_on_killed_session.js.
                assert.neq(executionCtxType, this.executionContextTypes.kClientRetryableWrite);
                // For the client transaction case, interrupt errors should be handled by the
                // withTxnAndAutoRetry wrapper.
                assert.neq(executionCtxType, this.executionContextTypes.kClientTransaction);
                return;
            }
            throw e;
        }
    };

    $config.setup = function(db, collName, cluster) {
        if (cluster.isSharded()) {
            // Set the transactionLifetimeLimitSeconds to 60 seconds so that cross-shard
            // transactions that start between when setFCV aborts unprepared transactions and when
            // setFCV starts waiting for the global lock do not get stuck trying to enter the
            // "prepared" state (since persisting the participant list requires the IX lock on the
            // config.transaction_coordinators collection).
            // TODO (SERVER-66213): Use the default transactionLifetimeLimitSeconds.
            this.lowerTransactionLifetimeLimitSeconds = true;
        }
        $super.setup.apply(this, arguments);
    };

    const fcvValues = [lastLTSFCV, lastContinuousFCV, latestFCV];

    $config.teardown = function(db, collName, cluster) {
        $super.teardown.apply(this, arguments);
        assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: latestFCV}));
    };

    $config.states.setFCV = function(db, collName) {
        if (this.tid === 0) {
            // Only allow one thread to run setFCV since there can only be one setFCV operation at
            // any given time anyway. It is better to make the other threads run more internal
            // transactions than making them wait to run setFCV.
            const targetFCV = fcvValues[Random.randInt(3)];
            print("Starting setFCV state, setting to: " + targetFCV);
            try {
                assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: targetFCV}));
            } catch (e) {
                if (e.code === 5147403) {
                    // Invalid fcv transition (e.g lastContinuous -> lastLTS)
                    print("setFCV: Invalid transition");
                    return;
                }
                throw e;
            }
            print("Finished setFCV state");
        }
    };

    // Runs concurrent retryable writes to verify they always observe the partial index after
    // upgrade.
    $config.states.retryableWrite = function(db, collName) {
        const retryableWriteSession = db.getMongo().startSession(
            {causalConsistency: this.shouldUseCausalConsistency, retryWrites: true});
        const retryableWriteDB = retryableWriteSession.getDatabase(db.getName());

        print("Starting retryable write state, session: " +
              tojsononeline(retryableWriteSession.getSessionId()));

        const docId = UUID();
        for (let i = 0; i < 10; ++i) {
            const res = assert.commandWorked(
                retryableWriteDB[collName].update({_id: docId, forRetryableWriteState: true},
                                                  {$inc: {counter: 1}, $set: {idx: i}},
                                                  {upsert: true}));
            if (i === 0) {
                assert.eq(res.nUpserted, 1, tojson(res));
            } else {
                assert.eq(res.nModified, 1, tojson(res));
            }
        }
        print("Finished retryable write state");
    };

    if ($config.passConnectionCache) {
        // If 'passConnectionCache' is true, every state function must accept 3 parameters: db,
        // collName and connCache. This workload does not set 'passConnectionCache' since it doesn't
        // use 'connCache' but it may extend a sharding workload that uses it.
        const originalSetFCV = $config.states.setFCV;
        $config.states.setFCV = function(db, collName, connCache) {
            originalSetFCV.call(this, db, collName);
        };
    }

    $config.transitions = {
        init: {
            setFCV: 0.18,
            internalTransactionForInsert: 0.18,
            internalTransactionForUpdate: 0.18,
            internalTransactionForDelete: 0.18,
            internalTransactionForFindAndModify: 0.18,
            retryableWrite: 0.1,
        },
        setFCV: {
            setFCV: 0.15,
            internalTransactionForInsert: 0.15,
            internalTransactionForUpdate: 0.15,
            internalTransactionForDelete: 0.15,
            internalTransactionForFindAndModify: 0.15,
            retryableWrite: 0.1,
            verifyDocuments: 0.15
        },
        internalTransactionForInsert: {
            setFCV: 0.3,
            internalTransactionForInsert: 0.1,
            internalTransactionForUpdate: 0.1,
            internalTransactionForDelete: 0.1,
            internalTransactionForFindAndModify: 0.1,
            retryableWrite: 0.1,
            verifyDocuments: 0.2
        },
        internalTransactionForUpdate: {
            setFCV: 0.3,
            internalTransactionForInsert: 0.1,
            internalTransactionForUpdate: 0.1,
            internalTransactionForDelete: 0.1,
            internalTransactionForFindAndModify: 0.1,
            retryableWrite: 0.1,
            verifyDocuments: 0.2
        },
        internalTransactionForDelete: {
            setFCV: 0.3,
            internalTransactionForInsert: 0.1,
            internalTransactionForUpdate: 0.1,
            internalTransactionForDelete: 0.1,
            internalTransactionForFindAndModify: 0.1,
            retryableWrite: 0.1,
            verifyDocuments: 0.2
        },
        internalTransactionForFindAndModify: {
            setFCV: 0.3,
            internalTransactionForInsert: 0.1,
            internalTransactionForUpdate: 0.1,
            internalTransactionForDelete: 0.1,
            internalTransactionForFindAndModify: 0.1,
            retryableWrite: 0.1,
            verifyDocuments: 0.2
        },
        verifyDocuments: {
            setFCV: 0.3,
            internalTransactionForInsert: 0.1,
            internalTransactionForUpdate: 0.1,
            internalTransactionForDelete: 0.1,
            internalTransactionForFindAndModify: 0.1,
            retryableWrite: 0.1,
            verifyDocuments: 0.2
        },
        retryableWrite: {
            setFCV: 0.3,
            internalTransactionForInsert: 0.1,
            internalTransactionForUpdate: 0.1,
            internalTransactionForDelete: 0.1,
            internalTransactionForFindAndModify: 0.1,
            retryableWrite: 0.1,
            verifyDocuments: 0.2
        }
    };

    return $config;
});

/**
 * Runs insert, update, delete and findAndModify commands in internal transactions using all the
 * available client session settings, and occasionally kills a random session.
 *
 * @tags: [
 *  requires_fcv_60,
 *  uses_transactions,
 *  kills_random_sessions,
 * ]
 */

import "jstests/libs/override_methods/retry_on_killed_session.js";

import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {killSession} from "jstests/concurrency/fsm_workload_helpers/kill_session.js";
import {$config as $baseConfig} from "jstests/concurrency/fsm_workloads/txns/internal_transactions/internal_transactions_unsharded.js";
import {KilledSessionUtil} from "jstests/libs/killed_session_util.js";

export const $config = extendWorkload($baseConfig, function ($config, $super) {
    $config.data.retryOnKilledSession = true;

    // Insert initial documents during setup instead of the init state, otherwise the insert could
    // get interrupted by the killSession state.
    $config.data.insertInitialDocsOnSetUp = true;

    // The transaction API does not abort internal transactions that are interrupted after they
    // have started to commit. The first retry of that transaction will abort the open transaction,
    // but will block if it happens again on that retry, so we lower the
    // transactionLifetimeLimitSeconds so subsequent retries do not block indefinitely (24 hours).
    $config.data.lowerTransactionLifetimeLimitSeconds = true;

    $config.data.expectDirtyDocs = {
        // The client is either not using a session or is using a session without retryable writes
        // enabled. Therefore, when a write is interrupted, they cannot retry the write to verify if
        // it has been executed or not.
        [$super.data.executionContextTypes.kNoClientSession]: true,
        [$super.data.executionContextTypes.kClientSession]: true,
        // retry_on_killed_session.js handles retrying retryable writes upon interrupt errors.
        [$super.data.executionContextTypes.kClientRetryableWrite]: false,
        // The withTxnAndAutoRetry wrapper handles retrying transactions upon interrupt errors (by
        // retrying just the commit or the entire transaction).
        [$super.data.executionContextTypes.kClientTransaction]: false,
    };

    // Threads only begin killing sessions once every thread has finished init(), where a thread
    // will decrement killCountdownLatch.
    $config.data.killCountdown = new CountDownLatch($config.threadCount);

    $config.data.runInternalTransaction = function runInternalTransaction(
        defaultDb,
        collName,
        executionCtxType,
        crudOp,
    ) {
        try {
            $super.data.runInternalTransaction.apply(this, arguments);
        } catch (e) {
            if (KilledSessionUtil.hasKilledSessionError(e) || KilledSessionUtil.hasKilledSessionWCError(e)) {
                return;
            }
            if (e.code == ErrorCodes.NoSuchTransaction) {
                // For an unprepared transaction, the race between commitTransaction with
                // non-default write concern and killSessions can lead to a NoSuchTransaction
                // error.
                // - The killSessions command aborts all unprepared transaction on that replica set
                //   including the transaction above.
                // - The commitTransaction command starts and fails with NoSuchTransaction. To obey
                //   the write concern, it starts writing a NoSuchTransaction noop oplog entry.
                // - The killSessions command kills all local operations on that session. The
                //   NoSuchTransaction noop write is interrupted and so the commitTransaction
                //   command fails with an Interrupted error as the write concern error. The
                //   NoSuchTransaction is is returned without a TransientTransactionError label, and
                //   so the transaction does not get retried by the transaction API.
                this.expectDirtyDocs[executionCtxType] = true;
                return;
            }
            throw e;
        }
    };

    $config.states.killSession = function (db, collName) {
        if ($config.data.killCountdown.getCount() > 0) {
            return;
        }
        return killSession(db, collName);
    };

    $config.teardown = function teardown(db, collName, cluster) {
        $super.teardown.apply(this, arguments);

        let currOps = db
            .getSiblingDB("admin")
            .aggregate([
                {
                    "$currentOp": {
                        "allUsers": true,
                        "idleConnections": true,
                        "idleSessions": true,
                    },
                },
                {$match: {transaction: {$exists: true}}},
            ])
            .toArray();

        jsTest.log(`Current ops with transactions:\n${tojson(currOps)}`);
    };

    $config.states.init = function init(db, collName) {
        $super.states.init.apply(this, arguments);
        $config.data.killCountdown.countDown();
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
            verifyDocuments: 0.2,
        },
        internalTransactionForInsert: {
            killSession: 0.4,
            internalTransactionForInsert: 0.12,
            internalTransactionForUpdate: 0.12,
            internalTransactionForDelete: 0.12,
            internalTransactionForFindAndModify: 0.12,
            verifyDocuments: 0.12,
        },
        internalTransactionForUpdate: {
            killSession: 0.4,
            internalTransactionForInsert: 0.12,
            internalTransactionForUpdate: 0.12,
            internalTransactionForDelete: 0.12,
            internalTransactionForFindAndModify: 0.12,
            verifyDocuments: 0.12,
        },
        internalTransactionForDelete: {
            killSession: 0.4,
            internalTransactionForInsert: 0.12,
            internalTransactionForUpdate: 0.12,
            internalTransactionForDelete: 0.12,
            internalTransactionForFindAndModify: 0.12,
            verifyDocuments: 0.12,
        },
        internalTransactionForFindAndModify: {
            killSession: 0.4,
            internalTransactionForInsert: 0.12,
            internalTransactionForUpdate: 0.12,
            internalTransactionForDelete: 0.12,
            internalTransactionForFindAndModify: 0.12,
            verifyDocuments: 0.12,
        },
        verifyDocuments: {
            killSession: 0.4,
            internalTransactionForInsert: 0.12,
            internalTransactionForUpdate: 0.12,
            internalTransactionForDelete: 0.12,
            internalTransactionForFindAndModify: 0.12,
            verifyDocuments: 0.12,
        },
    };

    return $config;
});

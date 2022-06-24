'use strict';

/**
 * Runs insert, update, delete and findAndModify commands against a sharded collection inside
 * single-shard and cross-shard internal transactions started on a shard using all the available
 * client session settings, and occasionally kills a random session on the shard. Only runs on
 * sharded clusters.
 *
 * @tags: [
 *  requires_fcv_60,
 *  requires_sharding,
 *  uses_transactions,
 *  antithesis_incompatible
 * ]
 */

load('jstests/concurrency/fsm_libs/extend_workload.js');
load('jstests/concurrency/fsm_workloads/internal_transactions_sharded_from_mongod.js');
load('jstests/libs/override_methods/retry_on_killed_session.js');

var $config = extendWorkload($config, function($config, $super) {
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
    };

    $config.data.runInternalTransaction = function runInternalTransaction(
        defaultDb, collName, executionCtxType, crudOp) {
        assert.neq(executionCtxType, this.executionContextTypes.kClientRetryableWrite);
        assert.neq(executionCtxType, this.executionContextTypes.kClientTransaction);
        try {
            $super.data.runInternalTransaction.apply(this, arguments);
        } catch (e) {
            if (e.code == ErrorCodes.Interrupted) {
                return;
            }
            if (e.writeErrors) {
                for (let writeError of e.writeErrors) {
                    if (writeError.code == ErrorCodes.Interrupted) {
                        return;
                    }
                }
            }
            throw e;
        }
    };

    $config.setup = function setup(db, collName, cluster) {
        $super.setup.apply(this, arguments);
        cluster.executeOnMongosNodes((db) => {
            // Force both mongoses to refresh the router info "config" database so that the
            // aggregate command for $listSessions is routed correctly.
            assert.commandWorked(db.adminCommand({flushRouterConfig: "config"}));
        });
    };

    $config.states.killSession = function(db, collName, connCache) {
        fsm.forceRunningOutsideTransaction(this);

        print("Starting killSession");
        let ourSessionWasKilled;
        do {
            ourSessionWasKilled = false;

            try {
                print("Starting refreshLogicalSessionCacheNow command");
                let res = this.mongo.adminCommand({refreshLogicalSessionCacheNow: 1});
                if (res.ok === 1) {
                    assert.commandWorked(res);
                } else if (res.code === 18630 || res.code === 18631) {
                    // Refreshing the logical session cache may trigger sharding the sessions
                    // collection, which can fail with 18630 or 18631 if its session is killed while
                    // running DBClientBase::getCollectionInfos() or DBClientBase::getIndexSpecs(),
                    // respectively. This means the collection is not set up, so retry.
                    ourSessionWasKilled = true;
                    continue;
                } else {
                    assert.commandFailedWithCode(res, ErrorCodes.DuplicateKey);
                }
                print("Finished refreshLogicalSessionCacheNow command");

                print("Starting listSessions");
                const sessions =
                    db.getSiblingDB("config")
                        .system.sessions
                        .aggregate([
                            {$listSessions: {}},
                            {
                                $match: {
                                    $and: [
                                        {lastUse: {$gt: this.initTime}},
                                        {"_id.id": {$ne: db.getSession().getSessionId().id}}
                                    ]
                                }
                            },
                            {$sample: {size: 1}},
                        ])
                        .toArray();
                print("Finished listSessions " + tojsononeline(sessions));

                if (sessions.length === 0) {
                    break;
                }

                const sessionUUID = sessions[0]._id.id;
                print("Starting killSessions command");
                assert.commandWorked(db.adminCommand({killSessions: [{id: sessionUUID}]}));
                print("Finished killSessions command");
            } catch (e) {
                print("killSessions error " + tojsononeline(e));
                if (isNetworkError(e) || isRetryableError(e)) {
                    print("Starting new sessions after listSessions or killSessions error");
                    this.startSessions();
                    // When causal consistency is required, the verifyDocuments state would perform
                    // reads against mongos with afterClusterTime equal to the max of the
                    // clusterTimes of all sessions that it has created on the shard that it uses to
                    // run internal transactions from. Bump the clusterTime on the mongos after the
                    // shard has recovered so that the mongos can gossip the clusterTime correctly
                    // to the other shard; otherwise when the next state is the verifyDocuments
                    // state, the afterClusterTime in the command could be higher than the
                    // clusterTime known to that shard and that would cause the command to fail.
                    this.bumpClusterTime(db, collName);
                    continue;
                }
                if (KilledSessionUtil.isKilledSessionCode(e.code)) {
                    // This session was killed when running either listSessions or killSesssions.
                    // We should retry.
                    ourSessionWasKilled = true;
                    continue;
                }
                throw e;
            }
        } while (ourSessionWasKilled);
        print("Finished killSession");
    };

    $config.transitions = {
        init: {
            killSession: 0.25,
            internalTransactionForInsert: 0.25,
            internalTransactionForUpdate: 0.25,
            internalTransactionForDelete: 0.25,
        },
        killSession: {
            internalTransactionForInsert: 0.25,
            internalTransactionForUpdate: 0.25,
            internalTransactionForDelete: 0.25,
            verifyDocuments: 0.25
        },
        internalTransactionForInsert: {
            killSession: 0.4,
            internalTransactionForInsert: 0.15,
            internalTransactionForUpdate: 0.15,
            internalTransactionForDelete: 0.15,
            verifyDocuments: 0.15
        },
        internalTransactionForUpdate: {
            killSession: 0.4,
            internalTransactionForInsert: 0.15,
            internalTransactionForUpdate: 0.15,
            internalTransactionForDelete: 0.15,
            verifyDocuments: 0.15
        },
        internalTransactionForDelete: {
            killSession: 0.4,
            internalTransactionForInsert: 0.15,
            internalTransactionForUpdate: 0.15,
            internalTransactionForDelete: 0.15,
            verifyDocuments: 0.15
        },
        internalTransactionForFindAndModify: {
            killSession: 0.4,
            internalTransactionForInsert: 0.15,
            internalTransactionForUpdate: 0.15,
            internalTransactionForDelete: 0.15,
            verifyDocuments: 0.15
        },
        verifyDocuments: {
            killSession: 0.25,
            internalTransactionForInsert: 0.25,
            internalTransactionForUpdate: 0.25,
            internalTransactionForDelete: 0.25,
        }
    };

    return $config;
});

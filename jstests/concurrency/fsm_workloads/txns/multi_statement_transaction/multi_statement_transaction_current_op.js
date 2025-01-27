/**
 * Runs update, findAndModify, delete, find, and getMore in a transaction with all threads using the
 * same session.
 *
 * @tags: [
 *      assumes_snapshot_transactions,
 *      requires_sharding,
 *      state_functions_share_transaction,
 *      uses_curop_agg_stage,
 *      uses_transactions,
 *      requires_getmore,
 * ]
 */

import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {
    $config as $baseConfig
} from
    "jstests/concurrency/fsm_workloads/txns/multi_statement_transaction/multi_statement_transaction_all_commands.js";

export const $config = extendWorkload($baseConfig, function($config, $super) {
    $config.data.verifyMongosSessionsWithTxns = function verifyMongosSessionsWithTxns(sessions) {
        const acceptableReadConcernLevels = ['snapshot', 'local'];
        sessions.forEach((session) => {
            const transactionDocument = session.transaction;

            assert.gte(transactionDocument.parameters.txnNumber, 0);
            assert.eq(transactionDocument.parameters.autocommit, false);
            if (transactionDocument.parameters.readConcern !== undefined) {
                assert(acceptableReadConcernLevels.includes(
                    transactionDocument.parameters.readConcern.level));
            }
            if (transactionDocument.globalReadTimestamp !== undefined) {
                assert.gt(transactionDocument.globalReadTimestamp, Timestamp(0, 0));
            }
            assert.gt(ISODate(transactionDocument.startWallClockTime),
                      ISODate("1970-01-01T00:00:00.000Z"));

            assert.hasFields(transactionDocument,
                             ["timeOpenMicros", "timeActiveMicros", "timeInactiveMicros"]);
            const timeOpen = Number(transactionDocument["timeOpenMicros"]);
            const timeActive = Number(transactionDocument["timeActiveMicros"]);
            const timeInactive = Number(transactionDocument["timeInactiveMicros"]);

            assert.gte(timeOpen, 0);
            assert.gte(timeActive, 0);
            assert.gte(timeInactive, 0);
            assert.eq(timeActive + timeInactive, timeOpen, () => tojson(transactionDocument));

            if (transactionDocument.numParticipants > 0) {
                const participants = transactionDocument.participants;
                assert.eq(transactionDocument.numParticipants, participants.length);

                let hasCoordinator = false;
                let numNonReadOnly = 0;
                let numReadOnly = 0;
                participants.forEach((participant) => {
                    if (participant.coordinator) {
                        assert.eq(hasCoordinator, false);
                        hasCoordinator = true;
                    }

                    if (participant.hasOwnProperty('readOnly')) {
                        if (participant.readOnly) {
                            ++numReadOnly;
                        } else {
                            ++numNonReadOnly;
                        }
                    }
                });

                assert.eq(hasCoordinator, true);
                assert.eq(transactionDocument.numNonReadOnlyParticipants, numNonReadOnly);
                assert.eq(transactionDocument.numReadOnlyParticipants, numReadOnly);
            }
        });
    };

    $config.states.runCurrentOp = function runCurrentOp(db, collName) {
        const admin = db.getSiblingDB("admin");
        const mongosSessionsWithTransactions = admin
                                                   .aggregate([
                                                       {
                                                           $currentOp: {
                                                               allUsers: true,
                                                               idleSessions: true,
                                                               idleConnections: true,
                                                               localOps: true
                                                           }
                                                       },
                                                       {$match: {transaction: {$exists: true}}}
                                                   ])
                                                   .toArray();

        this.verifyMongosSessionsWithTxns(mongosSessionsWithTransactions);
    };

    $config.transitions = {
        init: {
            runCurrentOp: .2,
            runFindAndModify: .16,
            runUpdate: .16,
            runDelete: .16,
            runFindAndGetMore: .16,
            commitTxn: .16
        },
        runCurrentOp: {
            runCurrentOp: .2,
            runFindAndModify: .16,
            runUpdate: .16,
            runDelete: .16,
            runFindAndGetMore: .16,
            commitTxn: .16
        },
        runFindAndModify: {
            runCurrentOp: .2,
            runFindAndModify: .16,
            runUpdate: .16,
            runDelete: .16,
            runFindAndGetMore: .16,
            commitTxn: .16
        },
        runUpdate: {
            runCurrentOp: .2,
            runFindAndModify: .16,
            runUpdate: .16,
            runDelete: .16,
            runFindAndGetMore: .16,
            commitTxn: .16
        },
        runDelete: {
            runCurrentOp: .2,
            runFindAndModify: .16,
            runUpdate: .16,
            runDelete: .16,
            runFindAndGetMore: .16,
            commitTxn: .16
        },
        runFindAndGetMore: {
            runCurrentOp: .2,
            runFindAndModify: .16,
            runUpdate: .16,
            runDelete: .16,
            runFindAndGetMore: .16,
            commitTxn: .16
        },
        commitTxn: {
            runCurrentOp: .1,
            runFindAndModify: .225,
            runUpdate: .225,
            runDelete: .225,
            runFindAndGetMore: .225
        },
    };

    return $config;
});

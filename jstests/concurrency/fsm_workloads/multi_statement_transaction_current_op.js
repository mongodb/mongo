'use strict';

/**
 * Runs update, findAndModify, delete, find, and getMore in a transaction with all threads using the
 * same session.
 *
 * @tags: [
 *      assumes_snapshot_transactions,
 *      requires_sharding,
 *      state_functions_share_transaction,
 *      uses_curop_agg_stage,
 *      uses_transactions
 * ]
 */

load('jstests/concurrency/fsm_libs/extend_workload.js');  // for extendWorkload
load('jstests/concurrency/fsm_workloads/multi_statement_transaction_all_commands.js');  // for
                                                                                        // $config

var $config = extendWorkload($config, function($config, $super) {
    $config.data.verifyMongosSessionsWithTxns = function verifyMongosSessionsWithTxns(sessions) {
        const acceptableReadConcernLevels = ['snapshot', 'local'];
        sessions.forEach((session) => {
            jsTestLog("xxx here is session: " + tojson(session));
            const transactionDocument = session.transaction;

            assertAlways.gte(transactionDocument.parameters.txnNumber, 0);
            assertAlways.eq(transactionDocument.parameters.autocommit, false);
            if (transactionDocument.parameters.readConcern !== undefined) {
                assertAlways(acceptableReadConcernLevels.includes(
                    transactionDocument.parameters.readConcern.level));
            }
            if (transactionDocument.globalReadTimestamp !== undefined) {
                assertAlways.gt(transactionDocument.globalReadTimestamp, Timestamp(0, 0));
            }
            assertAlways.gt(ISODate(transactionDocument.startWallClockTime),
                            ISODate("1970-01-01T00:00:00.000Z"));

            if (transactionDocument.numParticipants > 0) {
                const participants = transactionDocument.participants;
                assertAlways.eq(transactionDocument.numParticipants, participants.length);

                let hasCoordinator = false;
                let numNonReadOnly = 0;
                let numReadOnly = 0;
                participants.forEach((participant) => {
                    if (participant.coordinator) {
                        assertAlways.eq(hasCoordinator, false);
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

                assertAlways.eq(hasCoordinator, true);
                assertAlways.eq(transactionDocument.numNonReadOnlyParticipants, numNonReadOnly);
                assertAlways.eq(transactionDocument.numReadOnlyParticipants, numReadOnly);
            }
        });
    };

    $config.states.runCurrentOp = function runCurrentOp(db, collName) {
        const admin = db.getSiblingDB("admin");
        const mongosSessionsWithTransactions =
            admin
                .aggregate([
                    {
                        $currentOp: {
                            allUsers: true,
                            idleSessions: true,
                            idleConnections: true,
                            localOps: true
                        }
                    },
                    {$match: {$or: [{type: 'idleSession'}, {type: 'activeSession'}]}}
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

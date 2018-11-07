'use strict';

/**
 * Runs findAndModify, update, delete, find, and getMore within a transaction.
 *
 * @tags: [uses_transactions]
 */
load('jstests/concurrency/fsm_workload_helpers/cleanup_txns.js');
var $config = (function() {

    function quietly(func) {
        const printOriginal = print;
        try {
            print = Function.prototype;
            func();
        } finally {
            print = printOriginal;
        }
    }

    function autoRetryTxn(data, func) {
        // joinAndRetry is true either if there is a TransientTransactionError or if
        // startTransaction fails with ConflictingOperationInProgress. The latter occurs when we
        // attempt to start a transaction with a txnNumber that is already active on this session.
        // In both cases, we will re-run the command with this txnNumber without calling
        // startTransaction, and essentially join the already running transaction.
        let joinAndRetry = false;

        do {
            try {
                joinAndRetry = false;

                func();
            } catch (e) {
                if (e.code === ErrorCodes.TransactionTooOld ||
                    e.code === ErrorCodes.NoSuchTransaction ||
                    e.code === ErrorCodes.TransactionCommitted) {
                    // We pass `ignoreActiveTxn = true` to startTransaction so that we will not
                    // throw `Transaction already in progress on this session` when trying to start
                    // a new transaction on this client session that already has an active
                    // transaction on it. We instead will catch the ConflictingOperationInProgress
                    // error that the server later throws, and will re-run the command with
                    // 'startTransaction = false' so that we join the already running transaction.
                    data.session.startTransaction_forTesting({readConcern: {level: 'snapshot'}},
                                                             {ignoreActiveTxn: true});
                    data.txnNumber++;
                    joinAndRetry = true;
                    continue;
                }

                if ((e.hasOwnProperty('errorLabels') &&
                     e.errorLabels.includes('TransientTransactionError')) ||
                    e.code === ErrorCodes.ConflictingOperationInProgress) {
                    joinAndRetry = true;
                    continue;
                }

                throw e;
            }
        } while (joinAndRetry);
    }

    const states = {

        init: function init(db, collName) {
            this.session = db.getMongo().startSession({causalConsistency: true});
            this.sessionDb = this.session.getDatabase(db.getName());
            this.iteration = 1;

            this.session.startTransaction_forTesting({readConcern: {level: 'snapshot'}});
            this.txnNumber = 0;
        },

        runFindAndModify: function runFindAndModify(db, collName) {
            autoRetryTxn(this, () => {
                const collection = this.session.getDatabase(db.getName()).getCollection(collName);
                assertAlways.commandWorked(collection.runCommand(
                    'findAndModify', {query: {_id: this.tid}, update: {$inc: {x: 1}}, new: true}));
            });
        },

        runUpdate: function runUpdate(db, collName) {
            autoRetryTxn(this, () => {
                const collection = this.session.getDatabase(db.getName()).getCollection(collName);
                assertAlways.commandWorked(collection.runCommand('update', {
                    updates: [{q: {_id: this.tid}, u: {$inc: {x: 1}}}],
                }));
            });
        },

        runDelete: function runDelete(db, collName) {
            autoRetryTxn(this, () => {
                const collection = this.session.getDatabase(db.getName()).getCollection(collName);
                assertAlways.commandWorked(collection.runCommand('delete', {
                    deletes: [{q: {_id: this.tid}, limit: 1}],
                }));
            });
        },

        runFindAndGetMore: function runFindAndGetMore(db, collName) {
            autoRetryTxn(this, () => {
                const collection = this.session.getDatabase(db.getName()).getCollection(collName);
                const documents = collection.find().batchSize(2).toArray();
            });
        },

        commitTxn: function commitTxn(db, collName) {
            // shouldJoin is true when commitTransaction fails with ConflictingOperationInProgress.
            // This occurs when there's a transaction with the same txnNumber running on this
            // session. In this case we "join" this other transaction and retry the commit, meaning
            // all operations that were run on this thread will be committed in the same transaction
            // as the transaction we join.
            let shouldJoin;
            do {
                try {
                    shouldJoin = false;
                    quietly(() => this.session.commitTransaction());
                } catch (e) {
                    if (e.code === ErrorCodes.TransactionTooOld ||
                        e.code === ErrorCodes.TransactionCommitted ||
                        e.code === ErrorCodes.NoSuchTransaction) {
                        // If we get TransactionTooOld, TransactionCommitted, or NoSuchTransaction
                        // we do not try to commit this transaction.
                        break;
                    }

                    if ((e.hasOwnProperty('errorLabels') &&
                         e.errorLabels.includes('TransientTransactionError')) ||
                        e.code === ErrorCodes.ConflictingOperationInProgress) {
                        shouldJoin = true;
                        continue;
                    }

                    throw e;
                }
            } while (shouldJoin);

            this.session.startTransaction_forTesting({readConcern: {level: 'snapshot'}});
            this.txnNumber++;
        },
    };

    // Wrap each state in a cleanupOnLastIteration() invocation.
    for (let stateName of Object.keys(states)) {
        const stateFn = states[stateName];
        const abortErrorCodes = [
            ErrorCodes.NoSuchTransaction,
            ErrorCodes.TransactionCommitted,
            ErrorCodes.TransactionTooOld
        ];
        states[stateName] = function(db, collName) {
            cleanupOnLastIteration(this, () => stateFn.apply(this, arguments), abortErrorCodes);
        };
    }

    function setup(db, collName) {
        assertWhenOwnColl.commandWorked(db.runCommand({create: collName}));
        const bulk = db[collName].initializeUnorderedBulkOp();

        for (let i = 0; i < this.numDocs; ++i) {
            bulk.insert({_id: i, x: i});
        }

        const res = bulk.execute({w: 'majority'});
        assertWhenOwnColl.commandWorked(res);
        assertWhenOwnColl.eq(this.numDocs, res.nInserted);
    }

    function teardown(db, collName, cluster) {
    }

    const transitions = {
        init: {runFindAndModify: .25, runUpdate: .25, runDelete: .25, runFindAndGetMore: .25},
        runFindAndModify: {
            runFindAndModify: .2,
            runUpdate: .2,
            runDelete: .2,
            runFindAndGetMore: .2,
            commitTxn: .2
        },
        runUpdate: {
            runFindAndModify: .2,
            runUpdate: .2,
            runDelete: .2,
            runFindAndGetMore: .2,
            commitTxn: .2
        },
        runDelete: {
            runFindAndModify: .2,
            runUpdate: .2,
            runDelete: .2,
            runFindAndGetMore: .2,
            commitTxn: .2
        },
        runFindAndGetMore: {
            runFindAndModify: .2,
            runUpdate: .2,
            runDelete: .2,
            runFindAndGetMore: .2,
            commitTxn: .2
        },
        commitTxn: {runFindAndModify: .25, runUpdate: .25, runDelete: .25, runFindAndGetMore: .25},
    };

    return {
        threadCount: 5,
        iterations: 10,
        states: states,
        transitions: transitions,
        data: {
            numDocs: 20,
            ignoreActiveTxn: false,
        },
        setup: setup,
        teardown: teardown
    };

})();

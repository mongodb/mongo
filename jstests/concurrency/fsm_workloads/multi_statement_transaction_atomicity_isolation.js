'use strict';

/**
 * multi_statement_transaction_atomicity_isolation.js
 *
 * Inserts a handful of documents into a collection. Each thread then updates multiple documents
 * inside of a multi-statement transaction. The resulting documents are of the form
 *
 *   {_id: 0, order: [{tid: 0, iteration: 0, numUpdated: 3},
 *                    {tid: 1, iteration: 0, numUpdated: 2},
 *                    ...]}
 *
 *   {_id: 1, order: [{tid: 0, iteration: 0, numUpdated: 3},
 *                    {tid: 1, iteration: 0, numUpdated: 2},
 *                    ...]}
 *
 *   {_id: 2, order: [{tid: 0, iteration: 0, numUpdated: 3}, ...]}
 *
 * where the {tid: 0, iteration: 0, numUpdated: 3} element should occur 3 times and should always
 * come before the {tid: 1, iteration: 0, numUpdated: 2} element's 2 occurrences. In other words, we
 * track
 *
 *   (1) the relative order in which each of the transactions commit based on their position within
 *       the array, and
 *
 *   (2) the expected number of occurrences for each element in the array.
 *
 * An anomaly is detected if either
 *
 *   (a) transaction A's (tid, txnNumber, numToUpdate) element precedes transaction B's
 *       (tid, txnNumber, numToUpdate) element in one document and follows it in another. This would
 *       suggest that the database failed to detect a write-write conflict despite both transactions
 *       modifying the same document and is therefore not providing snapshot isolation.
 *
 *   (b) transaction C's (tid, txnNumber, numToUpdate) element doesn't appear numToUpdate times
 *       across a consistent snapshot of all of the documents. This would suggest that the database
 *       failed to atomically update all documents modified in a concurrent transaction.
 *
 *   (c) transaction D's (tid, txnNumber) for a given (docId, dbName, collName) doesn't match the
 *       (tid, txnNumber) for the thread with threadId == tid. This indicates that there are writes
 *       that exist in the database that were not committed.
 *
 * @tags: [uses_transactions, assumes_snapshot_transactions]
 */

// for Graph
load('jstests/libs/cycle_detection.js');

// For withTxnAndAutoRetry, isKilledSessionCode.
load('jstests/concurrency/fsm_workload_helpers/auto_retry_transaction.js');

// For arrayEq.
load("jstests/aggregation/extras/utils.js");

var $config = (function() {
    function checkTransactionCommitOrder(documents) {
        const graph = new Graph();

        for (let doc of documents) {
            const commitOrder = doc.order;
            for (let i = 1; i < commitOrder.length; ++i) {
                // We add an edge from commitOrder[i - 1] to commitOrder[i] because it being earlier
                // in the array that was $push'd to indicates that the commitOrder[i - 1]
                // transaction happened before the commit[i] transaction order.
                graph.addEdge(commitOrder[i - 1], commitOrder[i]);
            }
        }

        const result = graph.findCycle();

        if (result.length > 0) {
            const isOpPartOfCycle = (op) =>
                result.some(cyclicOp => bsonBinaryEqual({_: op}, {_: cyclicOp}));

            const filteredDocuments = documents.map(doc => {
                const filteredCommitOrder = doc.order.filter(isOpPartOfCycle);
                return Object.assign({}, doc, {order: filteredCommitOrder});
            });

            assertWhenOwnColl.eq([], result, tojson(filteredDocuments));
        }
    }

    function checkNumUpdatedByEachTransaction(documents) {
        const updateCounts = new Map();

        for (let doc of documents) {
            for (let op of doc.order) {
                // We store 'op' both as the key and as part of the value because the mongo shell's
                // Map type mangles the key and doesn't provide an API to retrieve the original
                // key-value pairs.
                const value = updateCounts.get(op) || {op, actual: 0};
                updateCounts.put(op, {op, actual: value.actual + 1});
            }
        }

        for (let {op, actual} of updateCounts.values()) {
            assert.eq(op.numUpdated, actual, () => {
                return 'transaction ' + tojson(op) + ' should have updated ' + op.numUpdated +
                    ' documents, but ' + actual + ' were updated: ' + tojson(updateCounts.values());
            });
        }
    }

    // Check that only writes that were committed are reflected in the database. Writes that were
    // committed are reflected in $config.data.updatedDocsClientHistory.
    function checkWritesOfCommittedTxns(data, documents) {
        // updatedDocsServerHistory is a dictionary of {txnNum: [list of {docId, dbName, collName}]}
        // that were updated by this thread (this.tid) and exist in the database.
        let updatedDocsServerHistory = [];
        for (let doc of documents) {
            for (let i = 0; i < doc.order.length; i++) {
                // Pull out only those docIds and txnNums that were updated by this thread.
                if (doc.order[i].tid === data.tid) {
                    const txnNum = doc.metadata[i].txnNum.valueOf();
                    updatedDocsServerHistory[txnNum] = updatedDocsServerHistory[txnNum] || [];
                    updatedDocsServerHistory[txnNum].push({
                        _id: doc._id,
                        dbName: doc.metadata[i].dbName,
                        collName: doc.metadata[i].collName
                    });
                }
            }
        }

        // Assert that any transaction written down in $config.data also exists in the database
        // and that the docIds associated with this txnNum in $config.data match those docIds
        // associated with this txnNum in the database.
        const highestTxnNum =
            Math.max(updatedDocsServerHistory.length, data.updatedDocsClientHistory.length);
        for (let txnNum = 0; txnNum < highestTxnNum; ++txnNum) {
            assertAlways((arrayEq(updatedDocsServerHistory[txnNum] || [],
                                  data.updatedDocsClientHistory[txnNum] || [])),
                         () => 'expected ' + tojson(data.updatedDocsClientHistory[txnNum]) +
                             ' but instead have ' + tojson(updatedDocsServerHistory[txnNum]) +
                             ' for txnNumber ' + txnNum);
        }
    }

    // Overridable functions for subclasses to do more complex transactions.
    const getAllCollections = (db, collName) => [db.getCollection(collName)];

    function getAllDocuments(numDocs, {useTxn}) {
        let allDocuments = [];

        if (useTxn) {
            withTxnAndAutoRetry(this.session, () => {
                allDocuments = [];
                for (let collection of this.collections) {
                    const txnDbName = collection.getDB().getName();
                    const txnCollName = collection.getName();
                    const txnCollection =
                        this.session.getDatabase(txnDbName).getCollection(txnCollName);

                    // We intentionally use a smaller batch size when fetching all of the
                    // documents in the collection in order to stress the behavior of reading
                    // from the same snapshot over the course of multiple network roundtrips.
                    const batchSize = Math.max(2, Math.floor(numDocs / 5));
                    allDocuments.push(...txnCollection.find().batchSize(batchSize).toArray());
                }
            }, {
                retryOnKilledSession: this.retryOnKilledSession,
                prepareProbability: this.prepareProbability
            });
        } else {
            for (let collection of this.collections) {
                allDocuments.push(...collection.find().toArray());
            }
        }

        assertWhenOwnColl.eq(allDocuments.length, numDocs * this.collections.length, () => {
            if (this.session) {
                return "txnNumber: " + tojson(this.session.getTxnNumber_forTesting()) +
                    ", session id: " + tojson(this.session.getSessionId()) +
                    ", all documents: " + tojson(allDocuments);
            }
            return "all documents: " + tojson(allDocuments);
        });

        return allDocuments;
    }

    function getDocIdsToUpdate(numDocs) {
        // Generate between [2, numDocs / 2] operations.
        const numOps = 2 + Random.randInt(Math.ceil(numDocs / 2) - 1);
        // Select 'numOps' document (without replacement) to update.
        let docIds = Array.from({length: numDocs}, (value, index) => index);
        return Array.shuffle(docIds).slice(0, numOps);
    }

    const states = (function() {
        return {
            init: function init(db, collName) {
                this.iteration = 0;
                // Set causalConsistency = true to ensure that in the checkConsistency state
                // function, we will be able to read our own writes that were committed as a
                // part of the update state function.
                this.session = db.getMongo().startSession({causalConsistency: true});
                this.collections = this.getAllCollections(db, collName);
                this.totalIteration = 1;
            },

            update: function update(db, collName) {
                const docIds = this.getDocIdsToUpdate(this.numDocs);
                let committedTxnInfo;
                let txnNumber;

                withTxnAndAutoRetry(this.session, () => {
                    committedTxnInfo = [];
                    for (let [i, docId] of docIds.entries()) {
                        const collection =
                            this.collections[Random.randInt(this.collections.length)];
                        const txnDbName = collection.getDB().getName();
                        const txnCollName = collection.getName();
                        txnNumber = this.session.getTxnNumber_forTesting();

                        // We apply the following update to each of the 'docIds' documents
                        // to record the number of times we expect to see the transaction
                        // being run in this execution of the update() state function by this
                        // worker thread present across all documents. Using the $push
                        // operator causes a transaction which commits after another
                        // transaction to appear later in the array.
                        const updateMods = {
                            $push: {
                                order: {
                                    tid: this.tid,
                                    iteration: this.iteration,
                                    numUpdated: docIds.length,
                                },
                                metadata: {
                                    dbName: txnDbName,
                                    collName: txnCollName,
                                    txnNum: txnNumber,
                                }
                            }
                        };

                        const txnCollection =
                            this.session.getDatabase(txnDbName).getCollection(txnCollName);
                        const coll = txnCollection.find().toArray();
                        const res = txnCollection.runCommand('update', {
                            updates: [{q: {_id: docId}, u: updateMods}],
                        });
                        assertAlways.commandWorked(
                            res,
                            () => "Failed to update. result: " + tojson(res) +
                                ", collection: " + tojson(coll));
                        assertWhenOwnColl.eq(res.n, 1, () => tojson(res));
                        assertWhenOwnColl.eq(res.nModified, 1, () => tojson(res));
                        committedTxnInfo.push(
                            {_id: docId, dbName: txnDbName, collName: txnCollName});
                    }
                }, {
                    retryOnKilledSession: this.retryOnKilledSession,
                    prepareProbability: this.prepareProbability
                });

                this.updatedDocsClientHistory[txnNumber.valueOf()] = committedTxnInfo;
                ++this.iteration;
            },

            checkConsistency: function checkConsistency(db, collName) {
                const documents = this.getAllDocuments(this.numDocs, {useTxn: true});
                checkTransactionCommitOrder(documents);
                checkNumUpdatedByEachTransaction(documents);
                checkWritesOfCommittedTxns(this, documents);
            },

            causalRead: function causalRead(db, collName) {
                const collection = this.collections[Random.randInt(this.collections.length)];
                const randomDbName = collection.getDB().getName();
                const randomCollName = collection.getName();

                // We do a non-transactional read on a causally consistent session so that it uses
                // 'afterClusterTime' internally and is subject to prepare conflicts (in a sharded
                // cluster). This is meant to expose deadlocks involving prepare conflicts outside
                // of transactions.
                const sessionCollection =
                    this.session.getDatabase(randomDbName).getCollection(randomCollName);

                try {
                    const cursor = sessionCollection.find();
                    assertAlways.eq(cursor.itcount(), this.numDocs);
                } catch (e) {
                    if (this.retryOnKilledSession && isKilledSessionCode(e.code)) {
                        // If the session is expected to be killed, ignore it.
                        return;
                    }
                    throw e;
                }
            }
        };
    })();

    /**
     * This function wraps the state functions and causes each thread to run checkConsistency()
     * before teardown.
     */
    function checkConsistencyOnLastIteration(data, func, checkConsistencyFunc) {
        let lastIteration = ++data.totalIteration >= data.iterations;
        func();
        if (lastIteration) {
            checkConsistencyFunc();
        }
    }

    // Wrap each state in a checkConsistencyOnLastIteration() invocation.
    for (let stateName of Object.keys(states)) {
        const stateFn = states[stateName];
        const checkConsistencyFn = states['checkConsistency'];
        states[stateName] = function(db, collName) {
            checkConsistencyOnLastIteration(this,
                                            () => stateFn.apply(this, arguments),
                                            () => checkConsistencyFn.apply(this, arguments));
        };
    }

    const transitions = {
        init: {update: 0.9, checkConsistency: 0.1},
        update: {update: 0.8, checkConsistency: 0.1, causalRead: 0.1},
        checkConsistency: {update: 1},
        causalRead: {update: 1},
    };

    function setup(db, collName, cluster) {
        // The default WC is majority and this workload may not be able to satisfy majority writes.
        if (cluster.isSharded()) {
            cluster.executeOnMongosNodes(function(db) {
                assert.commandWorked(db.adminCommand({
                    setDefaultRWConcern: 1,
                    defaultWriteConcern: {w: 1},
                    writeConcern: {w: "majority"}
                }));
            });
        } else if (cluster.isReplication()) {
            assert.commandWorked(db.adminCommand({
                setDefaultRWConcern: 1,
                defaultWriteConcern: {w: 1},
                writeConcern: {w: "majority"}
            }));
        }

        this.collections = this.getAllCollections(db, collName);

        for (let collection of this.collections) {
            const bulk = collection.initializeUnorderedBulkOp();

            for (let i = 0; i < this.numDocs; ++i) {
                bulk.insert({_id: i, order: []});
            }

            const res = bulk.execute({w: 'majority'});
            assertWhenOwnColl.commandWorked(res);
            assertWhenOwnColl.eq(this.numDocs, res.nInserted);
        }

        if (cluster.isSharded()) {
            // Advance each router's cluster time to be >= the time of the writes, so the first
            // global snapshots chosen by each is guaranteed to include the inserted documents.
            cluster.synchronizeMongosClusterTimes();
        }
    }

    function teardown(db, collName, cluster) {
        const documents = this.getAllDocuments(this.numDocs, {useTxn: false});
        checkTransactionCommitOrder(documents);
        checkNumUpdatedByEachTransaction(documents);

        // Unsetting CWWC is not allowed, so explicitly restore the default write concern to be
        // majority by setting CWWC to {w: majority}.
        if (cluster.isSharded()) {
            cluster.executeOnMongosNodes(function(db) {
                assert.commandWorked(db.adminCommand({
                    setDefaultRWConcern: 1,
                    defaultWriteConcern: {w: "majority"},
                    writeConcern: {w: "majority"}
                }));
            });
        } else if (cluster.isReplication()) {
            assert.commandWorked(db.adminCommand({
                setDefaultRWConcern: 1,
                defaultWriteConcern: {w: "majority"},
                writeConcern: {w: "majority"}
            }));
        }
    }

    return {
        threadCount: 10,
        iterations: 50,
        states: states,
        transitions: transitions,
        data: {
            getAllCollections,
            getAllDocuments,
            getDocIdsToUpdate,
            numDocs: 10,
            // Because of the usage of 'getMore' command in this test, we may receive
            // 'CursorNotFound' exception from the server, if a node was stepped down between the
            // 'find' and subsequent 'getMore' command. We retry the entire transaction in this
            // case.
            retryOnKilledSession: TestData.runningWithShardStepdowns,
            updatedDocsClientHistory: [],
        },
        setup: setup,
        teardown: teardown,
    };
})();

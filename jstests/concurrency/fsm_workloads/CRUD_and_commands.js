'use strict';

/**
 * Perform CRUD operations, some of which may implicitly create collections, in parallel with
 * collection-dropping operations.
 *
 * @tags: [
 * ]
 */
var $config = (function() {
    const data = {numIds: 10, docValue: "mydoc"};

    const states = {
        init: function init(db, collName) {
            this.session = db.getMongo().startSession({causalConsistency: true});
            this.sessionDb = this.session.getDatabase(db.getName());
        },

        insertDocs: function insertDocs(db, collName) {
            try {
                for (let i = 0; i < 5; i++) {
                    const res = db[collName].insert({value: this.docValue, num: 1});
                    assertWhenOwnColl.commandWorked(res);
                    assertWhenOwnColl.eq(1, res.nInserted);
                }
            } catch (e) {
                if (e.code == ErrorCodes.ConflictingOperationInProgress) {
                    // dropCollection in sharding can disrupt routing cache refreshes.
                    if (TestData.runInsideTransaction) {
                        e["errorLabels"] = ["TransientTransactionError"];
                        throw e;
                    }
                }
            }
        },

        updateDocs: function updateDocs(db, collName) {
            for (let i = 0; i < 5; ++i) {
                let indexToUpdate = Math.floor(Math.random() * this.numIds);
                let res;
                try {
                    res =
                        db[collName].update({_id: indexToUpdate}, {$inc: {num: 1}}, {upsert: true});
                    assertWhenOwnColl.commandWorked(res);
                } catch (e) {
                    // We propagate TransientTransactionErrors to allow the state function to
                    // automatically be retried when TestData.runInsideTransaction=true
                    if (e.hasOwnProperty('errorLabels') &&
                        e.errorLabels.includes('TransientTransactionError')) {
                        throw e;
                    } else if (e.code == ErrorCodes.ConflictingOperationInProgress) {
                        // dropCollection in sharding can disrupt routing cache refreshes.
                        if (TestData.runInsideTransaction) {
                            e["errorLabels"] = ["TransientTransactionError"];
                            throw e;
                        }
                    } else if (e.code == ErrorCodes.QueryPlanKilled ||
                               e.code == ErrorCodes.OperationFailed) {
                        // dropIndex can cause queries to throw if these queries
                        // yield.
                    } else {
                        // TODO(SERVER-46651) upsert with concurrent dropCollection can result in
                        // writeErrors if queries yield.
                        assertAlways.writeError(res, "unexpected error: " + tojsononeline(e));
                    }
                }
            }
        },

        findAndModifyDocs: function findAndModifyDocs(db, collName) {
            for (let i = 0; i < 5; ++i) {
                let indexToUpdate = Math.floor(Math.random() * this.numIds);
                let res;
                try {
                    res = db.runCommand({
                        findAndModify: collName,
                        query: {_id: indexToUpdate},
                        update: {$inc: {num: 1}},
                        upsert: true
                    });
                    assertWhenOwnColl.commandWorked(res);
                } catch (e) {
                    // We propagate TransientTransactionErrors to allow the state function to
                    // automatically be retried when TestData.runInsideTransaction=true
                    if (e.hasOwnProperty('errorLabels') &&
                        e.errorLabels.includes('TransientTransactionError')) {
                        throw e;
                    } else if (e.code === ErrorCodes.ConflictingOperationInProgress) {
                        // dropCollection in sharding can disrupt routing cache refreshes.
                        if (TestData.runInsideTransaction) {
                            e["errorLabels"] = ["TransientTransactionError"];
                            throw e;
                        }
                    } else {
                        assertAlways.contains(
                            e.code,
                            [
                                // dropIndex can cause queries to throw if these queries yield.
                                ErrorCodes.QueryPlanKilled
                            ],
                            'unexpected error code: ' + e.code + ': ' + e.message);
                    }
                }
            }
        },

        readDocs: function readDocs(db, collName) {
            for (let i = 0; i < 5; ++i) {
                try {
                    let res = db[collName].findOne({value: this.docValue});
                    if (res !== null) {
                        assertAlways.eq(this.docValue, res.value);
                    }
                } catch (e) {
                    // We propagate TransientTransactionErrors to allow the state function to
                    // automatically be retried when TestData.runInsideTransaction=true
                    if (e.hasOwnProperty('errorLabels') &&
                        e.errorLabels.includes('TransientTransactionError')) {
                        throw e;
                    } else if (e.code == ErrorCodes.ConflictingOperationInProgress) {
                        // dropCollection in sharding can disrupt routing cache refreshes.
                        if (TestData.runInsideTransaction) {
                            e["errorLabels"] = ["TransientTransactionError"];
                            throw e;
                        }
                    } else {
                        // dropIndex or collection drops can cause queries to throw if these queries
                        // yield.
                        assertAlways.contains(
                            e.code,
                            [
                                ErrorCodes.NamespaceNotFound,
                                ErrorCodes.OperationFailed,
                                ErrorCodes.QueryPlanKilled,
                            ],
                            'unexpected error code: ' + e.code + ': ' + e.message);
                    }
                }
            }
        },

        deleteDocs: function deleteDocs(db, collName) {
            let indexToDelete = Math.floor(Math.random() * this.numIds);
            try {
                db[collName].deleteOne({_id: indexToDelete});
            } catch (e) {
                // We propagate TransientTransactionErrors to allow the state function to
                // automatically be retried when TestData.runInsideTransaction=true
                if (e.hasOwnProperty('errorLabels') &&
                    e.errorLabels.includes('TransientTransactionError')) {
                    throw e;
                } else if (e.code == ErrorCodes.ConflictingOperationInProgress) {
                    if (TestData.runInsideTransaction) {
                        e["errorLabels"] = ["TransientTransactionError"];
                        throw e;
                    }
                } else {
                    // dropIndex can cause queries to throw if these queries yield.
                    assertAlways.contains(e.code,
                                          [ErrorCodes.QueryPlanKilled, ErrorCodes.OperationFailed],
                                          'unexpected error code: ' + e.code + ': ' + e.message);
                }
            }
        },

        dropCollection: function dropCollection(db, collName) {
            db[collName].drop();
        }
    };

    const transitions = {
        init: {
            insertDocs: 0.10,
            updateDocs: 0.10,
            findAndModifyDocs: 0.10,
            readDocs: 0.10,
            deleteDocs: 0.10,
            dropCollection: 0.10,
        },
        insertDocs: {
            insertDocs: 0.10,
            updateDocs: 0.10,
            findAndModifyDocs: 0.10,
            readDocs: 0.10,
            deleteDocs: 0.10,
            dropCollection: 0.30,
        },
        updateDocs: {
            insertDocs: 0.10,
            updateDocs: 0.10,
            findAndModifyDocs: 0.10,
            readDocs: 0.10,
            deleteDocs: 0.10,
            dropCollection: 0.30,
        },
        findAndModifyDocs: {
            insertDocs: 0.10,
            updateDocs: 0.10,
            findAndModifyDocs: 0.10,
            readDocs: 0.10,
            deleteDocs: 0.10,
            dropCollection: 0.30,
        },
        readDocs: {
            insertDocs: 0.10,
            updateDocs: 0.10,
            findAndModifyDocs: 0.10,
            readDocs: 0.10,
            deleteDocs: 0.10,
            dropCollection: 0.30,
        },
        deleteDocs: {
            insertDocs: 0.10,
            updateDocs: 0.10,
            findAndModifyDocs: 0.10,
            readDocs: 0.10,
            deleteDocs: 0.10,
            dropCollection: 0.30,
        },
        dropCollection: {
            insertDocs: 0.10,
            updateDocs: 0.10,
            findAndModifyDocs: 0.10,
            readDocs: 0.10,
            deleteDocs: 0.10,
            dropCollection: 0.10,
        }
    };

    function setup(db, collName, cluster) {
        assertAlways.commandWorked(db.runCommand({create: collName}));
        for (let i = 0; i < this.numIds; i++) {
            const res = db[collName].insert({_id: i, value: this.docValue, num: 1});
            assertAlways.commandWorked(res);
            assert.eq(1, res.nInserted);
        }
    }

    return {
        threadCount: 5,
        iterations: 20,
        startState: 'init',
        states: states,
        transitions: transitions,
        setup: setup,
        data: data,
    };
})();

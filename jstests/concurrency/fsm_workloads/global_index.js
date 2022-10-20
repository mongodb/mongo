'use strict';

/**
 * Perform global index CRUD operations, with create and drop commands.
 *
 * @tags: [
 *     featureFlagGlobalIndexes,
 *     requires_fcv_62,
 *     requires_replication
 * ]
 */
var $config = (function() {
    const data = {
        uuidArr: ["47b5c083-8d60-4920-90e2-ba3ff668c371", "8acc9ba2-2d8f-4b01-b835-8f1818c1df1c"],
        range: 5
    };

    function isExpectedCrudError(e) {
        /*
            6924201 - Container does not exist (delete).
            6789402 - Container does not exist (insert).
        */
        return e.code === 6789402 || e.code === 6924201 || e.code === ErrorCodes.KeyNotFound ||
            e.code == ErrorCodes.DuplicateKey ||
            (e.hasOwnProperty('errorLabels') &&
             e.errorLabels.includes('TransientTransactionError'));
    }

    function randInt(max) {
        return Math.floor(Math.random() * max);
    }

    function getUUID(uuidArr) {
        return UUID(uuidArr[randInt(uuidArr.length)]);
    }

    const states = {
        init: function init(db, collName) {
            this.session = db.getMongo().startSession({causalConsistency: true});
            this.sessionDb = this.session.getDatabase(db.getName());
        },

        createGlobalIndex: function createGlobalIndex(db, collName) {
            // _shardsvrCreateGlobalIndex is idempotent, we don't expect any failures.
            assertAlways.commandWorked(
                db.adminCommand({_shardsvrCreateGlobalIndex: getUUID(this.uuidArr)}));
        },

        dropGlobalIndex: function dropGlobalIndex(db, collName) {
            // _shardsvrDropGlobalIndex is idempotent, we don't expect any failures.
            assertAlways.commandWorked(
                db.adminCommand({_shardsvrDropGlobalIndex: getUUID(this.uuidArr)}));
        },

        insertKey: function insertKey(db, collname) {
            const i = randInt(this.range);
            try {
                this.session.startTransaction();
                assertAlways.commandWorked(this.sessionDb.runCommand({
                    "_shardsvrInsertGlobalIndexKey": getUUID(this.uuidArr),
                    key: {a: i},
                    docKey: {sk: 1, _id: i}
                }));
                this.session.commitTransaction();
            } catch (e) {
                this.session.abortTransaction();
                if (!isExpectedCrudError(e)) {
                    throw e;
                }
            }
        },

        deleteKey: function deleteKey(db, collname) {
            const i = randInt(this.range);
            try {
                this.session.startTransaction();
                assertAlways.commandWorked(this.sessionDb.runCommand({
                    "_shardsvrDeleteGlobalIndexKey": getUUID(this.uuidArr),
                    key: {a: i},
                    docKey: {sk: 1, _id: i}
                }));
                this.session.commitTransaction();
            } catch (e) {
                this.session.abortTransaction();
                if (!isExpectedCrudError(e)) {
                    throw e;
                }
            }
        },

        updateKey: function updateKey(db, collname) {
            const i = randInt(this.range);
            const flip = randInt(1);
            const deleteKey = flip ? i + 10000 : i;
            const insertKey = flip ? i : i + 10000;
            const uuid = getUUID(this.uuidArr);
            try {
                this.session.startTransaction();
                assertAlways.commandWorked(this.sessionDb.runCommand({
                    "_shardsvrDeleteGlobalIndexKey": uuid,
                    key: {a: deleteKey},
                    docKey: {sk: 1, _id: i}
                }));
                assertAlways.commandWorked(this.sessionDb.runCommand({
                    "_shardsvrInsertGlobalIndexKey": uuid,
                    key: {a: insertKey},
                    docKey: {sk: 1, _id: i}
                }));
                this.session.commitTransaction();
            } catch (e) {
                this.session.abortTransaction();
                if (!isExpectedCrudError(e)) {
                    throw e;
                }
            }
        },

        bulkInsert: function bulkInsert(db, collname) {
            try {
                const uuid = getUUID(this.uuidArr);
                let ops = [];

                for (let i = 0; i < 200; i++) {
                    ops.push({
                        _shardsvrInsertGlobalIndexKey: uuid,
                        key: {a: this.range + i},
                        docKey: {sk: 1, _id: this.range + i}
                    });
                }

                this.session.startTransaction();
                assertAlways.commandWorked(
                    this.sessionDb.runCommand({_shardsvrWriteGlobalIndexKeys: 1, ops: ops}));
                this.session.commitTransaction();
            } catch (e) {
                this.session.abortTransaction();
                if (!isExpectedCrudError(e)) {
                    throw e;
                }
            }
        },

        bulkDelete: function bulkDelete(db, collname) {
            try {
                const uuid = getUUID(this.uuidArr);
                let ops = [];

                for (let i = 0; i < 200; i++) {
                    ops.push({
                        _shardsvrDeleteGlobalIndexKey: uuid,
                        key: {a: this.range + i},
                        docKey: {sk: 1, _id: this.range + i}
                    });
                }

                this.session.startTransaction();
                assertAlways.commandWorked(
                    this.sessionDb.runCommand({_shardsvrWriteGlobalIndexKeys: 1, ops: ops}));
                this.session.commitTransaction();
            } catch (e) {
                this.session.abortTransaction();
                if (!isExpectedCrudError(e)) {
                    throw e;
                }
            }
        },
    };

    const transitions = {
        init: {
            createGlobalIndex: 1,
            dropGlobalIndex: 1,
            insertKey: 1,
            deleteKey: 1,
            updateKey: 1,
            bulkInsert: 1,
            bulkDelete: 1,
        },
        createGlobalIndex: {
            dropGlobalIndex: 1,
            insertKey: 1,
            deleteKey: 1,
            updateKey: 1,
            bulkInsert: 1,
            bulkDelete: 1,
        },
        dropGlobalIndex: {
            createGlobalIndex: 1,
            insertKey: 1,
            deleteKey: 1,
            updateKey: 1,
            bulkInsert: 1,
            bulkDelete: 1,
        },
        insertKey: {
            createGlobalIndex: 1,
            dropGlobalIndex: 1,
            deleteKey: 1,
            updateKey: 1,
            bulkInsert: 1,
            bulkDelete: 1,
        },
        deleteKey: {
            createGlobalIndex: 1,
            dropGlobalIndex: 1,
            insertKey: 1,
            updateKey: 1,
            bulkInsert: 1,
            bulkDelete: 1,
        },
        updateKey: {
            createGlobalIndex: 1,
            dropGlobalIndex: 1,
            insertKey: 1,
            deleteKey: 1,
            bulkInsert: 1,
            bulkDelete: 1,
        },
        bulkInsert: {
            createGlobalIndex: 1,
            dropGlobalIndex: 1,
            insertKey: 1,
            deleteKey: 1,
            updateKey: 1,
            bulkDelete: 1,
        },
        bulkDelete: {
            createGlobalIndex: 1,
            dropGlobalIndex: 1,
            insertKey: 1,
            deleteKey: 1,
            updateKey: 1,
            bulkInsert: 1,
        }
    };

    function setup(db, collName, cluster) {
    }

    return {
        threadCount: 14,
        iterations: 50,
        startState: 'init',
        states: states,
        transitions: transitions,
        setup: setup,
        data: data,
    };
})();

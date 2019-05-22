'use strict';

/**
 * Transactions with local (and majority) readConcern perform untimestamped reads and do not check
 * the min visible snapshot for collections, so they can access collections whose catalog
 * information does not match the state of the collection in the transaction's snapshot. This test
 * exercises this behavior by starting a transaction on one collection, then performing an operation
 * on a second collection, where another thread may have performed a DDL operation on the second
 * collection since the transaction started. The goal of the test is to ensure the server does not
 * crash in this scenario.
 *
 * Do not run in sharding suites because the first transaction statement is expect to succeed
 * unconditionally, which need not be true in a sharded cluster.
 * @tags: [uses_transactions, requires_replication]
 */

var $config = (function() {

    var states = (function() {

        function init(db, collName) {
            this.session = db.getMongo().startSession();
        }

        function runOpInTxn(
            session, db, startCollName, ddlDBName, ddlCollName, loggingCollName, opName, op) {
            const startColl = session.getDatabase(db.getName())[startCollName];
            const ddlColl = session.getDatabase(ddlDBName)[ddlCollName];

            // Start the transaction and run an operation on 'startColl'.
            session.startTransaction();
            assertWhenOwnColl.eq(1, startColl.find({_id: "startTxnDoc"}).itcount());

            // Run the specified operation on 'ddlColl'. Another thread may have performed a DDL
            // operation on 'ddlColl' since the transaction started. The operation may fail with one
            // of the allowed error codes, but it must not crash the server.
            let success = false;
            try {
                op(ddlColl);
                success = true;
            } catch (e) {
                assertWhenOwnColl.contains(e.code,
                                           [
                                             ErrorCodes.LockTimeout,
                                             ErrorCodes.WriteConflict,
                                             ErrorCodes.SnapshotUnavailable,
                                             ErrorCodes.OperationNotSupportedInTransaction
                                           ],
                                           () => tojson(e));
            }

            // Commit or abort the transaction.
            if (success) {
                assertWhenOwnColl.commandWorked(session.commitTransaction_forTesting());
            } else {
                // The failed operation already aborted the transaction. Run abortTransaction to
                // update the transaction state in the shell.
                assertWhenOwnColl.commandFailedWithCode(session.abortTransaction_forTesting(),
                                                        ErrorCodes.NoSuchTransaction);
            }

            // Record whether the operation succeeded or failed.
            assertWhenOwnColl.commandWorked(
                db[loggingCollName].insert({op: opName, success: success}));
        }

        /**
         * The following functions will be run by threads performing transaction operations.
         */

        function aggregate(db, collName) {
            const op = function(ddlColl) {
                ddlColl.aggregate([{$limit: 1}]).itcount();
            };
            runOpInTxn(this.session,
                       db,
                       collName,
                       this.ddlDBName,
                       this.ddlCollName,
                       this.loggingCollName,
                       "aggregate",
                       op);
        }

        function distinct(db, collName) {
            const op = function(ddlColl) {
                ddlColl.distinct("x");
            };
            runOpInTxn(this.session,
                       db,
                       collName,
                       this.ddlDBName,
                       this.ddlCollName,
                       this.loggingCollName,
                       "distinct",
                       op);
        }

        function findAndModify(db, collName) {
            const op = function(ddlColl) {
                ddlColl.findAndModify({query: {}, sort: {x: 1}, update: {$inc: {x: 1}}});
            };
            runOpInTxn(this.session,
                       db,
                       collName,
                       this.ddlDBName,
                       this.ddlCollName,
                       this.loggingCollName,
                       "findAndModify",
                       op);
        }

        function findCollScan(db, collName) {
            const op = function(ddlColl) {
                ddlColl.findOne();
            };
            runOpInTxn(this.session,
                       db,
                       collName,
                       this.ddlDBName,
                       this.ddlCollName,
                       this.loggingCollName,
                       "findCollScan",
                       op);
        }

        function findGetMore(db, collName) {
            const op = function(ddlColl) {
                ddlColl.find().batchSize(1).itcount();
            };
            runOpInTxn(this.session,
                       db,
                       collName,
                       this.ddlDBName,
                       this.ddlCollName,
                       this.loggingCollName,
                       "findGetMore",
                       op);
        }

        function findIdScan(db, collName) {
            const op = function(ddlColl) {
                ddlColl.findOne({_id: 0});
            };
            runOpInTxn(this.session,
                       db,
                       collName,
                       this.ddlDBName,
                       this.ddlCollName,
                       this.loggingCollName,
                       "findIdScan",
                       op);
        }

        function findSecondaryIndexScan(db, collName) {
            const op = function(ddlColl) {
                ddlColl.findOne({x: 1});
            };
            runOpInTxn(this.session,
                       db,
                       collName,
                       this.ddlDBName,
                       this.ddlCollName,
                       this.loggingCollName,
                       "findSecondaryIndexScan",
                       op);
        }

        function insert(db, collName) {
            const op = function(ddlColl) {
                let res = ddlColl.insert({x: 1});
                if (res instanceof WriteResult && res.hasWriteError()) {
                    throw _getErrorWithCode(res.getWriteError(), res.getWriteError().errmsg);
                } else if (!res.ok) {
                    assertWhenOwnColl.commandWorked(res);
                }
            };
            runOpInTxn(this.session,
                       db,
                       collName,
                       this.ddlDBName,
                       this.ddlCollName,
                       this.loggingCollName,
                       "insert",
                       op);
        }

        function remove(db, collName) {
            const op = function(ddlColl) {
                assertWhenOwnColl.commandWorked(ddlColl.remove({}, {justOne: true}));
            };
            runOpInTxn(this.session,
                       db,
                       collName,
                       this.ddlDBName,
                       this.ddlCollName,
                       this.loggingCollName,
                       "remove",
                       op);
        }

        function update(db, collName) {
            const op = function(ddlColl) {
                assertWhenOwnColl.commandWorked(ddlColl.update({}, {$inc: {x: 1}}));
            };
            runOpInTxn(this.session,
                       db,
                       collName,
                       this.ddlDBName,
                       this.ddlCollName,
                       this.loggingCollName,
                       "update",
                       op);
        }

        /**
         * The following functions will be run by threads performing DDL operations.
         */

        function createColl(db, collName) {
            // Insert a document to ensure the collection exists and provide data that can be
            // accessed in the transaction states.
            assertWhenOwnColl.commandWorked(
                db.getSiblingDB(this.ddlDBName)[this.ddlCollName].insert({x: 1}));
        }

        function createIndex(db, collName) {
            assertWhenOwnColl.commandWorked(
                db.getSiblingDB(this.ddlDBName)[this.ddlCollName].createIndex({x: 1}));
        }

        function dropColl(db, collName) {
            assertWhenOwnColl.commandWorkedOrFailedWithCode(
                db.getSiblingDB(this.ddlDBName).runCommand({drop: this.ddlCollName}),
                ErrorCodes.NamespaceNotFound);
        }

        function renameColl(db, collName) {
            const ddlCollFullName = db.getSiblingDB(this.ddlDBName)[this.ddlCollName].getFullName();
            const renameCollFullName =
                db.getSiblingDB(this.ddlDBName)[this.renameCollName].getFullName();
            assertWhenOwnColl.commandWorkedOrFailedWithCode(
                db.adminCommand(
                    {renameCollection: ddlCollFullName, to: renameCollFullName, dropTarget: true}),
                ErrorCodes.NamespaceNotFound);
        }

        return {
            init: init,
            aggregate: aggregate,
            distinct: distinct,
            findAndModify: findAndModify,
            findCollScan: findCollScan,
            findGetMore: findGetMore,
            findIdScan: findIdScan,
            findSecondaryIndexScan: findSecondaryIndexScan,
            insert: insert,
            remove: remove,
            update: update,
            createColl: createColl,
            createIndex: createIndex,
            dropColl: dropColl,
            renameColl: renameColl
        };
    })();

    function setup(db, collName, cluster) {
        assertWhenOwnColl.commandWorked(db[collName].insert({_id: "startTxnDoc"}));
        assertWhenOwnColl.commandWorked(db.runCommand({create: this.loggingCollName}));
    }

    function teardown(db, collName, cluster) {
        // Report the number of successful and failed transaction operations of each type. This test
        // does not provide value if all transaction operations fail with LockTimeout, since then we
        // are not accessing the DDL collection.
        let res =
            db[this.loggingCollName]
                .aggregate([{$match: {success: true}}, {$group: {_id: "$op", count: {$sum: 1}}}])
                .toArray();
        jsTestLog("Successful transaction operations: " + tojson(res));

        res = db[this.loggingCollName]
                  .aggregate([{$match: {success: false}}, {$group: {_id: "$op", count: {$sum: 1}}}])
                  .toArray();
        jsTestLog("Failed transaction operations: " + tojson(res));
    }

    var randomTxnState = {
        aggregate: 0.1,
        distinct: 0.1,
        findAndModify: 0.1,
        findCollScan: 0.1,
        findGetMore: 0.1,
        findIdScan: 0.1,
        findSecondaryIndexScan: 0.1,
        insert: 0.1,
        remove: 0.1,
        update: 0.1
    };

    var randomDDLState = {createColl: 0.4, createIndex: 0.2, dropColl: 0.2, renameColl: 0.2};

    var transitions = {
        // 80% of threads perform transaction operations, and 20% perform DDL operations.
        init: {aggregate: 0.8, createColl: 0.2},

        // Transaction states.
        aggregate: randomTxnState,
        distinct: randomTxnState,
        findAndModify: randomTxnState,
        findCollScan: randomTxnState,
        findGetMore: randomTxnState,
        findIdScan: randomTxnState,
        findSecondaryIndexScan: randomTxnState,
        insert: randomTxnState,
        remove: randomTxnState,
        update: randomTxnState,

        // DDL states.
        createColl: randomDDLState,
        createIndex: randomDDLState,
        dropColl: randomDDLState,
        renameColl: randomDDLState
    };

    return {
        threadCount: 10,
        iterations: 100,
        startState: 'init',
        states: states,
        transitions: transitions,
        data: {
            ddlCollName: "ddl_coll",
            ddlDBName: "access_collection_in_transaction_after_catalog_changes_ddl_db",
            loggingCollName: "access_collection_in_transaction_after_catalog_changes_logging",
            renameCollName: "rename_coll"
        },
        setup: setup,
        teardown: teardown
    };

})();

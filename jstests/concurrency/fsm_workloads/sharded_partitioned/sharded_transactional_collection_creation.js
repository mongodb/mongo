/**
 * Tests collection creation inside a multi-document transaction.
 *
 * @tags: [
 *  # Tries to create collections in prepared transactions
 *  featureFlagCreateCollectionInPreparedTransactions,
 *  requires_sharding,
 *  uses_transactions,
 * ]
 */

import {
    withTxnAndAutoRetry
} from "jstests/concurrency/fsm_workload_helpers/auto_retry_transaction.js";

// TODO(SERVER-46971) Remove `local` readConcern.
TestData.defaultTransactionReadConcernLevel = "local";

export const $config = (function() {
    const data = {
        createdDbName: jsTest.name(),
        createdCollName: jsTest.name(),
        // Used by the FSM worker to shard the collection passed to the functions.
        shardKey: {_id: 'hashed'},
    };

    var states = (function() {
        function init(db, collName) {
            this.seqNum = 0;
            this.session = db.getMongo().startSession();
        }

        function createCollectionImplicitlyViaInsert(db, collName) {
            withTxnAndAutoRetry(this.session, () => {
                // Write to multiple shards to force a distributed transaction.
                assert.commandWorked(this.session.getDatabase(db.getName())
                                         .getCollection(collName)
                                         .insert([{a: 1}, {a: 2}, {a: 3}]));
                assert.commandWorked(this.session.getDatabase(this.createdDbName)
                                         .getCollection(this.createdCollName)
                                         .insertOne({_id: this.seqNum}));
            }, {});
        }

        function createCollectionExplicitlyViaCreate(db, collName) {
            withTxnAndAutoRetry(this.session, () => {
                // Write to multiple shards to force a distributed transaction.
                assert.commandWorked(this.session.getDatabase(db.getName())
                                         .getCollection(collName)
                                         .insert([{a: 1}, {a: 2}, {a: 3}]));
                assert.commandWorked(this.session.getDatabase(this.createdDbName)
                                         .createCollection(this.createdCollName));
            }, {});
        }

        function createCollectionImplicitlyViaCreateIndexes(db, collName) {
            withTxnAndAutoRetry(this.session, () => {
                // Write to multiple shards to force a distributed transaction.
                assert.commandWorked(this.session.getDatabase(db.getName())
                                         .getCollection(collName)
                                         .insert([{a: 1}, {a: 2}, {a: 3}]));
                assert.commandWorked(this.session.getDatabase(this.createdDbName)
                                         .getCollection(this.createdCollName)
                                         .createIndex({a: 1}));
            }, {});
        }

        function createCollectionOutsideTxn(db, collName) {
            assert.commandWorked(
                db.getSiblingDB(this.createdDbName).createCollection(this.createdCollName));
            this.seqNum++;
        }

        function dropCollection(db, collName) {
            assert(db.getSiblingDB(this.createdDbName).getCollection(this.createdCollName).drop());
            this.seqNum++;
        }

        function dropDatabase(db, collName) {
            assert.commandWorked(db.getSiblingDB(this.createdDbName).dropDatabase());
            this.seqNum++;
        }

        return {
            init: init,
            createCollectionOutsideTxn: createCollectionOutsideTxn,
            createCollectionImplicitlyViaInsert: createCollectionImplicitlyViaInsert,
            createCollectionExplicitlyViaCreate: createCollectionExplicitlyViaCreate,
            createCollectionImplicitlyViaCreateIndexes: createCollectionImplicitlyViaCreateIndexes,
            dropCollection: dropCollection,
            dropDatabase: dropDatabase,
        };
    })();

    var transitions = {
        init: {
            createCollectionOutsideTxn: 1.0,
            createCollectionExplicitlyViaCreate: 1.0,
            createCollectionImplicitlyViaCreateIndexes: 1.0,
            createCollectionImplicitlyViaInsert: 1.0,
        },
        createCollectionOutsideTxn: {dropCollection: 1.0, dropDatabase: 1.0},
        createCollectionExplicitlyViaCreate: {dropCollection: 1.0, dropDatabase: 1.0},
        createCollectionImplicitlyViaCreateIndexes: {dropCollection: 1.0, dropDatabase: 1.0},
        createCollectionImplicitlyViaInsert: {dropCollection: 1.0, dropDatabase: 1.0},
        dropCollection: {
            createCollectionOutsideTxn: 1.0,
            createCollectionExplicitlyViaCreate: 1.0,
            createCollectionImplicitlyViaCreateIndexes: 1.0,
            createCollectionImplicitlyViaInsert: 1.0,
        },
        dropDatabase: {
            createCollectionOutsideTxn: 1.0,
            createCollectionExplicitlyViaCreate: 1.0,
            createCollectionImplicitlyViaCreateIndexes: 1.0,
            createCollectionImplicitlyViaInsert: 1.0,
        },
    };

    // TODO SERVER-83328: Increase thread count to test concurrent collection DDL operations within
    // multi-document transactions.
    return {threadCount: 1, iterations: 50, data: data, states: states, transitions: transitions};
})();

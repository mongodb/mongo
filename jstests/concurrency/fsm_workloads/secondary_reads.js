'use strict';

/**
 * secondary_reads.js
 *
 * One thread (tid 0) is dedicated to writing documents with field 'x' in ascending order into the
 * collection. This thread is also responsible for ensuring the required index on 'x' exists.
 *
 * Other threads do one of the following operations each iteration.
 * 1) Retrieve first 50 documents in descending order with local readConcern from a secondary node.
 * 2) Retrieve first 50 documents in descending order with available readConcern from a secondary
 * node.
 * 3) Retrieve first 50 documents in descending order with majority readConcern from a secondary
 * node.
 *
 * For each read, we perform a reverse index scan on 'x' and check if there are any 'holes' in the
 * returned batch. There should not be any 'holes' because despite the secondary applying operations
 * out of order, the scan on the ordered field 'x' guarantees we will see all inserts in-order.
 *
 * @tags: [requires_replication, uses_write_concern]
 */

var $config = (function() {
    // Use the workload name as the collection name.
    var uniqueCollectionName = 'secondary_reads';

    load('jstests/concurrency/fsm_workload_helpers/server_types.js');

    function isWriterThread() {
        return this.tid === 0;
    }

    function buildIndex(db, spec) {
        assertAlways.commandWorked(db[this.collName].createIndex(spec));
    }

    function assertSecondaryReadOk(res) {
        assertAlways.commandWorked(res);
    }

    function insertDocumentsAndBuildIndex(db, writeConcern) {
        // This index is required to ensure secondary reads do not see "holes" between documents.
        // Because documents are applied out-of-order on the secondary, we cannot perform a natural
        // collection scan and expect to see all documents in-order, specifically if we read at a
        // timestamp that falls in the middle of an already-completed batch. The property of not
        // seeing "holes" between documents can only be guaranteed by scanning on an ordered index.
        this.buildIndex(db, {x: 1});

        let bulk = db[this.collName].initializeOrderedBulkOp();
        for (let i = this.nDocumentsInTotal; i < this.nDocumentsInTotal + this.nDocumentsToInsert;
             i++) {
            bulk.insert({_id: i, x: i});
        }
        let res = bulk.execute(writeConcern);
        assertWhenOwnColl.commandWorked(res);
        assertWhenOwnColl.eq(this.nDocumentsToInsert, res.nInserted);
        this.nDocumentsInTotal += this.nDocumentsToInsert;
    }

    function readFromSecondaries(db, readConcernLevel) {
        let arr = [];
        assert.soon(() => {
            try {
                arr = db[this.collName]
                          .find()
                          .readPref('secondary')
                          .readConcern(readConcernLevel)
                          .sort({x: -1})
                          .hint({x: 1})
                          .limit(this.nDocumentsToCheck)
                          .toArray();
                return true;
            } catch (e) {
                // We propagate TransientTransactionErrors to allow the state function to
                // automatically be retried when TestData.runInsideTransaction=true
                if (e.hasOwnProperty('errorLabels') &&
                    e.errorLabels.includes('TransientTransactionError')) {
                    throw e;
                }
                this.assertSecondaryReadOk(e);
                print("retrying failed secondary read operation: " + tojson(e));
                return false;
            }
        });
        // Make sure there is no hole in the result.
        for (let i = 0; i < arr.length - 1; i++) {
            assertWhenOwnColl.eq(arr[i].x, arr[i + 1].x + 1, () => tojson(arr));
        }
    }

    function getReadConcernLevel(supportsCommittedReads) {
        const readConcernLevels = ['local'];
        if (!TestData.runningWithCausalConsistency && !TestData.runInsideTransaction) {
            readConcernLevels.push('available');
        }
        if (supportsCommittedReads) {
            readConcernLevels.push('majority');
        }
        return readConcernLevels[Random.randInt(readConcernLevels.length)];
    }

    var states = (function() {
        // One thread is dedicated to writing and other threads perform reads on
        // secondaries with a randomly chosen readConcern level.
        function readFromSecondaries(db, collName) {
            if (this.isWriterThread()) {
                this.insertDocumentsAndBuildIndex(db);
            } else {
                this.readFromSecondaries(db, getReadConcernLevel(supportsCommittedReads(db)));
            }
        }

        return {readFromSecondaries: readFromSecondaries};
    })();

    var transitions = {readFromSecondaries: {readFromSecondaries: 1}};

    var setup = function setup(db, collName, cluster) {
        this.nDocumentsInTotal = 0;
        // Start write workloads to activate oplog application on secondaries
        // before any reads.
        this.insertDocumentsAndBuildIndex(db, {w: cluster.getReplSetNumNodes()});
    };

    return {
        threadCount: 30,
        iterations: 10,
        startState: 'readFromSecondaries',
        states: states,
        data: {
            nDocumentsToInsert: 2000,
            nDocumentsToCheck: 50,
            isWriterThread: isWriterThread,
            insertDocumentsAndBuildIndex: insertDocumentsAndBuildIndex,
            buildIndex: buildIndex,
            readFromSecondaries: readFromSecondaries,
            assertSecondaryReadOk: assertSecondaryReadOk,
            collName: uniqueCollectionName
        },
        transitions: transitions,
        setup: setup,
    };
})();

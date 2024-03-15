/**
 * auto_compact.js
 *
 * Bulk inserts 1000 documents and builds indexes. Then alternates between compacting the
 * collection and verifying the number of documents and indexes. Operates on a separate collection
 * for each thread.
 *
 * @tags: [
 *  assumes_standalone_mongod,
 *  assumes_unsharded_collection,
 *  # The config fuzzer may try to stress wiredtiger which can cause this test to timeout.
 *  does_not_support_config_fuzzer,
 *  # The compact test requires large enough collections for the compact operation to do work. This
 *  # can cause too much cache pressure for some concurrency tests with transactions.
 *  does_not_support_transactions,
 *  featureFlagAutoCompact,
 *  incompatible_with_macos,
 *  # Requires all nodes to be running the latest binary.
 *  multiversion_incompatible,
 *  requires_compact,
 *  # Compact is not supported for in memory databases.
 *  requires_persistence
 * ]
 */

import {
    assertWorkedHandleTxnErrors
} from "jstests/concurrency/fsm_workload_helpers/assert_handle_fail_in_transaction.js";

// WiredTiger eviction is slow on Windows debug variants and can cause timeouts when taking a
// checkpoint through compaction.
const buildInfo = getBuildInfo();
const skipTest = buildInfo.debug && buildInfo.buildEnvironment.target_os == "windows";

export const $config = (function() {
    var data = {
        targetDocuments: 1000,
        nDocs: 0,
        nIndexes: 1 + 1,    // The number of indexes created in createIndexes + 1 for { _id: 1 }
        prefix: 'compact',  // Use filename for prefix because filename is assumed unique
        autoCompactRunning: false
    };

    var states = (function() {
        function insertDocuments(db, collName) {
            var nDocumentsToInsert =
                this.targetDocuments - db[this.threadCollName].find().itcount();
            var bulk = db[this.threadCollName].initializeUnorderedBulkOp();
            for (var i = 0; i < nDocumentsToInsert; ++i) {
                bulk.insert({a: Random.randInt(2), b: 'b'.repeat(100000), c: 'c'.repeat(100000)});
            }
            var res = bulk.execute();
            this.nDocs += res.nInserted;
            assert.commandWorked(res);
            assert.eq(nDocumentsToInsert, res.nInserted);
            assert.lte(db[this.threadCollName].find().itcount(), this.targetDocuments);
        }

        function removeDocuments(db, collName) {
            // Remove around one third of the documents in the collection.
            var res = db[this.threadCollName].deleteMany({a: Random.randInt(2)});
            this.nDocs -= res.deletedCount;
            assert.commandWorked(res);
        }

        function createIndexes(db, collName) {
            // The number of indexes created here is also stored in data.nIndexes
            var aResult = db[collName].createIndex({a: 1});

            assertWorkedHandleTxnErrors(aResult, ErrorCodes.IndexBuildAlreadyInProgress);
        }

        // This method is independent of collectionSetup to allow it to be overridden in
        // workloads that extend this one
        function init(db, collName) {
            this.threadCollName = this.prefix + '_' + this.tid;
        }

        function collectionSetup(db, collName) {
            insertDocuments.call(this, db, this.threadCollName);
            createIndexes.call(this, db, this.threadCollName);
        }

        function enableAutoCompact(db, collName) {
            if (!this.autoCompactRunning) {
                let res;
                let retries = 0;
                const maxRetries = 10;
                while ((res = assert.commandWorkedOrFailedWithCode(
                            db.adminCommand({autoCompact: true, freeSpaceTargetMB: 1}),
                            ErrorCodes.ObjectIsBusy))
                               .code == ErrorCodes.ObjectIsBusy &&
                       retries < maxRetries) {
                    retries++;
                    sleep(1);
                }
            }
            this.autoCompactRunning = true;
        }

        function disableAutoCompact(db, collName) {
            if (this.autoCompactRunning) {
                let res;
                let retries = 0;
                const maxRetries = 10;
                while ((res = assert.commandWorkedOrFailedWithCode(
                            db.adminCommand({autoCompact: false}), ErrorCodes.ObjectIsBusy))
                               .code == ErrorCodes.ObjectIsBusy &&
                       retries < maxRetries) {
                    retries++;
                    sleep(1);
                }
            }

            this.autoCompactRunning = false;
        }

        function query(db, collName) {
            var count = db[this.threadCollName].find().itcount();
            assert.eq(count,
                      this.nDocs,
                      'number of documents in ' +
                          'collection should not change following a compact');
            var indexesCount = db[this.threadCollName].getIndexes().length;
            assert.eq(indexesCount, this.nIndexes);
        }

        return {
            init: init,
            collectionSetup: collectionSetup,
            enableAutoCompact: enableAutoCompact,
            disableAutoCompact: disableAutoCompact,
            query: query,
            removeDocuments: removeDocuments,
            insertDocuments: insertDocuments
        };
    })();

    function teardown(db, collName, cluster) {
        while ((assert.commandWorkedOrFailedWithCode(db.adminCommand({autoCompact: false}),
                                                     ErrorCodes.ObjectIsBusy))
                   .code == ErrorCodes.ObjectIsBusy) {
            sleep(1);
        }
    }

    var transitions = {
        init: {collectionSetup: 1},
        collectionSetup: {removeDocuments: 1},
        removeDocuments: {enableAutoCompact: 0.1, disableAutoCompact: 0.1, insertDocuments: 0.8},
        disableAutoCompact: {query: 1},
        enableAutoCompact: {query: 1},
        query: {insertDocuments: 1},
        insertDocuments: {removeDocuments: 1}
    };

    return {
        threadCount: 1,
        iterations: skipTest ? 0 : 100,
        states: states,
        transitions: transitions,
        data: data,
        teardown: teardown
    };
})();

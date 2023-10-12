/**
 * compact.js
 *
 * Bulk inserts 1000 documents and builds indexes. Then alternates between compacting the
 * collection and verifying the number of documents and indexes. Operates on a separate collection
 * for each thread.
 *
 * There is a known hang during concurrent FSM workloads with the compact command used
 * with wiredTiger LSM variants. Bypass this command for the wiredTiger LSM variant
 * until a fix is available for WT-2523.
 *
 * @tags: [does_not_support_wiredtiger_lsm, incompatible_with_macos, requires_compact]
 */

import {
    assertWorkedHandleTxnErrors
} from "jstests/concurrency/fsm_workload_helpers/assert_handle_fail_in_transaction.js";
import {isEphemeral} from "jstests/concurrency/fsm_workload_helpers/server_types.js";

// WiredTiger eviction is slow on Windows debug variants and can cause timeouts when taking a
// checkpoint through compaction.
const buildInfo = getBuildInfo();
const skipTest = buildInfo.debug && buildInfo.buildEnvironment.target_os == "windows";

export const $config = (function() {
    var data = {
        nDocumentsToInsert: 1000,
        nIndexes: 3 + 1,   // The number of indexes created in createIndexes + 1 for { _id: 1 }
        prefix: 'compact'  // Use filename for prefix because filename is assumed unique
    };

    var states = (function() {
        function insertDocuments(db, collName) {
            var bulk = db[collName].initializeUnorderedBulkOp();
            for (var i = 0; i < this.nDocumentsToInsert; ++i) {
                bulk.insert({a: Random.randInt(10), b: Random.randInt(10), c: Random.randInt(10)});
            }
            var res = bulk.execute();
            assert.commandWorked(res);
            assert.eq(this.nDocumentsToInsert, res.nInserted);
        }

        function createIndexes(db, collName) {
            // The number of indexes created here is also stored in data.nIndexes
            var aResult = db[collName].createIndex({a: 1});

            assertWorkedHandleTxnErrors(aResult, ErrorCodes.IndexBuildAlreadyInProgress);

            var bResult = db[collName].createIndex({b: 1});

            assertWorkedHandleTxnErrors(bResult, ErrorCodes.IndexBuildAlreadyInProgress);

            var cResult = db[collName].createIndex({c: 1});

            assertWorkedHandleTxnErrors(cResult, ErrorCodes.IndexBuildAlreadyInProgress);
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

        function compact(db, collName) {
            var res = db.runCommand({compact: this.threadCollName, force: true});
            if (!isEphemeral(db)) {
                assert.commandWorked(res);
            } else {
                assert.commandFailedWithCode(res, ErrorCodes.CommandNotSupported);
            }
        }

        function query(db, collName) {
            var count = db[this.threadCollName].find().itcount();
            assert.eq(count,
                      this.nDocumentsToInsert,
                      'number of documents in ' +
                          'collection should not change following a compact');
            var indexesCount = db[this.threadCollName].getIndexes().length;
            assert.eq(indexesCount, this.nIndexes);
        }

        return {init: init, collectionSetup: collectionSetup, compact: compact, query: query};
    })();

    var transitions = {
        init: {collectionSetup: 1},
        collectionSetup: {compact: 0.5, query: 0.5},
        compact: {compact: 0.5, query: 0.5},
        query: {compact: 0.5, query: 0.5}
    };

    return {
        threadCount: 3,
        iterations: skipTest ? 0 : 8,
        states: states,
        transitions: transitions,
        data: data,
    };
})();

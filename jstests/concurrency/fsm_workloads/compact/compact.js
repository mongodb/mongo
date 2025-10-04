/**
 * compact.js
 *
 * Bulk inserts 1000 documents and builds indexes. Then alternates between compacting the
 * collection and verifying the number of documents and indexes. Operates on a separate collection
 * for each thread.
 *
 * @tags: [
 *  incompatible_with_macos,
 *  requires_compact,
 *  # The config fuzzer may try to stress wiredtiger which can cause this test to timeout.
 *  does_not_support_config_fuzzer,
 *  # The compact test requires large enough collections for the compact operation to do work. This
 *  # can cause too much cache pressure for some concurrency tests with transactions.
 *  does_not_support_transactions,
 *  # Compact is not supported for in memory databases.
 *  requires_persistence,
 *  assumes_against_mongod_not_mongos,
 * ]
 */

import {assertWorkedHandleTxnErrors} from "jstests/concurrency/fsm_workload_helpers/assert_handle_fail_in_transaction.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";

// WiredTiger eviction is slow on Windows debug variants and can cause timeouts when taking a
// checkpoint through compaction.
const buildInfo = getBuildInfo();
const skipTest = buildInfo.debug && buildInfo.buildEnvironment.target_os == "windows";

export const $config = (function () {
    let data = {
        targetDocuments: 1000,
        nDocs: 0,
        nIndexes: 1 + 1, // The number of indexes created in createIndexes + 1 for { _id: 1 }
        prefix: "compact", // Use filename for prefix because filename is assumed unique
    };

    function runCompact(db, config) {
        let res = db.runCommand(config);

        // The compact command can be successful or interrupted because of cache pressure or
        // concurrent calls to compact.
        assert.commandWorkedOrFailedWithCode(res, ErrorCodes.Interrupted, tojson(res));
    }

    let states = (function () {
        function insertDocuments(db, collName) {
            let nDocumentsToInsert = this.targetDocuments - db[this.threadCollName].find().itcount();
            let bulk = db[this.threadCollName].initializeUnorderedBulkOp();
            for (let i = 0; i < nDocumentsToInsert; ++i) {
                bulk.insert({a: Random.randInt(2), b: "b".repeat(100000), c: "c".repeat(100000)});
            }
            let res = bulk.execute();
            this.nDocs += res.nInserted;
            assert.commandWorked(res);
            assert.eq(nDocumentsToInsert, res.nInserted);
            assert.lte(db[this.threadCollName].find().itcount(), this.targetDocuments);
        }

        function removeDocuments(db, collName) {
            // Remove around one third of the documents in the collection.
            let res = db[this.threadCollName].deleteMany({a: Random.randInt(2)});
            this.nDocs -= res.deletedCount;
            assert.commandWorked(res);
        }

        function createIndexes(db, collName) {
            // The number of indexes created here is also stored in data.nIndexes
            let aResult = db[collName].createIndex({a: 1});

            assertWorkedHandleTxnErrors(aResult, ErrorCodes.IndexBuildAlreadyInProgress);
        }

        // This method is independent of collectionSetup to allow it to be overridden in
        // workloads that extend this one
        function init(db, collName) {
            this.threadCollName = this.prefix + "_" + this.tid;
        }

        function collectionSetup(db, collName) {
            insertDocuments.call(this, db, this.threadCollName);
            createIndexes.call(this, db, this.threadCollName);
        }

        function compact(db, collName) {
            let config;
            if (FeatureFlagUtil.isPresentAndEnabled(db.getMongo(), "CompactOptions")) {
                config = {compact: this.threadCollName, force: true, freeSpaceTargetMB: 1};
            } else {
                config = {compact: this.threadCollName, force: true};
            }
            runCompact(db, config);
        }

        function dryCompact(db, collName) {
            if (!FeatureFlagUtil.isPresentAndEnabled(db.getMongo(), "CompactOptions")) return;
            const config = {compact: this.threadCollName, force: true, dryRun: true, freeSpaceTargetMB: 1};
            runCompact(db, config);
        }

        function query(db, collName) {
            let count = db[this.threadCollName].find().itcount();
            assert.eq(
                count,
                this.nDocs,
                "number of documents in " + "collection should not change following a compact",
            );
            let indexesCount = db[this.threadCollName].getIndexes().length;
            assert.eq(indexesCount, this.nIndexes);
        }

        return {
            init: init,
            collectionSetup: collectionSetup,
            dryCompact: dryCompact,
            compact: compact,
            query: query,
            removeDocuments: removeDocuments,
            insertDocuments: insertDocuments,
        };
    })();

    let transitions = {
        init: {collectionSetup: 1},
        collectionSetup: {removeDocuments: 1},
        removeDocuments: {dryCompact: 1},
        dryCompact: {compact: 1},
        compact: {query: 1},
        query: {insertDocuments: 1},
        insertDocuments: {removeDocuments: 1},
    };

    return {
        threadCount: 3,
        iterations: skipTest ? 0 : 12,
        states: states,
        transitions: transitions,
        data: data,
    };
})();

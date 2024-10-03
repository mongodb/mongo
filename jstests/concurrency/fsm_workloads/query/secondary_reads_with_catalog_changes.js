/**
 * secondary_reads_with_catalog_changes.js
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
 * 4) Drop the index on 'x'.
 * 5) Drop the collection.
 *
 * Note that index/collection drop could interrupt the reads, so we need to retry if the read is
 * interrupted.
 *
 * @tags: [
 *   creates_background_indexes,
 *   requires_replication,
 *   uses_write_concern,
 * ]
 */

import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {$config as $baseConfig} from "jstests/concurrency/fsm_workloads/query/secondary_reads.js";

export const $config = extendWorkload($baseConfig, function($config, $super) {
    $config.data.buildIndex = function buildIndex(db, spec) {
        // Index must be built eventually.
        assert.soon(() => {
            const res = db[this.collName].createIndex(spec);
            if (res.ok === 1) {
                assert.commandWorked(res);
                return true;
            }
            if (TestData.runInsideTransaction) {
                assert.commandFailedWithCode(res, [
                    ErrorCodes.IndexBuildAborted,
                    ErrorCodes.IndexBuildAlreadyInProgress,
                    ErrorCodes.NoMatchingDocument,
                ]);
            } else {
                assert.commandFailedWithCode(res, [
                    ErrorCodes.IndexBuildAborted,
                    ErrorCodes.NoMatchingDocument,
                ]);
            }
            print("retrying failed createIndex operation: " + tojson(res));
            return false;
        });
    };

    $config.data.assertSecondaryReadOk = function(res) {
        assert.commandFailedWithCode(
            res,
            [
                // The query was interrupted due to an index or collection drop
                ErrorCodes.QueryPlanKilled,
                // The required, hinted index does not exist
                ErrorCodes.BadValue,
                // The collection was dropped
                ErrorCodes.NamespaceNotFound,
                // Index not found by cached SBE plan during concurrent index drop
                ErrorCodes.IndexNotFoundCachedPlan,
            ],
            'unexpected error code: ' + res.code + ': ' + res.message);
    };

    $config.states.dropIndex = function dropIndex(db, collName) {
        if (this.isWriterThread(this.tid)) {
            this.insertDocumentsAndBuildIndex(db);
        } else {
            const res = db[this.collName].dropIndex({x: 1});
            if (res.ok === 1) {
                assert.commandWorked(res);
                // Always rebuild the index because reader threads will retry until the index
                // exists.
                this.buildIndex(db, {x: 1});
            } else {
                assert.commandFailedWithCode(res, [
                    ErrorCodes.IndexNotFound,
                    ErrorCodes.NamespaceNotFound,
                    ErrorCodes.BackgroundOperationInProgressForNamespace
                ]);
            }
        }
    };

    $config.states.dropCollection = function dropCollection(db, collName) {
        if (this.isWriterThread(this.tid)) {
            this.insertDocumentsAndBuildIndex(db);
        } else {
            const res = db.runCommand({drop: this.collName});
            if (res.ok === 1) {
                assert.commandWorked(res);
                // Always rebuild the index because reader threads will retry until the index
                // exists.
                this.buildIndex(db, {x: 1});
            } else {
                assert.commandFailedWithCode(res, [
                    ErrorCodes.NamespaceNotFound,
                    ErrorCodes.BackgroundOperationInProgressForNamespace
                ]);
            }
            this.nDocumentsInTotal = 0;
        }
    };

    $config.transitions = {
        readFromSecondaries: {readFromSecondaries: 0.9, dropIndex: 0.05, dropCollection: 0.05},
        dropIndex: {readFromSecondaries: 1},
        dropCollection: {readFromSecondaries: 1}
    };

    return $config;
});

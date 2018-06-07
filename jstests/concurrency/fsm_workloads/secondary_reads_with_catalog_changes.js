'use strict';

load('jstests/concurrency/fsm_libs/extend_workload.js');       // for extendWorkload
load('jstests/concurrency/fsm_workloads/secondary_reads.js');  // for $config

/**
 * secondary_reads_with_catalog_changes.js
 *
 * One thread (tid 0) is dedicated to writing documents with field 'x' in
 * ascending order into the collection.
 *
 * Other threads do one of the following operations each iteration.
 * 1) Retrieve first 50 documents in descending order with local readConcern from a secondary node.
 * 2) Retrieve first 50 documents in descending order with available readConcern from a secondary
 * node.
 * 3) Retrieve first 50 documents in descending order with majority readConcern from a secondary
 * node.
 * 4) Build indexes on field x.
 * 5) Drop indexes on field x.
 * 6) Drop collection.
 *
 * Note that index/collection drop could interrupt the reads, so we need to retry if the read is
 * interrupted.
 *
 * @tags: [requires_replication]
 */
var $config = extendWorkload($config, function($config, $super) {

    $config.states.buildIndex = function buildIndex(db, collName) {
        if (this.isWriterThread(this.tid)) {
            this.insertDocuments(db, this.collName);
        } else {
            assertWhenOwnColl.commandWorked(db[this.collName].createIndex(
                {x: 1}, {unique: true, background: Random.rand() < 0.5}));
        }
    };

    $config.states.dropIndex = function dropIndex(db, collName) {
        if (this.isWriterThread(this.tid)) {
            this.insertDocuments(db, this.collName);
        } else {
            const res = db[this.collName].dropIndex({x: 1});
            if (res.ok === 1) {
                assertWhenOwnColl.commandWorked(res);
            } else {
                assertWhenOwnColl.commandFailedWithCode(res, [
                    ErrorCodes.IndexNotFound,
                    ErrorCodes.NamespaceNotFound,
                    ErrorCodes.BackgroundOperationInProgressForNamespace
                ]);
            }
        }
    };

    $config.states.dropCollection = function dropCollection(db, collName) {
        if (this.isWriterThread(this.tid)) {
            this.insertDocuments(db, this.collName);
        } else {
            const res = db.runCommand({drop: this.collName});
            if (res.ok === 1) {
                assertWhenOwnColl.commandWorked(res);
            } else {
                assertWhenOwnColl.commandFailedWithCode(res, [
                    ErrorCodes.NamespaceNotFound,
                    ErrorCodes.BackgroundOperationInProgressForNamespace
                ]);
            }
            this.nDocumentsInTotal = 0;
        }
    };

    $config.transitions = {
        readFromSecondaries:
            {readFromSecondaries: 0.9, buildIndex: 0.05, dropIndex: 0.03, dropCollection: 0.02},
        buildIndex: {readFromSecondaries: 1},
        dropIndex: {readFromSecondaries: 1},
        dropCollection: {readFromSecondaries: 1}
    };

    return $config;
});

'use strict';

/**
 * WiredTiger allows online compaction of its collections so it does not require an exclusive lock.
 * This workload is meant to test the behavior of the locking changes done in SERVER-16413. To
 * run the 'compact' command while simultaneously inserting documents and creating indexes on the
 * collection being compacted.
 *
 * @tags: [requires_compact, does_not_support_wiredtiger_lsm]
 */

load('jstests/concurrency/fsm_workload_helpers/server_types.js');  // for isEphemeral

var $config = (function() {
    var states = (function() {
        function init(db, collName) {
            insertDocuments.call(this, db, collName);
        }

        function insertDocuments(db, collName) {
            const nDocumentsToInsert = 100;
            var bulk = db[collName].initializeUnorderedBulkOp();
            for (var i = 0; i < nDocumentsToInsert; ++i) {
                bulk.insert({x: i});
            }
            var res = bulk.execute();
            assertAlways.commandWorked(res);
            assertAlways.eq(nDocumentsToInsert, res.nInserted);
        }

        function compact(db, collName) {
            let res = db.runCommand({compact: collName, force: true});
            if (!isEphemeral(db)) {
                assertAlways.commandWorked(res);
            } else {
                assertAlways.commandFailedWithCode(res, ErrorCodes.CommandNotSupported);
            }
        }

        function createIndex(db, collName) {
            db[collName].createIndex({x: 1}, {background: true});
        }

        function dropIndex(db, collName) {
            db[collName].dropIndex({x: 1});
        }

        function validate(db, collName) {
            let res = assert.commandWorked(db.getCollection(collName).validate());
            assert.eq(true, res.valid);
        }

        return {
            init: init,
            insertDocuments: insertDocuments,
            compact: compact,
            createIndex: createIndex,
            dropIndex: dropIndex,
            validate: validate
        };
    })();

    var transitions = {
        init: {compact: 0.5, createIndex: 0.5},
        insertDocuments: {compact: 0.3, createIndex: 0.3, validate: 0.2, dropIndex: 0.2},
        compact: {insertDocuments: 0.3, createIndex: 0.3, validate: 0.2, dropIndex: 0.2},
        createIndex: {compact: 0.3, insertDocuments: 0.3, validate: 0.2, dropIndex: 0.2},
        dropIndex: {compact: 0.2, createIndex: 0.7, validate: 0.1},
        validate: {dropIndex: 0.1, compact: 0.3, createIndex: 0.3, insertDocuments: 0.3}
    };

    return {
        threadCount: 10,
        iterations: 10,
        states: states,
        transitions: transitions,
    };
})();

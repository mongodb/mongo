'use strict';

/**
 * compact.js
 *
 * Bulk inserts 1000 documents and builds indexes. Then alternates between compacting the
 * collection and verifying the number of documents and indexes. Operates on a separate collection
 * for each thread.
 */

load('jstests/concurrency/fsm_workload_helpers/drop_utils.js');    // for dropCollections
load('jstests/concurrency/fsm_workload_helpers/server_types.js');  // for isEphemeral

var $config = (function() {
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
            assertAlways.writeOK(res);
            assertAlways.eq(this.nDocumentsToInsert, res.nInserted);
        }

        function createIndexes(db, collName) {
            // The number of indexes created here is also stored in data.nIndexes
            var aResult = db[collName].ensureIndex({a: 1});
            assertAlways.commandWorked(aResult);

            var bResult = db[collName].ensureIndex({b: 1});
            assertAlways.commandWorked(bResult);

            var cResult = db[collName].ensureIndex({c: 1});
            assertAlways.commandWorked(cResult);
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
            var res =
                db.runCommand({compact: this.threadCollName, paddingFactor: 1.0, force: true});
            if (!isEphemeral(db)) {
                assertAlways.commandWorked(res);
            } else {
                assertAlways.commandFailedWithCode(res, ErrorCodes.CommandNotSupported);
            }
        }

        function query(db, collName) {
            var count = db[this.threadCollName].find().itcount();
            assertWhenOwnColl.eq(count,
                                 this.nDocumentsToInsert,
                                 'number of documents in ' +
                                     'collection should not change following a compact');
            var indexesCount = db[this.threadCollName].getIndexes().length;
            assertWhenOwnColl.eq(indexesCount, this.nIndexes);
        }

        return {init: init, collectionSetup: collectionSetup, compact: compact, query: query};
    })();

    var transitions = {
        init: {collectionSetup: 1},
        collectionSetup: {compact: 0.5, query: 0.5},
        compact: {compact: 0.5, query: 0.5},
        query: {compact: 0.5, query: 0.5}
    };

    var teardown = function teardown(db, collName, cluster) {
        var pattern = new RegExp('^' + this.prefix + '_\\d+$');
        dropCollections(db, pattern);
    };

    return {
        threadCount: 15,
        iterations: 10,
        states: states,
        transitions: transitions,
        teardown: teardown,
        data: data
    };
})();

'use strict';

/**
 * remove_and_bulk_insert.js
 *
 * Each thread alternates between inserting 1000 documents and deleting the entire contents of the
 * collection.
 *
 * This workload was designed to reproduce SERVER-20512, where a record in an evicted page was
 * accessed after a WriteConflictException occurred in Collection::deleteDocument().
 */
var $config = (function() {

    var states = {
        insert: function insert(db, collName) {
            var bulk = db[collName].initializeUnorderedBulkOp();
            for (var i = 0; i < 1000; ++i) {
                bulk.insert({});
            }
            assert.writeOK(bulk.execute());
        },

        remove: function remove(db, collName) {
            var res = db[collName].remove({});
            assertAlways.lte(0, res.nRemoved, tojson(res));
        }
    };

    var transitions = {insert: {insert: 0.5, remove: 0.5}, remove: {insert: 0.5, remove: 0.5}};

    return {
        threadCount: 5,
        iterations: 50,
        startState: 'insert',
        states: states,
        transitions: transitions
    };

})();

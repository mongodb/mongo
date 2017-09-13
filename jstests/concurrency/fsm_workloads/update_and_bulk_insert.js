'use strict';

/**
 * update_and_bulk_insert.js
 *
 * Each thread alternates between inserting 100 documents and updating every document in the
 * collection.
 *
 * This workload was designed to test for an issue similar to SERVER-20512 with UpdateStage, where
 * we attempted to make a copy of a record after a WriteConflictException occurred in
 * Collection::updateDocument().
 */
var $config = (function() {

    var states = {
        insert: function insert(db, collName) {
            var bulk = db[collName].initializeUnorderedBulkOp();
            for (var i = 0; i < 100; ++i) {
                bulk.insert({});
            }
            assert.writeOK(bulk.execute());
        },

        update: function update(db, collName) {
            var res = db[collName].update({}, {$inc: {n: 1}}, {multi: true});
            assertAlways.lte(0, res.nMatched, tojson(res));
            if (db.getMongo().writeMode() === 'commands') {
                assertAlways.eq(res.nMatched, res.nModified, tojson(res));
            }
            assertAlways.eq(0, res.nUpserted, tojson(res));
        }
    };

    var transitions = {insert: {insert: 0.2, update: 0.8}, update: {insert: 0.2, update: 0.8}};

    return {
        threadCount: 5,
        iterations: 50,
        startState: 'insert',
        states: states,
        transitions: transitions
    };

})();

'use strict';

/**
 * update_check_index.js
 *
 * Ensures that concurrent multi updates cannot produce duplicate index entries. Regression test
 * for SERVER-17132.
 */
var $config = (function() {

    var states = (function() {
        function multiUpdate(db, collName) {
            // Set 'c' to some random value.
            var newC = Random.randInt(1000);
            db[collName].update({a: 1, b: 1}, {$set: {c: newC}}, {multi: true});
        }

        return {multiUpdate: multiUpdate};
    })();

    var transitions = {multiUpdate: {multiUpdate: 1.0}};

    function setup(db, collName, cluster) {
        assertAlways.commandWorked(db[collName].ensureIndex({a: 1}));
        assertAlways.commandWorked(db[collName].ensureIndex({b: 1}));
        assertAlways.commandWorked(db[collName].ensureIndex({c: 1}));

        for (var i = 0; i < 10; i++) {
            assertAlways.writeOK(db[collName].insert({a: 1, b: 1, c: 1}));
        }
    }

    // Asserts that the number of index entries for all three entries matches the number of docs
    // in the collection. This condition should always be true for non-multikey indices. If it is
    // not true, then the index has been corrupted.
    function teardown(db, collName, cluster) {
        assertWhenOwnColl(function() {
            var numIndexKeys = db[collName].find({}, {_id: 0, a: 1}).hint({a: 1}).itcount();
            var numDocs = db[collName].find().itcount();
            assertWhenOwnColl.eq(
                numIndexKeys, numDocs, 'index {a: 1} has wrong number of index keys');

            numIndexKeys = db[collName].find({}, {_id: 0, b: 1}).hint({b: 1}).itcount();
            assertWhenOwnColl.eq(
                numIndexKeys, numDocs, 'index {b: 1} has wrong number of index keys');

            numIndexKeys = db[collName].find({}, {_id: 0, c: 1}).hint({c: 1}).itcount();
            assertWhenOwnColl.eq(
                numIndexKeys, numDocs, 'index {c: 1} has wrong number of index keys');
        });
    }

    return {
        threadCount: 10,
        iterations: 100,
        states: states,
        startState: 'multiUpdate',
        transitions: transitions,
        setup: setup,
        teardown: teardown
    };
})();

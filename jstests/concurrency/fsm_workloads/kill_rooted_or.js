'use strict';

/**
 * kill_rooted_or.js
 *
 * Queries using a rooted $or predicate to cause plan selection to use the subplanner. Tests that
 * the subplanner correctly halts plan execution when the collection is dropped or a candidate index
 * is dropped.
 *
 * This workload was designed to reproduce SERVER-24761.
 */
var $config = (function() {

    // Use the workload name as the collection name, since the workload name is assumed to be
    // unique.
    var uniqueCollectionName = 'kill_rooted_or';

    var data = {
        collName: uniqueCollectionName,
        indexSpecs: [
            {a: 1},
            {a: 1, c: 1},
            {b: 1},
            {b: 1, c: 1},
        ]
    };

    var states = {
        query: function query(db, collName) {
            var cursor = db[this.collName].find({$or: [{a: 0}, {b: 0}]});
            try {
                assert.eq(0, cursor.itcount());
            } catch (e) {
                // Ignore errors due to the plan executor being killed.
            }
        },

        dropCollection: function dropCollection(db, collName) {
            db[this.collName].drop();

            // Recreate all of the indexes on the collection.
            this.indexSpecs.forEach(indexSpec => {
                assertAlways.commandWorked(db[this.collName].createIndex(indexSpec));
            });
        },

        dropIndex: function dropIndex(db, collName) {
            var indexSpec = this.indexSpecs[Random.randInt(this.indexSpecs.length)];

            // We don't assert that the command succeeded when dropping an index because it's
            // possible another thread has already dropped this index.
            db[this.collName].dropIndex(indexSpec);

            // Recreate the index that was dropped.
            assertAlways.commandWorked(db[this.collName].createIndex(indexSpec));
        }
    };

    var transitions = {
        query: {query: 0.8, dropCollection: 0.1, dropIndex: 0.1},
        dropCollection: {query: 1},
        dropIndex: {query: 1}
    };

    function setup(db, collName, cluster) {
        this.indexSpecs.forEach(indexSpec => {
            assertAlways.commandWorked(db[this.collName].createIndex(indexSpec));
        });
    }

    return {
        threadCount: 10,
        iterations: 50,
        data: data,
        states: states,
        startState: 'query',
        transitions: transitions,
        setup: setup
    };

})();

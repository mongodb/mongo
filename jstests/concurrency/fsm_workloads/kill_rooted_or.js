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
    // unique. Note that we choose our own collection name instead of using the collection provided
    // by the concurrency framework, because this workload drops its collection.
    var uniqueCollectionName = 'kill_rooted_or';

    var data = {
        collName: uniqueCollectionName,
        indexSpecs: [
            {a: 1},
            {a: 1, c: 1},
            {b: 1},
            {b: 1, c: 1},
        ],
        numDocs: 200,
    };

    var states = {
        query: function query(db, collNameUnused) {
            var cursor = db[this.collName].find({$or: [{a: 0}, {b: 0}]});
            try {
                // We don't know exactly how many documents will be in the collection at the time of
                // the query, so we can't verify this value.
                cursor.itcount();
            } catch (e) {
                // We expect to see errors caused by the plan executor being killed, because of the
                // collection getting dropped on another thread.
                if (ErrorCodes.QueryPlanKilled != e.code) {
                    throw e;
                }
            }
        },

        dropCollection: function dropCollection(db, collNameUnused) {
            db[this.collName].drop();

            // Restore the collection.
            populateIndexes(db[this].collName, this.indexSpecs);
            populateCollection(db[this.collName], this.numDocs);
        },

        dropIndex: function dropIndex(db, collNameUnused) {
            var indexSpec = this.indexSpecs[Random.randInt(this.indexSpecs.length)];

            // We don't assert that the command succeeded when dropping an index because it's
            // possible another thread has already dropped this index.
            db[this.collName].dropIndex(indexSpec);

            // Recreate the index that was dropped. (See populateIndexes() for why we ignore the
            // CannotImplicitlyCreateCollection error.)
            assertAlways.commandWorkedOrFailedWithCode(db[this.collName].createIndex(indexSpec),
                                                       ErrorCodes.CannotImplicitlyCreateCollection);
        }
    };

    var transitions = {
        query: {query: 0.8, dropCollection: 0.1, dropIndex: 0.1},
        dropCollection: {query: 1},
        dropIndex: {query: 1}
    };

    function populateIndexes(coll, indexSpecs) {
        indexSpecs.forEach(indexSpec => {
            // In sharded configurations, there's a limit to how many times mongos can retry an
            // operation that fails because it wants to implicitly create a collection that is
            // concurrently dropped. Normally, that's fine, but if some jerk keeps dropping our
            // collection (as in the 'dropCollection' state of this test), then we run out of
            // retries and get a CannotImplicitlyCreateCollection error once in a while, which we
            // have to ignore.
            assertAlways.commandWorkedOrFailedWithCode(coll.createIndex(indexSpec),
                                                       ErrorCodes.CannotImplicitlyCreateCollection);
        });
    }

    function populateCollection(coll, numDocs) {
        // See populateIndexes() for why we ignore CannotImplicitlyCreateCollection errors.
        // Similarly, this bulk insert can also give up with a NoProgressMade error after repeated
        // attempts in the sharded causal consistency configuration. We also ignore that error.
        const bulkInsertResult = coll.insert(Array(numDocs).fill({a: 0, b: 0, c: 0}));
        assertAlways(!bulkInsertResult.hasWriteConcernError(), bulkInsertResult);
        bulkInsertResult.getWriteErrors().forEach(err => {
            assertAlways.contains(
                err.code, [ErrorCodes.CannotImplicitlyCreateCollection, ErrorCodes.NoProgressMade]);
        }, bulkInsertResult);
    }

    function setup(db, collNameUnused, cluster) {
        populateIndexes(db[this.collName], this.indexSpecs);
        populateCollection(db[this.collName], this.numDocs);
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

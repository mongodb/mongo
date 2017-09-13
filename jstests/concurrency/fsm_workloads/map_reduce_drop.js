'use strict';

/**
 * map_reduce_drop.js
 *
 * This workload generates random data and inserts it into a collection.
 * It then runs simultaneous mapReduce commands while dropping the source
 * collection or source database.  It repopulates the data before each
 * mapReduce in an attempt to ensure that the mapReduce commands are
 * actually doing work.
 *
 * This workload serves as a regression test for SERVER-6757, SERVER-15087,
 * and SERVER-15842.
 */

var $config = (function() {

    // Use a unique database name for this workload because we'll be dropping
    // the db periodically.  (Using the default database would cause other
    // workloads to fail when running multiple workloads in parallel or in
    // composed mode.)
    var uniqueDBName = 'map_reduce_drop';

    var data = {
        mapper: function mapper() {
            emit(this.key, 1);
        },
        reducer: function reducer() {
            // This dummy reducer is present to enable the database and collection
            // drops to occur during different phases of the mapReduce.
            return 1;
        },
        numDocs: 250
    };

    var states = (function() {

        function dropColl(db, collName) {
            var mapReduceDB = db.getSiblingDB(db.getName() + uniqueDBName);

            // We don't check the return value of drop() because the collection
            // might not exist due to a drop() in another thread.
            mapReduceDB[collName].drop();
        }

        function dropDB(db, collName) {
            var mapReduceDB = db.getSiblingDB(db.getName() + uniqueDBName);

            var res = mapReduceDB.dropDatabase();
            assertAlways.commandWorked(res);
        }

        function mapReduce(db, collName) {
            var mapReduceDB = db.getSiblingDB(db.getName() + uniqueDBName);

            // Try to ensure that some documents have been inserted before running
            // the mapReduce command.  Although it's possible for the documents to
            // be dropped by another thread, some mapReduce commands should end up
            // running on non-empty collections by virtue of the number of
            // iterations and threads in this workload.
            var bulk = mapReduceDB[collName].initializeUnorderedBulkOp();
            for (var i = 0; i < this.numDocs; ++i) {
                bulk.insert({key: Random.randInt(10000)});
            }
            var res = bulk.execute();
            assertAlways.writeOK(res);

            var options = {
                finalize: function finalize(key, reducedValue) {
                    return reducedValue;
                },
                out: collName + '_out'
            };

            try {
                mapReduceDB[collName].mapReduce(this.mapper, this.reducer, options);
            } catch (e) {
                // Ignore all mapReduce exceptions.  This workload is only concerned
                // with verifying server availability.
            }
        }

        return {dropColl: dropColl, dropDB: dropDB, mapReduce: mapReduce};

    })();

    var transitions = {
        dropColl: {mapReduce: 1},
        dropDB: {mapReduce: 1},
        mapReduce: {mapReduce: 0.7, dropDB: 0.05, dropColl: 0.25}
    };

    function teardown(db, collName, cluster) {
        var mapReduceDB = db.getSiblingDB(db.getName() + uniqueDBName);

        // Ensure that the database that was created solely for this workload
        // has been dropped, in case it hasn't already been dropped by a
        // worker thread.
        var res = mapReduceDB.dropDatabase();
        assertAlways.commandWorked(res);
    }

    return {
        threadCount: 5,
        iterations: 10,
        data: data,
        states: states,
        startState: 'mapReduce',
        transitions: transitions,
        teardown: teardown
    };

})();

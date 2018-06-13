'use strict';

/**
 * plan_cache_drop_database.js
 *
 * Repeatedly executes count queries with limits against a collection that
 * is periodically dropped (as part of a database drop).  This combination of
 * events triggers the concurrent destruction of a Collection object and
 * the updating of said object's PlanCache (SERVER-17117).
 */
var $config = (function() {

    function populateData(db, collName) {
        var coll = db[collName];

        var bulk = coll.initializeUnorderedBulkOp();
        for (var i = 0; i < 1000; ++i) {
            bulk.insert({a: 1, b: Random.rand()});
        }
        var res = bulk.execute();
        assertAlways.writeOK(res);

        // Create two indexes to force plan caching: The {a: 1} index is
        // cached by the query planner because we query on a single value
        // of 'a' and a range of 'b' values.
        assertAlways.commandWorked(coll.ensureIndex({a: 1}));
        assertAlways.commandWorked(coll.ensureIndex({b: 1}));
    }

    var states = (function() {

        function count(db, collName) {
            var coll = db.getSiblingDB(this.planCacheDBName)[collName];

            var cmdObj = {query: {a: 1, b: {$gt: Random.rand()}}, limit: Random.randInt(10)};

            // We can't use assertAlways.commandWorked here because the plan
            // executor can be killed during the count.
            coll.runCommand('count', cmdObj);
        }

        function dropDB(db, collName) {
            var myDB = db.getSiblingDB(this.planCacheDBName);

            // We can't assert anything about the dropDatabase return value
            // because the database might not exist due to other threads
            // calling dropDB.
            myDB.dropDatabase();

            // Re-populate the data to make plan caching possible.
            populateData(myDB, collName);
        }

        return {count: count, dropDB: dropDB};

    })();

    var transitions = {count: {count: 0.95, dropDB: 0.05}, dropDB: {count: 0.95, dropDB: 0.05}};

    function setup(db, collName, cluster) {
        this.planCacheDBName = db.getName() + 'plan_cache_drop_database';

        var myDB = db.getSiblingDB(this.planCacheDBName);
        populateData(myDB, collName);
    }

    return {
        threadCount: 10,
        iterations: 50,
        states: states,
        startState: 'count',
        transitions: transitions,
        setup: setup,
    };

})();

/**
 * plan_cache_drop_database.js
 *
 * Repeatedly executes count queries with limits against a collection that
 * is periodically dropped (as part of a database drop).  This combination of
 * events triggers the concurrent destruction of a Collection object and
 * the updating of said object's PlanCache (SERVER-17117).
 *
 * @tags: [
 *  # cannot createIndex after dropDatabase without sharding first
 *  assumes_against_mongod_not_mongos,
 * ]
 */
export const $config = (function () {
    function populateData(db, collName) {
        let coll = db[collName];

        try {
            let bulk = coll.initializeUnorderedBulkOp();
            for (let i = 0; i < 1000; ++i) {
                bulk.insert({a: 1, b: Random.rand()});
            }
            let res = bulk.execute();
            assert.commandWorked(res);

            // Create two indexes to force plan caching: The {a: 1} index is
            // cached by the query planner because we query on a single value
            // of 'a' and a range of 'b' values.
            const expectedErrorCodes = [
                ErrorCodes.DatabaseDropPending,
                ErrorCodes.IndexBuildAborted,
                ErrorCodes.NoMatchingDocument,
            ];
            if (TestData.testingReplicaSetEndpoint) {
                expectedErrorCodes.push(ErrorCodes.NamespaceNotFound);
                // These sharding error codes can bubble up if we exhaust all retries.
                expectedErrorCodes.push(ErrorCodes.CannotImplicitlyCreateCollection);
                expectedErrorCodes.push(ErrorCodes.StaleDbVersion);
                expectedErrorCodes.push(ErrorCodes.StaleConfig);
            }
            assert.commandWorkedOrFailedWithCode(coll.createIndex({a: 1}), expectedErrorCodes);
            assert.commandWorkedOrFailedWithCode(coll.createIndex({b: 1}), expectedErrorCodes);
        } catch (ex) {
            assert.eq(true, ex instanceof BulkWriteError, tojson(ex));
            assert.writeErrorWithCode(ex, ErrorCodes.DatabaseDropPending, tojson(ex));
        }
    }

    let states = (function () {
        function count(db, collName) {
            let coll = db.getSiblingDB(this.planCacheDBName)[collName];

            let cmdObj = {query: {a: 1, b: {$gt: Random.rand()}}, limit: Random.randInt(10)};

            // We can't use assert.commandWorked here because the plan
            // executor can be killed during the count.
            coll.runCommand("count", cmdObj);
        }

        function dropDB(db, collName) {
            let myDB = db.getSiblingDB(this.planCacheDBName);

            // We can't assert anything about the dropDatabase return value
            // because the database might not exist due to other threads
            // calling dropDB.
            myDB.dropDatabase();

            // Re-populate the data to make plan caching possible.
            populateData(myDB, collName);
        }

        return {count: count, dropDB: dropDB};
    })();

    let transitions = {count: {count: 0.95, dropDB: 0.05}, dropDB: {count: 0.95, dropDB: 0.05}};

    function setup(db, collName, cluster) {
        this.planCacheDBName = db.getName() + "plan_cache_drop_database";

        let myDB = db.getSiblingDB(this.planCacheDBName);
        populateData(myDB, collName);
    }

    return {
        threadCount: 10,
        iterations: 50,
        states: states,
        startState: "count",
        transitions: transitions,
        setup: setup,
    };
})();

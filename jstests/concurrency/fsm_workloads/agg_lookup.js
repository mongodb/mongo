'use strict';

/**
 * agg_lookup.js
 *
 * Runs a $lookup aggregation simultaneously with updates.
 */
var $config = (function() {
    const data = {numDocs: 100};

    const states = (function() {
        function query(db, collName) {
            // Run the aggregate with 'allowDiskUse' if it was configured during setup.
            const aggOptions = {allowDiskUse: this.allowDiskUse};

            function getQueryResults() {
                let arr = null;
                try {
                    const cursor = db[collName]
                          .aggregate([
                              {
                                  $lookup: {
                                      from: collName,
                                      localField: "_id",
                                      foreignField: "to",
                                      as: "out",
                                  }
                              },
                          ], aggOptions);

                    arr = cursor.toArray();
                } catch (e) {
                    if (TestData.runningWithShardStepdowns) {
                        // When running with stepdowns, we expect to sometimes see the query
                        // killed.
                        assert.eq(e.code, ErrorCodes.QueryPlanKilled);
                    } else {
                        throw e;
                    }
                }

                return arr;
            }

            const res = getQueryResults();
            if (res) {
                assertWhenOwnColl.eq(res.length, data.numDocs);
            }
        }

        function update(db, collName) {
            const index = Random.randInt(this.numDocs + 1);
            const update = Random.randInt(this.numDocs + 1);
            const res = db[collName].update({_id: index}, {$set: {to: update}});
            assertWhenOwnColl.commandWorked(res);
        }

        return {query, update};
    })();

    const transitions = {query: {query: 0.5, update: 0.5}, update: {query: 0.5, update: 0.5}};

    function setup(db, collName, cluster) {
        // Load example data.
        const bulk = db[collName].initializeUnorderedBulkOp();
        for (let i = 0; i < this.numDocs; ++i) {
            bulk.insert({_id: i, to: i + 1});
        }

        const res = bulk.execute();
        assertWhenOwnColl.commandWorked(res);
        assertWhenOwnColl.eq(this.numDocs, res.nInserted);
        assertWhenOwnColl.eq(this.numDocs, db[collName].find().itcount());

        const getParam = db.adminCommand({getParameter: 1, internalQueryForceClassicEngine: 1});
        const isLookupPushdownEnabled =
            getParam.hasOwnProperty("internalQueryForceClassicEngine") &&
            !getParam.internalQueryForceClassicEngine.value;

        this.allowDiskUse = true;
        // If $lookup pushdown into SBE is enabled, we select a random join algorithm to use and
        // set the collection up accordingly.
        if (isLookupPushdownEnabled) {
            // Use a random join algorithm on each test run.
            const numStrategies = 3;
            const strategy = Random.randInt(numStrategies);
            if (strategy === 0) {
                jsTestLog("Using hash join");
            } else if (strategy === 1) {
                assertWhenOwnColl.commandWorked(db[collName].createIndex({to: 1}));
                jsTestLog("Using index join");
                this.allowDiskUse = false;
            } else {
                jsTestLog("Using nested loop join");
                this.allowDiskUse = false;
            }
        }
    }

    function teardown(db, collName) {
        // Drop indexes, if any were created.
        assertWhenOwnColl.commandWorked(db[collName].dropIndexes());
    }

    return {
        threadCount: 10,
        iterations: 100,
        states: states,
        startState: 'query',
        transitions: transitions,
        data: data,
        setup: setup,
        teardown: teardown,
    };
})();

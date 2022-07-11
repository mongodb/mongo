'use strict';

/**
 * agg_graph_lookup.js
 *
 * Runs a $graphLookup aggregation simultaneously with updates.
 */
var $config = (function() {
    const data = {numDocs: 1000};

    const states = (function() {
        function query(db, collName) {
            const limitAmount = 20;
            const startingId = Random.randInt(this.numDocs - limitAmount);

            function getQueryResults() {
                let arr = null;
                try {
                    const cursor = db[collName]
                          .aggregate([
                              {$match: {_id: {$gt: startingId}}},
                              {
                                  $graphLookup: {
                                      from: collName,
                                      startWith: "$to",
                                      connectToField: "_id",
                                      connectFromField: "to",
                                      maxDepth: 10,
                                      as: "out",
                                  }
                              },
                              {$limit: limitAmount}
                          ]);

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
                assertWhenOwnColl.eq(res.length, limitAmount);
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
    }

    return {
        threadCount: 10,
        iterations: 100,
        states: states,
        startState: 'query',
        transitions: transitions,
        data: data,
        setup: setup,
    };
})();

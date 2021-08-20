'use strict';

load("jstests/libs/fixture_helpers.js");  // For isSharded.

/**
 * agg_graph_lookup.js
 *
 * Runs a $graphLookup aggregation simultaneously with updates.
 */
var $config = (function() {
    var data = {numDocs: 1000};
    var isShardedAndShardedLookupDisabled = false;

    var states = (function() {
        function query(db, collName) {
            if (this.isShardedAndShardedLookupDisabled) {
                return;
            }

            var limitAmount = 20;
            var startingId = Random.randInt(this.numDocs - limitAmount);
            var res = db[collName]
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
                          ])
                          .toArray();

            assertWhenOwnColl.eq(res.length, limitAmount);
        }

        function update(db, collName) {
            var index = Random.randInt(this.numDocs + 1);
            var update = Random.randInt(this.numDocs + 1);
            var res = db[collName].update({_id: index}, {$set: {to: update}});
            assertWhenOwnColl.commandWorked(res);
        }

        return {query, update};
    })();

    var transitions = {query: {query: 0.5, update: 0.5}, update: {query: 0.5, update: 0.5}};

    function setup(db, collName, cluster) {
        // Do not run the rest of the tests if the foreign collection is implicitly sharded but the
        // flag to allow $lookup/$graphLookup into a sharded collection is disabled.
        const getShardedLookupParam =
            db.adminCommand({getParameter: 1, featureFlagShardedLookup: 1});
        const isShardedLookupEnabled =
            getShardedLookupParam.hasOwnProperty("featureFlagShardedLookup") &&
            getShardedLookupParam.featureFlagShardedLookup.value;
        if (FixtureHelpers.isSharded(db[collName]) && !isShardedLookupEnabled) {
            jsTestLog(
                "Skipping test because the sharded lookup feature flag is disabled and we have sharded collections");
            this.isShardedAndShardedLookupDisabled = true;
            return;
        }

        // Load example data.
        var bulk = db[collName].initializeUnorderedBulkOp();
        for (var i = 0; i < this.numDocs; ++i) {
            bulk.insert({_id: i, to: i + 1});
        }
        var res = bulk.execute();
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

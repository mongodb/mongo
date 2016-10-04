'use strict';

/**
 * agg_graph_lookup.js
 *
 * Runs a $graphLookup aggregation simultaneously with updates.
 */
var $config = (function() {

    var data = {numDocs: 1000};

    var states = {
        query: function query(db, collName) {
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
        },
        update: function update(db, collName) {
            var index = Random.randInt(this.numDocs + 1);
            var update = Random.randInt(this.numDocs + 1);
            var res = db[collName].update({_id: index}, {$set: {to: update}});
            assertWhenOwnColl.writeOK(res);
        }
    };

    var transitions = {query: {query: 0.5, update: 0.5}, update: {query: 0.5, update: 0.5}};

    function setup(db, collName, cluster) {
        // Load example data.
        var bulk = db[collName].initializeUnorderedBulkOp();
        for (var i = 0; i < this.numDocs; ++i) {
            bulk.insert({_id: i, to: i + 1});
        }
        var res = bulk.execute();
        assertWhenOwnColl.writeOK(res);
        assertWhenOwnColl.eq(this.numDocs, res.nInserted);
        assertWhenOwnColl.eq(this.numDocs, db[collName].find().itcount());
    }

    function teardown(db, collName, cluster) {
        assertWhenOwnColl(db[collName].drop());
    }

    return {
        threadCount: 10,
        iterations: 100,
        states: states,
        startState: 'query',
        transitions: transitions,
        data: data,
        setup: setup,
        teardown: teardown
    };
})();

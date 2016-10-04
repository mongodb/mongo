// In MongoDB 3.4, $graphLookup was introduced. In this file, we test some complex graphs.

(function() {
    "use strict";

    var local = db.local;
    var foreign = db.foreign;

    local.drop();
    foreign.drop();

    var airports = [
        {_id: "JFK", connects: ["PWM", "BOS", "LGA", "SFO"]},
        {_id: "PWM", connects: ["BOS", "JFK"]},
        {_id: "BOS", connects: ["PWM", "JFK", "LGA"]},
        {_id: "SFO", connects: ["JFK", "MIA"]},
        {_id: "LGA", connects: ["BOS", "JFK", "ORD"]},
        {_id: "ORD", connects: ["LGA"]},
        {_id: "ATL", connects: ["MIA"]},
        {_id: "MIA", connects: ["ATL", "SFO"]}
    ];

    var bulk = foreign.initializeUnorderedBulkOp();
    airports.forEach(function(a) {
        bulk.insert(a);
    });
    assert.writeOK(bulk.execute());

    // Insert a dummy document so that something will flow through the pipeline.
    local.insert({});

    // Perform a simple $graphLookup and ensure it retrieves every result.
    var res = local
                  .aggregate({
                      $graphLookup: {
                          from: "foreign",
                          startWith: "PWM",
                          connectFromField: "connects",
                          connectToField: "_id",
                          as: "connections"
                      }
                  })
                  .toArray()[0];

    // "foreign" represents a connected graph.
    assert.eq(res.connections.length, airports.length);

    // Perform a $graphLookup and ensure it correctly computes the shortest path to a node when more
    // than one path exists.
    res = local
              .aggregate({
                  $graphLookup: {
                      from: "foreign",
                      startWith: "BOS",
                      connectFromField: "connects",
                      connectToField: "_id",
                      depthField: "hops",
                      as: "connections"
                  }
              },
                         {$unwind: "$connections"},
                         {$project: {_id: "$connections._id", hops: "$connections.hops"}})
              .toArray();

    var expectedDistances = {BOS: 0, PWM: 1, JFK: 1, LGA: 1, ORD: 2, SFO: 2, MIA: 3, ATL: 4};

    assert.eq(res.length, airports.length);
    res.forEach(function(c) {
        assert.eq(c.hops, expectedDistances[c._id]);
    });

    // Disconnect the graph, and ensure we don't find the other side.
    foreign.remove({_id: "JFK"});

    res = db.local
              .aggregate({
                  $graphLookup: {
                      from: "foreign",
                      startWith: "ATL",
                      connectFromField: "connects",
                      connectToField: "_id",
                      as: "connections"
                  }
              })
              .toArray()[0];

    // ATL should now connect to itself, MIA, and SFO.
    assert.eq(res.connections.length, 3);
}());

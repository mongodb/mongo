// In MongoDB 3.4, $graphLookup was introduced. In this file, we test basic behavior and correctness
// of the stage.

(function() {
    "use strict";

    var local = db.local;
    var foreign = db.foreign;

    local.drop();
    foreign.drop();

    var bulk = foreign.initializeUnorderedBulkOp();
    for (var i = 0; i < 100; i++) {
        bulk.insert({_id: i, neighbors: [i - 1, i + 1]});
    }
    assert.writeOK(bulk.execute());

    assert.writeOK(local.insert({starting: 50}));

    // Perform a simple $graphLookup and ensure it retrieves every result.
    var res = local
                  .aggregate({
                      $graphLookup: {
                          from: "foreign",
                          startWith: "$starting",
                          connectFromField: "neighbors",
                          connectToField: "_id",
                          as: "integers"
                      }
                  })
                  .toArray()[0];

    assert.eq(res.integers.length, 100);

    // Perform a $graphLookup and ensure it respects "maxDepth".
    res = local
              .aggregate({
                  $graphLookup: {
                      from: "foreign",
                      startWith: "$starting",
                      connectFromField: "neighbors",
                      connectToField: "_id",
                      maxDepth: 5,
                      as: "integers"
                  }
              })
              .toArray()[0];

    // At depth zero, we retrieve one integer, and two for every depth thereafter.
    assert.eq(res.integers.length, 11);

    // Perform a $graphLookup and ensure it properly evaluates "startWith".
    res = local
              .aggregate({
                  $graphLookup: {
                      from: "foreign",
                      startWith: {$add: ["$starting", 3]},
                      connectFromField: "neighbors",
                      connectToField: "_id",
                      maxDepth: 0,
                      as: "integers"
                  }
              })
              .toArray()[0];

    assert.eq(res.integers.length, 1);
    assert.eq(res.integers[0]._id, 53);

    // Perform a $graphLookup and ensure it properly expands "startWith".
    res = local
              .aggregate({
                  $graphLookup: {
                      from: "foreign",
                      startWith: {$literal: [1, 2, 3]},
                      connectFromField: "neighbors",
                      connectToField: "_id",
                      maxDepth: 0,
                      as: "integers"
                  }
              })
              .toArray()[0];

    assert.eq(res.integers.length, 3);

    // $graphLookup should not recurse when the 'connectFromField' is missing. However, if it
    // mistakenly does, then it would look for a 'connectToField' value of null. In order to prevent
    // regressions, we insert a document with a 'connectToField' value of null, then perform a
    // $graphLookup, and ensure that we do not find the erroneous document.
    assert.writeOK(foreign.remove({_id: 51}));
    assert.writeOK(foreign.insert({_id: 51}));
    assert.writeOK(foreign.insert({_id: null, neighbors: [50, 52]}));

    res = local
              .aggregate({
                  $graphLookup: {
                      from: "foreign",
                      startWith: "$starting",
                      connectFromField: "neighbors",
                      connectToField: "_id",
                      as: "integers"
                  }
              })
              .toArray()[0];

    // Our result should be missing the values with _id from 52 to 99.
    assert.eq(res.integers.length, 52);

    // Perform a $graphLookup and ensure we don't go into an infinite loop when our graph is cyclic.
    assert.writeOK(foreign.remove({_id: {$in: [null, 51]}}));
    assert.writeOK(foreign.insert({_id: 51, neighbors: [50, 52]}));

    assert.writeOK(foreign.update({_id: 99}, {$set: {neighbors: [98, 0]}}));
    assert.writeOK(foreign.update({_id: 0}, {$set: {neighbors: [99, 1]}}));

    res = local
              .aggregate({
                  $graphLookup: {
                      from: "foreign",
                      startWith: "$starting",
                      connectFromField: "neighbors",
                      connectToField: "_id",
                      as: "integers"
                  }
              })
              .toArray()[0];

    assert.eq(res.integers.length, 100);

    // Perform a $graphLookup and ensure that "depthField" is properly populated.
    res = local
              .aggregate({
                  $graphLookup: {
                      from: "foreign",
                      startWith: "$starting",
                      connectFromField: "neighbors",
                      connectToField: "_id",
                      depthField: "distance",
                      as: "integers"
                  }
              })
              .toArray()[0];

    assert.eq(res.integers.length, 100);

    res.integers.forEach(function(n) {
        assert.eq(n.distance, Math.abs(50 - n._id));
    });
}());

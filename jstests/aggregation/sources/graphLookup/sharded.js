// In SERVER-23725, $graphLookup was introduced. In this file, we test that the expression behaves
// correctly on a sharded collection.
load("jstests/aggregation/extras/utils.js");  // For assertErrorCode.

(function() {
    "use strict";

    var st = new ShardingTest({name: "aggregation_graph_lookup", shards: 2, mongos: 1});

    st.adminCommand({enableSharding: "graphLookup"});
    st.ensurePrimaryShard("graphLookup", "shard0001");
    st.adminCommand({shardCollection: "graphLookup.local", key: {_id: 1}});

    var foreign = st.getDB("graphLookup").foreign;
    var local = st.getDB("graphLookup").local;

    var bulk = foreign.initializeUnorderedBulkOp();

    for (var i = 0; i < 100; i++) {
        bulk.insert({_id: i, next: i + 1});
    }
    assert.writeOK(bulk.execute());

    assert.writeOK(local.insert({}));

    var res = st.s.getDB("graphLookup")
                  .local
                  .aggregate({
                      $graphLookup: {
                          from: "foreign",
                          startWith: {$literal: 0},
                          connectToField: "_id",
                          connectFromField: "next",
                          as: "number_line"
                      }
                  })
                  .toArray();

    assert.eq(res.length, 1);
    assert.eq(res[0].number_line.length, 100);

    // Cannot perform a $graphLookup where the "from" collection is sharded.
    var pipeline = {
        $graphLookup: {
            from: "local",
            startWith: {$literal: 0},
            connectToField: "_id",
            connectFromField: "_id",
            as: "out"
        }
    };

    assertErrorCode(foreign, pipeline, 28769);
}());

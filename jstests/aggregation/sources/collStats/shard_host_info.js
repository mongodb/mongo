/**
 * Verifies that the $collStats aggregation stage includes the shard and hostname for each output
 * document when run via mongoS, and that the former is absent when run on a non-shard mongoD.
 */
(function() {
    "use strict";

    // Test mongoD behaviour using the standalone started by resmoke.py.
    let testDB = db.getSiblingDB(jsTestName());
    let testColl = testDB.test;

    // getHostName() doesn't include port, db.getMongo().host is 127.0.0.1:<port>
    const hostName = (getHostName() + ":" + db.getMongo().host.split(":")[1]);

    // Test that the shard field is absent and the host field is present when run on mongoD.
    assert.eq(testColl
                  .aggregate([
                      {$collStats: {latencyStats: {histograms: true}}},
                      {$group: {_id: {shard: "$shard", host: "$host"}}}
                  ])
                  .toArray(),
              [{_id: {host: hostName}}]);

    // Test that both shard and hostname are present for $collStats results on a sharded cluster.
    const st = new ShardingTest({name: jsTestName(), shards: 2});

    testDB = st.s.getDB(jsTestName());
    testColl = testDB.test;

    assert.commandWorked(testDB.dropDatabase());

    // Enable sharding on the test database.
    assert.commandWorked(testDB.adminCommand({enableSharding: testDB.getName()}));

    // Shard 'testColl' on {_id: 'hashed'}. This will automatically presplit the collection and
    // place chunks on each shard.
    assert.commandWorked(
        testDB.adminCommand({shardCollection: testColl.getFullName(), key: {_id: "hashed"}}));

    // Group $collStats result by $shard and $host to confirm that both fields are present.
    assert.eq(testColl
                  .aggregate([
                      {$collStats: {latencyStats: {histograms: true}}},
                      {$group: {_id: {shard: "$shard", host: "$host"}}},
                      {$sort: {_id: 1}}
                  ])
                  .toArray(),
              [
                {_id: {shard: "shard0000", host: st.shard0.host}},
                {_id: {shard: "shard0001", host: st.shard1.host}},
              ]);

    st.stop();
})();

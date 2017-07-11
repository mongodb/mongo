/**
 * Basic test that checks that mongos includes the cluster time metatadata in it's response.
 * This does not test cluster time propagation via the shell as there are many back channels
 * where the cluster time metadata can propagated, making it inherently racy.
 */
(function() {
    var st = new ShardingTest({shards: {rs0: {nodes: 3}}, mongosWaitsForKeys: true});
    st.s.adminCommand({enableSharding: 'test'});

    var db = st.s.getDB('test');

    // insert on one shard and extract the cluster time
    var res = assert.commandWorked(db.runCommand({insert: 'user', documents: [{x: 10}]}));
    assert.hasFields(res, ['$clusterTime']);

    var logicalTimeMetadata = res.$clusterTime;
    assert.hasFields(logicalTimeMetadata, ['clusterTime', 'signature']);

    res = st.rs0.getPrimary().adminCommand({replSetGetStatus: 1});

    var appliedTime = res.optimes.appliedOpTime.ts;
    assert.eq(0,
              timestampCmp(appliedTime, logicalTimeMetadata.clusterTime),
              'appliedTime: ' + tojson(appliedTime) + ' != clusterTime: ' +
                  tojson(logicalTimeMetadata.clusterTime));

    assert.commandWorked(db.runCommand({ping: 1, '$clusterTime': logicalTimeMetadata}));

    st.stop();

})();

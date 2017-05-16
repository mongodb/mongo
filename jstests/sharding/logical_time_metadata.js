/**
 * Basic test that checks that mongos includes the logical time metatadata in it's response.
 * This does not test logical time propagation via the shell as there are many back channels
 * where the logical time metadata can propagated, making it inherently racy.
 */
(function() {
    var st = new ShardingTest({shards: {rs0: {nodes: 3}}});
    st.s.adminCommand({enableSharding: 'test'});

    var db = st.s.getDB('test');

    // insert on one shard and extract the logical time
    var res = assert.commandWorked(db.runCommand({insert: 'user', documents: [{x: 10}]}));
    assert.hasFields(res, ['logicalTime']);

    var logicalTimeMetadata = res.logicalTime;
    assert.hasFields(logicalTimeMetadata, ['clusterTime', 'signature']);

    res = st.rs0.getPrimary().adminCommand({replSetGetStatus: 1});

    var appliedTime = res.optimes.appliedOpTime.ts;
    assert.eq(0,
              timestampCmp(appliedTime, logicalTimeMetadata.clusterTime),
              'appliedTime: ' + tojson(appliedTime) + ' != clusterTime: ' +
                  tojson(logicalTimeMetadata.clusterTime));

    assert.commandWorked(db.runCommand({ping: 1, logicalTime: logicalTimeMetadata}));

    st.stop();

})();

// Tests whether profiling can trigger stale config errors and interfere with write batches
// SERVER-13413

(function() {

    var st = new ShardingTest({shards: 1, mongos: 2});
    st.stopBalancer();

    var admin = st.s0.getDB('admin');
    var shards = st.s0.getCollection('config.shards').find().toArray();
    var coll = st.s0.getCollection('foo.bar');

    assert(admin.runCommand({enableSharding: coll.getDB() + ''}).ok);
    assert(admin.runCommand({shardCollection: coll + '', key: {_id: 1}}).ok);

    st.printShardingStatus();

    jsTest.log('Turning on profiling on ' + st.shard0);

    st.shard0.getDB(coll.getDB().toString()).setProfilingLevel(2);

    var profileColl = st.shard0.getDB(coll.getDB().toString()).system.profile;

    var inserts = [{_id: 0}, {_id: 1}, {_id: 2}];

    assert.writeOK(st.s1.getCollection(coll.toString()).insert(inserts));

    profileEntry = profileColl.findOne();
    assert.neq(null, profileEntry);
    printjson(profileEntry);
    assert.eq(profileEntry.query.documents, inserts);

    st.stop();

})();

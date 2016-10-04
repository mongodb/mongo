/**
 * This tests the basic cases for implicit database creation in a sharded cluster.
 */
(function() {
    "use strict";

    var st = new ShardingTest({shards: 2});
    var configDB = st.s.getDB('config');

    assert.eq(null, configDB.databases.findOne());

    var testDB = st.s.getDB('test');

    // Test that reads will not result into a new config.databases entry.
    assert.eq(null, testDB.user.findOne());
    assert.eq(null, configDB.databases.findOne({_id: 'test'}));

    assert.writeOK(testDB.user.insert({x: 1}));

    var testDBDoc = configDB.databases.findOne();
    assert.eq('test', testDBDoc._id, tojson(testDBDoc));

    // Test that inserting to another collection in the same database will not modify the existing
    // config.databases entry.
    assert.writeOK(testDB.bar.insert({y: 1}));
    assert.eq(testDBDoc, configDB.databases.findOne());

    st.s.adminCommand({enableSharding: 'foo'});
    var fooDBDoc = configDB.databases.findOne({_id: 'foo'});

    assert.neq(null, fooDBDoc);
    assert(fooDBDoc.partitioned);

    var newShardConn = MongoRunner.runMongod({});
    var unshardedDB = newShardConn.getDB('unshardedDB');

    unshardedDB.user.insert({z: 1});

    assert.commandWorked(st.s.adminCommand({addShard: newShardConn.name}));

    assert.neq(null, configDB.databases.findOne({_id: 'unshardedDB'}));

    MongoRunner.stopMongod(newShardConn.port);
    st.stop();

})();

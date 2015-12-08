/**
 * Test write concern with w parameter when writing directly to the config servers works as expected
 */
function writeToConfigTest(){
    jsTestLog("Testing data writes to config server with write concern");
    var st = new ShardingTest({ shards: 2 });
    var confDB = st.s.getDB( 'config' );

    assert.writeOK(confDB.settings.update({ _id: 'balancer' },
                                          { $set: { stopped: true }},
                                          { writeConcern: { w: 'majority' }}));

    // w:1 should still work - it gets automatically upconverted to w:majority
    assert.writeOK(confDB.settings.update({ _id: 'balancer' },
                                          { $set: { stopped: true }},
                                          { writeConcern: { w: 1 }}));

    // Write concerns other than w:1 and w:majority should fail.
    assert.writeError(confDB.settings.update({ _id: 'balancer' },
                                             { $set: { stopped: true }},
                                             { writeConcern: { w: 2 }}));

    st.stop();
}

/**
 * Test write concern with w parameter will not cause an error when writes to mongos
 * would trigger writes to config servers (in this test, split chunks is used).
 */
function configTest(){
    jsTestLog("Testing metadata writes to config server with write concern");
    var st = new ShardingTest({ shards: 1, rs: true, other: { chunkSize: 1 }});
     
    var mongos = st.s;
    var testDB = mongos.getDB( 'test' );
    var coll = testDB.user;
     
    testDB.adminCommand({ enableSharding: testDB.getName() });
    testDB.adminCommand({ shardCollection: coll.getFullName(), key: { x: 1 }});
     
    var chunkCount = function() {
        return mongos.getDB( 'config' ).chunks.find().count();
    };
     
    var initChunks = chunkCount();
    var currChunks = initChunks;
    var gleObj = null;
    var x = 0;
    var largeStr = new Array(1024*128).toString();

    assert.soon(function() {
        var bulk = coll.initializeUnorderedBulkOp();
        for (var i = 0; i < 100; i++) {
            bulk.insert({x: x++, largeStr: largeStr});
        }
        assert.writeOK(bulk.execute({w: 'majority', wtimeout: 60 * 1000}));
        currChunks = chunkCount();
        return currChunks > initChunks;
    }, function() { return "currChunks: " + currChunks + ", initChunks: " + initChunks; });

    st.stop();
}

writeToConfigTest();
configTest();


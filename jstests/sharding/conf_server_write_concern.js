/**
 * Test write concern with w parameter when writing directly to the config servers will
 * not cause an error.
 */
function writeToConfigTest(){
    var st = new ShardingTest({ shards: 2 });
    var confDB = st.s.getDB( 'config' );

    assert.writeOK(confDB.settings.update({ _id: 'balancer' },
                                          { $set: { stopped: true }},
                                          { writeConcern: { w: 'majority' }}));

    // w:1 should still work
    assert.writeOK(confDB.settings.update({ _id: 'balancer' },
                                          { $set: { stopped: true }},
                                          { writeConcern: { w: 1 }}));

    st.stop();
}

/**
 * Test write concern with w parameter will not cause an error when writes to mongos
 * would trigger writes to config servers (in this test, split chunks is used).
 */
function configTest( sync ){
    var st = new ShardingTest({ shards: 1, sync: sync,
          rs: { oplogSize: 10 }, other: { chunkSize: 1 }});
     
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
     
    while( currChunks <= initChunks ){
        assert.writeOK(coll.insert({ x: x++ }, { writeConcern: { w: 'majority' }}));
        currChunks = chunkCount();
    }

    st.stop();
}

writeToConfigTest();
configTest( false );
configTest( true ); // sync cluster config servers


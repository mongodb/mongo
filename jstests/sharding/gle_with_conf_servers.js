/**
 * Test getLastError with w parameter when writing directly to the config servers will
 * not cause an error.
 */
function writeToConfigTest(){
    var st = new ShardingTest({ shards: 2 });
    var confDB = st.s.getDB( 'config' );

    confDB.settings.update({ _id: 'balancer' }, { $set: { stopped: true }});
    var gleObj = confDB.runCommand({ getLastError: 1, w: 'majority' });

    assert( gleObj.ok );
    assert.eq(null, gleObj.err);

    // w:1 should still work
    confDB.settings.update({ _id: 'balancer' }, { $set: { stopped: true }});
    var gleObj = confDB.runCommand({ getLastError: 1, w: 1 });

    assert(gleObj.ok);
    assert.eq(null, gleObj.err);

    st.stop();
}

/**
 * Test getLastError with w parameter will not cause an error when writes to mongos
 * would trigger writes to config servers (in this test, split chunks is used).
 */
function configTest( configCount ){
    var st = new ShardingTest({ shards: 1, config: configCount,
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
        coll.insert({ x: x++ });
        gleObj = testDB.runCommand({ getLastError: 1, w: 'majority' });
        currChunks = chunkCount();
    }

    assert( gleObj.ok );
    assert.eq( null, gleObj.err );

    st.stop();  
}

writeToConfigTest();
configTest( 1 );
configTest( 3 ); // sync cluster config servers


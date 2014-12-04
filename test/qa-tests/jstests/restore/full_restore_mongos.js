(function() {

    // Tests that using mongorestore to do a full restore fails gracefully 
    // on a mongos.
    
    jsTest.log('Testing that restoration of a full dump fails gracefully'+
        ' against a mongos');

    var shardingTest = new ShardingTest('full_restore_mongos', 2, 0, 3, { chunksize: 1,
        enableBalancer: 0 });
    shardingTest.adminCommand({ enableSharding: 'full_restore_mongos' });

    // run a restore against the mongos
    var ret = runMongoProgram.apply(null, ['mongorestore', 
        '--port', shardingTest.s0.port,
        'jstests/restore/testdata/dump_empty']);
    assert.neq(0, ret);

    // success
    shardingTest.stop();

}());

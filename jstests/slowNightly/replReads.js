// Test that doing slaveOk reads from secondaries hits all the secondaries evenly

function testReadLoadBalancing(numReplicas) {

    s = new ShardingTest( "replReads" , 1 /* numShards */, 1 /* verboseLevel */, 1 /* numMongos */, { rs : true , numReplicas : numReplicas, chunksize : 1 } )

    s.adminCommand({enablesharding : "test"})
    s.config.settings.find().forEach(printjson)

    s.adminCommand({shardcollection : "test.foo", key : {_id : 1}})

    s.getDB("test").foo.insert({a : 123})

    primary = s._rs[0].test.liveNodes.master
    secondaries = s._rs[0].test.liveNodes.slaves

    for (var i = 0; i < secondaries.length; i++) {
        // For some reason I need to this twice on the slave connections to get it to work.  :(
        secondaries[i].getDB('test').setProfilingLevel(2)
        secondaries[i].getDB('test').setProfilingLevel(2)
    }

    for (var i = 0; i < secondaries.length * 10; i++) {
        conn = new Mongo(s._mongos[0].host)
        conn.setSlaveOk()
        conn.getDB('test').foo.findOne()
    }

    for (var i = 0; i < secondaries.length; i++) {
        assert.eq(10, secondaries[i].getDB('test').system.profile.find().count(), "Wrong number of read queries sent to secondary " + i)
    }

    s.stop()
}

//for (var i = 1; i < 10; i++) {
//    testReadLoadBalancing(i)
//}

// Is there a way that this can be run multiple times with different values?
testReadLoadBalancing(3)
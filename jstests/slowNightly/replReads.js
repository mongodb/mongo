2// Test that doing slaveOk reads from secondaries hits all the secondaries evenly

function testReadLoadBalancing(numReplicas) {

    s = new ShardingTest( "replReads" , 1 /* numShards */, 0 /* verboseLevel */, 1 /* numMongos */, { rs : true , numReplicas : numReplicas, chunksize : 1 } )

    s.adminCommand({enablesharding : "test"})
    s.config.settings.find().forEach(printjson)

    s.adminCommand({shardcollection : "test.foo", key : {_id : 1}})

    s.getDB("test").foo.insert({a : 123})

    primary = s._rs[0].test.liveNodes.master
    secondaries = s._rs[0].test.liveNodes.slaves

    function rsStats() {
        return s.getDB( "admin" ).runCommand( "connPoolStats" )["replicaSets"]["replReads-rs0"];
    }
    
    assert.eq( numReplicas , rsStats().hosts.length );
    assert.soon( 
        function() {
            var x = rsStats().hosts;
            for ( var i=0; i<x.length; i++ ) 
                if ( ! x[i].ok )
                    return false;
            return true;
        } 
    );
    
    for (var i = 0; i < secondaries.length; i++) {
        assert.soon( function(){ return secondaries[i].getDB("test").foo.count() > 0; } )
        secondaries[i].getDB('test').setProfilingLevel(2)
    }

    for (var i = 0; i < secondaries.length * 10; i++) {
        conn = new Mongo(s._mongos[0].host)
        conn.setSlaveOk()
        conn.getDB('test').foo.findOne()
    }

    for (var i = 0; i < secondaries.length; i++) {
        var profileCollection = secondaries[i].getDB('test').system.profile;
        assert.eq(10, profileCollection.find().count(), "Wrong number of read queries sent to secondary " + i + " " + tojson( profileCollection.find().toArray() ))
    }

    s.stop()
}

//for (var i = 1; i < 10; i++) {
//    testReadLoadBalancing(i)
//}

// Is there a way that this can be run multiple times with different values?
testReadLoadBalancing(3)

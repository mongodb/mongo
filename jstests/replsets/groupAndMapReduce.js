doTest = function( signal ) {

    // Test basic replica set functionality.
    // -- Replication
    // -- Failover

    // Replica set testing API
    // Create a new replica set test. Specify set name and the number of nodes you want.
    var replTest = new ReplSetTest( {name: 'testSet', nodes: 3} );

    // call startSet() to start each mongod in the replica set
    // this returns a list of nodes
    var nodes = replTest.startSet();

    // Call initiate() to send the replSetInitiate command
    // This will wait for initiation
    replTest.initiate();

    // Call getMaster to return a reference to the node that's been
    // elected master.
    var master = replTest.getMaster();

    // save some records
    var len = 100
    for (var i = 0; i < len; ++i) {
        master.getDB("foo").foo.save({a: i});
    }

    // This method will check the oplogs of the master
    // and slaves in the set and wait until the change has replicated.
    replTest.awaitReplication();
    print("Sleeping 10s for slaves to go to secondary state");
    sleep(10000);

    slaves = replTest.liveNodes.slaves;
    assert( slaves.length == 2, "Expected 2 slaves but length was " + slaves.length );
    slaves.forEach(function(slave) {
        // testing against 
        slave.setSlaveOk();

        // try to read from slave
        var count = slave.getDB("foo").foo.count();
        printjson( count );
        assert.eq( len , count , "slave count wrong: " + slave );
       
        var one = slave.getDB("foo").foo.findOne();
        printjson(one);

//        stats = slave.getDB("foo").adminCommand({replSetGetStatus:1});
//        printjson(stats);
 
        // now do group on slave
        count = slave.getDB("foo").foo.group({initial: {n:0}, reduce: function(obj,out){out.n++;}});
        printjson( count );
        assert.eq( len , count[0].n , "slave group count wrong: " + slave );
        
    });

    

    // Shut down the set and finish the test.
    replTest.stopSet( signal );
}

doTest( 15 );
print("SUCCESS");

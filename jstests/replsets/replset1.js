load("jstests/replsets/rslib.js");
var ssl_options;
doTest = function( signal ) {

    // Test basic replica set functionality.
    // -- Replication
    // -- Failover

    // Replica set testing API
    // Create a new replica set test. Specify set name and the number of nodes you want.
    var replTest = new ReplSetTest( {name: 'testSet', nodes: 3, nodeOptions: ssl_options} );

    // call startSet() to start each mongod in the replica set
    // this returns a list of nodes
    var nodes = replTest.startSet();

    // Call initiate() to send the replSetInitiate command
    // This will wait for initiation
    replTest.initiate();

    // Call getMaster to return a reference to the node that's been
    // elected master.
    var master = replTest.getMaster();

    // Calling getMaster also makes available the liveNodes structure,
    // which looks like this:
    // liveNodes = {master: masterNode,
    //              slaves: [slave1, slave2]
    //             }
    printjson(replTest.liveNodes);

    // Here's how you save something to master
    master.getDB("foo").foo.save({a: 1000});

    // This method will check the oplogs of the master
    // and slaves in the set and wait until the change has replicated.
    replTest.awaitReplication();


    cppconn = new Mongo( replTest.getURL() ).getDB( "foo" );
    assert.eq( 1000 , cppconn.foo.findOne().a , "cppconn 1" );

    {
        // check c++ finding other servers
        var temp = replTest.getURL();
        temp = temp.substring( 0 , temp.lastIndexOf( "," ) );
        temp = new Mongo( temp ).getDB( "foo" );
        assert.eq( 1000 , temp.foo.findOne().a , "cppconn 1" );
    }


    // Here's how to stop the master node
    var master_id = replTest.getNodeId( master );
    replTest.stop( master_id );

    // Now let's see who the new master is:
    var new_master = replTest.getMaster();

    // Is the new master the same as the old master?
    var new_master_id = replTest.getNodeId( new_master );

    assert( master_id != new_master_id, "Old master shouldn't be equal to new master." );

    reconnect(cppconn);
    assert.eq( 1000 , cppconn.foo.findOne().a , "cppconn 2" );

    // Now let's write some documents to the new master
    for(var i=0; i<1000; i++) {
        new_master.getDB("bar").bar.save({a: i});
    }
    new_master.getDB("admin").runCommand({getlasterror: 1});

    // Here's how to restart the old master node:
    slave = replTest.restart(master_id);


    // Now, let's make sure that the old master comes up as a slave
    assert.soon(function() {
        var res = slave.getDB("admin").runCommand({ismaster: 1});
        printjson(res);
        return res['ok'] == 1 && res['ismaster'] == false;
    });

    // And we need to make sure that the replset comes back up
    assert.soon(function() {
        var res = new_master.getDB("admin").runCommand({replSetGetStatus: 1});
        printjson( res );
        return res.myState == 1;
    });

    // And that both slave nodes have all the updates
    new_master = replTest.getMaster();
    assert.eq( 1000 , new_master.getDB( "bar" ).runCommand( { count:"bar"} ).n , "assumption 2")
    replTest.awaitReplication();

    slaves = replTest.liveNodes.slaves;
    assert( slaves.length == 2, "Expected 2 slaves but length was " + slaves.length );
    slaves.forEach(function(slave) {
        slave.setSlaveOk();
        var count = slave.getDB("bar").runCommand({count: "bar"});
        printjson( count );
        assert.eq( 1000 , count.n , "slave count wrong: " + slave );
    });

    // last error
    master = replTest.getMaster();
    slaves = replTest.liveNodes.slaves;
    printjson(replTest.liveNodes);

    db = master.getDB("foo")
    t = db.foo

    ts = slaves.map( function(z){ z.setSlaveOk(); return z.getDB( "foo" ).foo; } )

    t.save({a: 1000});
    t.ensureIndex( { a : 1 } )

    result = db.runCommand({getLastError : 1, w: 3 , wtimeout :30000 })
    printjson(result);
    lastOp = result.lastOp;
    lastOplogOp = master.getDB("local").oplog.rs.find().sort({$natural : -1}).limit(1).next();
    assert.eq(lastOplogOp['ts'], lastOp);

    ts.forEach( function(z){ assert.eq( 2 , z.getIndexKeys().length , "A " + z.getMongo() ); } )

    t.reIndex()

    db.getLastError( 3 , 30000 )
    ts.forEach( function(z){ assert.eq( 2 , z.getIndexKeys().length , "A " + z.getMongo() ); } )

    // Shut down the set and finish the test.
    replTest.stopSet( signal );
}

doTest( 15 );
print("replset1.js SUCCESS");

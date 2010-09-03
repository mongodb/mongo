doTest = function( signal ) {

    // Test orphaned master steps down
    var replTest = new ReplSetTest( {name: 'testSet', nodes: 3} );

    replTest.startSet();
    replTest.initiate();

    var master = replTest.getMaster();

    // Kill both slaves, simulating a network partition
    var slaves = replTest.liveNodes.slaves;
    for(var i=0; i<slaves.length; i++) {
        var slave_id = replTest.getNodeId(slaves[i]);
        replTest.stop( slave_id );
    }

    var result = master.getDB("admin").runCommand({ismaster: 1});
    //printjson( result );
    assert.soon(function() {
        var result = master.getDB("admin").runCommand({ismaster: 1});
        //printjson( result );
        return (result['ok'] == 1 && result['ismaster'] == false);
    }, "Master fails to step down when orphaned.");

    replTest.stopSet( signal );
}

doTest( 15 );
print("replset4.js SUCCESS");

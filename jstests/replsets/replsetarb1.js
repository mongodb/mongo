// FAILING TEST
// no primary is ever elected if the first server is an arbiter

doTest = function( signal ) {

    var replTest = new ReplSetTest( {name: 'unicomplex', nodes: 3} );
    var nodes = replTest.nodeList();

    print(tojson(nodes));

    var conns = replTest.startSet();
    var r = replTest.initiate({"_id" : "unicomplex", 
                "members" : [
                             {"_id" : 0, "host" : nodes[0], "arbiterOnly" : true}, 
                             {"_id" : 1, "host" : nodes[1]}, 
                             {"_id" : 2, "host" : nodes[2]}]});

    // Make sure we have a master
    // Neither this
    var master = replTest.getMaster();

    // Make sure we have an arbiter
    // Nor this will succeed
    assert.soon(function() {
        res = conns[0].getDB("admin").runCommand({replSetGetStatus: 1});
        printjson(res);
        return res.myState == 7;
    }, "Aribiter failed to initialize.");

    replTest.stopSet( signal );
}

// doTest( 15 );

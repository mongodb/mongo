// FAILING TEST
// no primary is ever elected if the first server is an arbiter

doTest = function( signal ) {

    var replTest = new ReplSetTest( {name: 'testSet', nodes: 3} );
    var nodes = replTest.nodeList();

    replTest.startSet();
    replTest.node[0].initiate({"_id" : "unicomplex", 
                "members" : [
                             {"_id" : 0, "host" : nodes[0], "arbiterOnly" : true}, 
                             {"_id" : 1, "host" : nodes[1]}, 
                             {"_id" : 2, "host" : nodes[2]}]});

    // one of these should be master

    var m2 = replTest.nodes[1].runCommand({ismaster:1})
    var m3 = replTest.nodes[2].runCommand({ismaster:1})

    // but they aren't! FAIL
    assert(m2.ismaster || m3.ismaster, 'a master exists');

    replTest.stopSet( signal );
}

//doTest( 15 );

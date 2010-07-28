// FAILING TEST
// should check that election happens in priority order

doTest = function( signal ) {

    var replTest = new ReplSetTest( {name: 'testSet', nodes: 3} );
    var nodes = replTest.nodeList();

    replTest.startSet();
    replTest.node[0].initiate({"_id" : "unicomplex", 
                "members" : [
                             {"_id" : 0, "host" : nodes[0], "priority" : 1}, 
                             {"_id" : 1, "host" : nodes[1], "priority" : 2}, 
                             {"_id" : 2, "host" : nodes[2], "priority" : 3}]});

    sleep(10000);

    // 2 should be master
    var m3 = replTest.nodes[2].runCommand({ismaster:1})

    // FAILS: node[0] is elected master, regardless of priority
    assert(m3.ismaster, 'highest priority is master');

    // kill 2, 1 should take over
    var m3Id = replTest.getNodeId(nodes[2]);
    replTest.stop(m3Id);

    sleep(10000);

    var m2 = replTest.nodes[1].runCommand({ismaster:1})
    assert(m2.ismaster, 'node 2 is master');

    // bring 2 back up, nothing should happen
    replTest.start(m3Id);

    sleep(10000);

    m2 = replTest.nodes[1].runCommand({ismaster:1})
    assert(m2.ismaster, 'node 2 is still master');

    // kill 1, 2 should become master
    var m2Id = replTest.getNodeId(nodes[1]);
    replTest.stop(m2Id);

    sleep(10000);

    m3 = replTest.nodes[2].runCommand({ismaster:1})
    assert(m3.ismaster, 'node 3 is master');

    replTest.stopSet( signal );
}

//doTest( 15 );

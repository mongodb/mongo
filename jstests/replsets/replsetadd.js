
doTest = function( signal ) {
//    // Replica set testing API
//    // Create a new replica set test. Specify set name and the number of nodes you want.
//    var replTest = new ReplSetTest( {name: 'testSet', nodes: 2} );
//
//    // this returns a list of nodes
//    var nodes = replTest.startSet();
//
//    // This will wait for initiation
//    replTest.initiate();
//
//    // Call getMaster to return a reference to the node that's been
//    // elected master.
//    var master = replTest.getMaster();
//
//    // Start up a new node
//    replTest.add();
//
//    var nodeList = replTest.nodeList();
//    printjson( nodeList );
//    var newHost = nodeList[nodeList.length-1];
//
//    // Initiate new node
//    var result = master.getDB("admin").eval('rs.add(\'' + newHost + '\');');
//    printjson( result );
//
//    assert( result['ok'] == 1 );
}

doTest( 9 );

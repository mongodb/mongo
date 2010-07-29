
doTest = function( signal ) {
    // Test add node

    var replTest = new ReplSetTest( {name: 'testSet', nodes: 0} );

    var first = replTest.add();

    // Initiate replica set
    assert.soon(function() {
      var res = first.getDB("admin").runCommand({replSetInitiate: null});
      return res['ok'] == 1;
    });

    // Get status
    assert.soon(function() {
      var result = first.getDB("admin").runCommand({replSetGetStatus: true});
      return result['ok'] == 1;
    });

    // Start a second node
    var second = replTest.add();

    // Add the second node.
    // This runs the equivalent of rs.add(newNode);
    replTest.reInitiate();

    replTest.stopSet( signal );
}

doTest( 15 );

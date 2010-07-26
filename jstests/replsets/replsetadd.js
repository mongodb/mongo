
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
    // Results in the following exception:
    //m31000| Mon Jul 26 14:46:20 [conn1] replSet replSetReconfig all members seem up
    //m31000| Mon Jul 26 14:46:20 [conn1] replSet info saving a newer config version to local.system.replset
    //m31000| Mon Jul 26 14:46:20 [conn1]   Assertion failure !sp.state.primary() db/repl/rs.h 184
    //m31000| 0x10002dcde 0x100031d78 0x1001dc733 0x1001dce60 0x1001f03b4 0x1001f10eb 0x1002b37d4 0x1002b49aa 0x100181448 0x100184d72 0x10025bf93 0x10025f746 0x10033795d 0x100695404 0x7fff81429456 0x7fff81429309 
    replTest.reInitiate();

    replTest.stopSet( signal );
}

// doTest( 15 );

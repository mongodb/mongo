
doTest = function( signal ) {
    // Test add node

    var replTest = new ReplSetTest( {name: 'testSet', nodes: 0, host:"localhost"} );

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

    // try to change to hostnames (from localhost)
    var master = replTest.getMaster();
    var config = master.getDB("local").system.replset.findOne();
    config.version++;
    config.members.forEach(function(m) {
            m.host = m.host.replace("localhost", getHostName());
            print(m.host);
        });

    var result = master.getDB("admin").runCommand({replSetReconfig: config});
    assert.eq(result.ok, 0);
    assert.eq(result.assertionCode, 13645);

    replTest.stopSet( signal );
}

doTest( 15 );

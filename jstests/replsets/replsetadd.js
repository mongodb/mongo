
doTest = function( signal ) {
    // Test add node

    var replTest = new ReplSetTest( {name: 'testSet', nodes: 0, host:"localhost"} );

    var first = replTest.add();

    // Initiate replica set
    assert.soon(function() {
      var res = first.getDB("admin").runCommand({replSetInitiate: {
                  _id : 'testSet',
                  members : [{_id : 0, host : "localhost:"+replTest.ports[0]}]
              }
          });
      return res['ok'] == 1;
    });

    // Get status
    assert.soon(function() {
      var result = first.getDB("admin").runCommand({replSetGetStatus: true});
      return result['ok'] == 1;
    });

    replTest.getMaster();

    // Start a second node
    var second = replTest.add();

    // Add the second node.
    // This runs the equivalent of rs.add(newNode);
    print("calling add again");
    try {
        replTest.reInitiate();
    }
    catch(e) {
        print(e);
    }

    print("try to change to localhost to "+getHostName());
    var master = replTest.getMaster();
    
    var config = master.getDB("local").system.replset.findOne();
    config.version++;
    config.members.forEach(function(m) {
            m.host = m.host.replace("localhost", getHostName());
            print(m.host);
        });
    printjson(config);

    print("trying reconfig that shouldn't work");
    var result = master.getDB("admin").runCommand({replSetReconfig: config});
    assert.eq(result.ok, 0, tojson(result));
    assert.eq(result.code, 13645, tojson(result));

    replTest.stopSet( signal );
}

doTest( 15 );

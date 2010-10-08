doTest = function( signal ) {
    var name = 'config1';
  
    var replTest = new ReplSetTest( {name: name, nodes: 3} );
    var nodes = replTest.startSet();

    var config = replTest.getReplSetConfig();
    config.settings = {"heartbeatSleep" : .5, heartbeatTimeout : .8};
    
    replTest.initiate(config);

    // Call getMaster to return a reference to the node that's been
    // elected master.
    var master = replTest.getMaster();

    config = master.getDB("local").system.replset.findOne();
    assert.eq(config.settings.heartbeatSleep, .5);
    assert.eq(config.settings.heartbeatTimeout, .8);
};

doTest(15);

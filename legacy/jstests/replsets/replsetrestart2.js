// config saved on shutdown

var compare_configs = function(c1, c2) {
    assert.eq(c1.version, c2.version, 'version same');
    assert.eq(c1._id, c2._id, '_id same');

    printjson(c1);
    printjson(c2);

    for (var i in c1.members) {
        assert(c2.members[i] !== undefined, 'field '+i+' exists in both configs');
        assert.eq(c1.members[i]._id, c2.members[i]._id, 'id is equal in both configs');
        assert.eq(c1.members[i].host, c2.members[i].host, 'id is equal in both configs');
    }
}

doTest = function( signal ) {
    var replTest = new ReplSetTest( {name: 'testSet', nodes: 3} );
    replTest.startSet();

    sleep(5000);

    replTest.initiate();

    sleep(5000);

    var master = replTest.getMaster();
    var config = master.getDB("local").system.replset.findOne();

    replTest.stopSet( signal , true );

    replTest.restart(0);
    replTest.restart(1);
    replTest.restart(2);

    sleep(5000);

    master = replTest.getMaster();
    var config2 = master.getDB("local").system.replset.findOne();

    compare_configs(config, config2);

    replTest.stopSet( signal );
}

doTest( 15 );

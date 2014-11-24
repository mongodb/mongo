//
// Tests authorization when a config server is down
//

var st = new ShardingTest({ shards : 1,
                            mongos : 1,
                            verbose : 1,
                            keyFile : "jstests/libs/key1",
                            other : { separateConfig : true, sync : true } })

var mongos = st.s0
var configs = st._configServers

printjson( configs )
st.printShardingStatus()

assert.writeOK(mongos.getCollection( "foo.bar" ).insert({ hello : "world" }));

var stopOrder = [ 1, 0 ]

for( var i = 0; i < stopOrder.length; i++ ){

    var configToStop = configs[ stopOrder[i] ]

    jsTest.log( "Stopping config server " + stopOrder[i] + " : " + configToStop )

    MongoRunner.stopMongod( configToStop )

    jsTest.log( "Starting mongos with auth..." )

    var mongosWithAuth = MongoRunner.runMongos({ keyFile : "jstests/libs/key1",
                                                 configdb : mongos.savedOptions.configdb })
    var foodb = mongosWithAuth.getDB('foo');
    var res = foodb.bar.findOne();
    assert.neq(null, res, "Test FAILED: unable to find document using mongos with auth");
    assert.eq("world", res.hello);

    assert.throws( function() { foodb.createUser({user:'user' + i, pwd: 'pwd', roles: []}); } );
}

// Restart the config servers and make sure everything is consistent
for (var i = 0; i < stopOrder.length; i++ ) {

    var configToStart = configs[ stopOrder[i] ];

    jsTest.log( "Starting config server " + stopOrder[i] + " : " + configToStop );

    configToStart.restart = true;
    configs[stopOrder[i]] = MongoRunner.runMongod( configToStart );
}

assert.eq(0, mongos.getDB('foo').getUsers().length);
for (var i = 0; i < configs.length; i++) {
    assert.eq(0, configs[i].getDB('foo').getUsers().length);
}

jsTest.log( "DONE!" )

st.stop()


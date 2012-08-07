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

mongos.getCollection( "foo.bar" ).insert({ hello : "world" })
assert.eq( null, mongos.getDB( "foo" ).getLastError() )

var stopOrder = [ 1, 0 ]

for( var i = 0; i < stopOrder.length; i++ ){
    
    var configToStop = configs[ stopOrder[i] ]
    
    jsTest.log( "Stopping config server " + stopOrder[i] + " : " + configToStop )
    
    MongoRunner.stopMongod( configToStop )
    
    jsTest.log( "Starting mongos with auth..." )
    
    var mongosWithAuth = MongoRunner.runMongos({ keyFile : "jstests/libs/key1",
                                                 configdb : mongos.savedOptions.configdb })
                                                 
    assert.neq( null, mongosWithAuth.getCollection( "foo.bar" ).findOne() )
}

jsTest.log( "DONE!" )

st.stop()


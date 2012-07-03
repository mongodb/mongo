//
// Tests whether or not multi-version mongos/mongod instances can be launched
//

var verifyVersion = function( mongo, version ){
    
    var result = mongo.getDB( "admin" ).runCommand({ serverStatus : 1 })
    
    if( result.version != version ) printjson( result  )
    
    assert.eq( result.version, version )
}

var versionsToCheck = [ "1.8.5",
                        "2.0.6" ]

for( var i = 0; i < versionsToCheck.length; i++ ){

    var version = versionsToCheck[ i ]
    
    var mongod = MongoRunner.runMongod({ binVersion : version })
    var mongos = MongoRunner.runMongos({ binVersion : version, configdb : mongod })

    // Make sure the started versions are actually the correct versions
    verifyVersion( mongod, version )
    verifyVersion( mongos, version )

    MongoRunner.stopMongos( mongos )
    MongoRunner.stopMongod( mongod )
}
    
jsTest.log( "Done!" )

//
// End
//

//
// Tests whether or not multi-version mongos/mongod instances can be launched
//

load('./jstests/multiVersion/libs/verify_versions.js');

// Check our oldest and newest versions
var versionsToCheck = [ "oldest-supported",
                        "latest"];

for( var i = 0; i < versionsToCheck.length; i++ ){

    var version = versionsToCheck[ i ]
    
    var mongod = MongoRunner.runMongod({ binVersion : version })
    var mongos = MongoRunner.runMongos({ binVersion : version, configdb : mongod })

    // Make sure the started versions are actually the correct versions
    assert.binVersion( mongod, version );
    assert.binVersion( mongos, version );

    MongoRunner.stopMongos( mongos )
    MongoRunner.stopMongod( mongod )
}
    
jsTest.log( "Done!" )

//
// End
//

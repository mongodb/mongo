//
// Tests whether or not multi-version mongos/mongod instances can be launched
//

load('./jstests/multiVersion/libs/verify_versions.js');

// Check our oldest and newest versions
var versionsToCheck = [ "oldest-supported",
                        "latest"];

for( var i = 0; i < versionsToCheck.length; i++ ){

    var version = versionsToCheck[ i ]

    var mongod1 = MongoRunner.runMongod({ binVersion : version, configsvr : "" });
    var mongod2 = MongoRunner.runMongod({ binVersion : version, configsvr : "" });
    var mongod3 = MongoRunner.runMongod({ binVersion : version, configsvr : "" });
    var configdbStr = mongod1.host + "," + mongod2.host + "," + mongod3.host;
    var mongos = MongoRunner.runMongos({ binVersion : version, configdb : configdbStr });

    // Make sure the started versions are actually the correct versions
    assert.binVersion( mongod1, version );
    assert.binVersion( mongod2, version );
    assert.binVersion( mongod3, version );
    assert.binVersion( mongos, version );

    MongoRunner.stopMongos( mongos );
    MongoRunner.stopMongod( mongod1 );
    MongoRunner.stopMongod( mongod2 );
    MongoRunner.stopMongod( mongod3 );
}

jsTest.log( "Done!" )

//
// End
//

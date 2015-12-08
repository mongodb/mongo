/**
 * This is a self-test for the multiversion suite. It tests whether or not multi-version
 * mongos/mongod instances can be launched.
 */

load("./jstests/multiVersion/libs/verify_versions.js");

(function() {
    "use strict";

    var versionsToCheck = [
        "last-stable",
        "latest",
        "",
    ];

    versionsToCheck.forEach(function(version) {
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
    });
})();

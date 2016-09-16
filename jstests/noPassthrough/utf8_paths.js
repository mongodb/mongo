/**
 * Test that verifies mongod can start using paths that contain UTF-8 characters that are not ASCII.
 */
(function() {
    'use strict';
    var db_name = "ελληνικά";
    var path = MongoRunner.dataPath + "Росси́я";

    mkdir(path);

    // Test MongoD
    let testMongoD = function() {
        let conn = MongoRunner.runMongod({
            dbpath: path,
            useLogFiles: true,
            pidfilepath: path + "/pidfile",
            directoryperdb: "",
        });
        assert.neq(null, conn, 'mongod was unable to start up');

        let coll = conn.getCollection(db_name + ".foo");
        assert.writeOK(coll.insert({_id: 1}));

        MongoRunner.stopMongod(conn);
    };

    testMongoD();

    // Start a second time to test things like log rotation.
    testMongoD();
})();

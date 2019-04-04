/**
 * Test that verifies merizod can start using paths that contain UTF-8 characters that are not ASCII.
 */
(function() {
    'use strict';
    var db_name = "ελληνικά";
    var path = MerizoRunner.dataPath + "Росси́я";

    mkdir(path);

    // Test MerizoD
    let testMerizoD = function() {
        let options = {
            dbpath: path,
            useLogFiles: true,
            pidfilepath: path + "/pidfile",
        };

        // directoryperdb is only supported with the wiredTiger storage engine
        if (!jsTest.options().storageEngine || jsTest.options().storageEngine === "wiredTiger") {
            options["directoryperdb"] = "";
        }

        let conn = MerizoRunner.runMerizod(options);
        assert.neq(null, conn, 'merizod was unable to start up');

        let coll = conn.getCollection(db_name + ".foo");
        assert.writeOK(coll.insert({_id: 1}));

        MerizoRunner.stopMerizod(conn);
    };

    testMerizoD();

    // Start a second time to test things like log rotation.
    testMerizoD();
})();

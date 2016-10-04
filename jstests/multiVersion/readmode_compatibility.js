// Ensure that the latest version of the shell (with no particular readMode set) can issue find
// operations against a 3.0 server. The shell should read the wire version and fall back to "legacy"
// readMode.
(function() {
    'use strict';

    var storageEngine = jsTest.options().storageEngine;

    var conn30 = MongoRunner.runMongod({binVersion: '3.0', storageEngine: storageEngine});
    assert.neq(conn30, null, 'unable to start 3.0 mongod');

    // Force writeMode to "commands" so that we can check the results of write operations.
    conn30.forceWriteMode('commands');

    // Forcing the readMode to "compatibility" and then asking for the readMode should cause the
    // shell to resolve the readMode to "legacy".
    conn30.forceReadMode('compatibility');
    assert.eq('legacy', conn30.readMode());

    var testDB = conn30.getDB('test');
    var coll = testDB.readmode_compatibility;
    coll.drop();

    for (var i = 0; i < 5; i++) {
        assert.writeOK(coll.insert({_id: i}));
    }

    // Use a batchSize of 2 to ensure that we exercise both find and getMore.
    conn30.forceReadMode('compatibility');
    assert.eq(5, coll.find().batchSize(2).itcount());
    assert.eq('legacy', conn30._readMode);

    MongoRunner.stopMongod(conn30);

    // With the latest version of mongod, forcing the readMode to "compatibility" and then asking
    // for the readMode should cause the shell to resolve the readMode to "commands".
    var connLatest = MongoRunner.runMongod({storageEngine: storageEngine});
    assert.neq(connLatest, null, 'unable to start 3.2 mongod');
    connLatest.forceReadMode('compatibility');
    assert.eq('commands', connLatest.readMode());
})();

/**
 * Tests that a null embedded malformed string is rejected gracefully.
 */
(function() {
    'use strict';

    var engine = 'wiredTiger';
    if (jsTest.options().storageEngine) {
        engine = jsTest.options().storageEngine;
    }

    // Skip this test if not running with the right storage engine.
    if (engine !== 'wiredTiger' && engine !== 'inMemory') {
        jsTest.log('Skipping test because storageEngine is not "wiredTiger" or "inMemory"');
        return;
    }

    // Build an array of malformed strings to test
    var malformedStrings = ["\u0000000", "\0,", "bl\0ah", "split_pct=30,\0split_pct=35,"];

    // Start up a mongod.
    // Test that collection and index creation with malformed creation strings fail gracefully.
    runTest();

    function runTest() {
        var dbpath = MongoRunner.dataPath + 'wt_malformed_creation_string';
        resetDbpath(dbpath);

        // Start a mongod
        var conn = MongoRunner.runMongod({
            dbpath: dbpath,
            noCleanData: true,
        });
        assert.neq(null, conn, 'mongod was unable to start up');

        var testDB = conn.getDB('test');

        // Collection creation with malformed string should fail
        for (var i = 0; i < malformedStrings.length; i++) {
            assert.commandFailedWithCode(
                testDB.createCollection(
                    'coll', {storageEngine: {[engine]: {configString: malformedStrings[i]}}}),
                ErrorCodes.FailedToParse);
        }

        // Create collection to test index creation on
        assert.commandWorked(testDB.createCollection('coll'));

        // Index creation with malformed string should fail
        for (var i = 0; i < malformedStrings.length; i++) {
            assert.commandFailedWithCode(testDB.coll.createIndex({a: 1}, {
                name: 'with_malformed_str',
                storageEngine: {[engine]: {configString: malformedStrings[i]}}
            }),
                                         ErrorCodes.FailedToParse);
        }

        MongoRunner.stopMongod(conn);
    }
})();

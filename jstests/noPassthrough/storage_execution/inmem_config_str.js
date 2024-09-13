// SERVER-28179 Test the startup of in-memory storage engine using --inMemoryEngineConfigString
import {ReplSetTest} from "jstests/libs/replsettest.js";

if (jsTest.options().storageEngine !== "inMemory") {
    jsTestLog("Skipping test because storageEngine is not inMemory");
    quit();
}

var mongod = MongoRunner.runMongod({
    storageEngine: 'inMemory',
    inMemoryEngineConfigString: 'eviction=(threads_min=1)',
});
assert.neq(null, mongod, "mongod failed to started up with --inMemoryEngineConfigString");

MongoRunner.stopMongod(mongod);

// When creating new collections, we should always check the in-memory config strings
// regardless of the current storage engine.
const rst = new ReplSetTest({
    nodes: [
        {
            storageEngine: 'wiredTiger',
        },
        {
            // A secondary configured with the wiredTiger storage
            // engine should ignore settings for the in-memory
            // storage engine.
            storageEngine: 'wiredTiger',
            // Disallow elections on the secondary.
            rsConfig: {
                priority: 0,
                votes: 0,
            },
        },
        {
            storageEngine: 'inMemory',
            // Disallow elections on the secondary.
            rsConfig: {
                priority: 0,
                votes: 0,
            },
        },
    ],
});
rst.startSet();
rst.initiate();

let primary = rst.getPrimary();
let testDB = primary.getDB('test');

// Basic error checking on storage engine collection options.
// When the current storage engine is not 'inMemory', these checks will
// be handled by the storage engine factory on the primary.
// See InMemoryFactory.
assert.commandFailedWithCode(
    testDB.createCollection('test_bad_storage_options_coll',
                            {storageEngine: {inMemory: {notConfigStringField: 12345}}}),
    ErrorCodes.InvalidOptions);
assert.commandFailedWithCode(testDB.createCollection('test_bad_storage_options_idx', {
    indexOptionDefaults: {storageEngine: {inMemory: {notConfigStringField: 12345}}}
}),
                             ErrorCodes.InvalidOptions);

// A collection created with in-memory specific storage options should not cause issues for
// a secondary running the wiredTiger storage engine.
assert.commandWorked(testDB.createCollection('test_inmem_storage_options_coll', {
    storageEngine: {inMemory: {configString: 'split_pct=88'}},
    indexOptionDefaults: {storageEngine: {inMemory: {configString: 'split_pct=77'}}}
}));
rst.awaitReplication();

rst.stopSet();

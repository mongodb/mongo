/**
 * Tests that the expired data is deleted correctly after an index is converted to TTL using the
 * collMod command.
 *
 * @tags: [
 *   assumes_no_implicit_collection_creation_after_drop,
 *   multiversion_incompatible,
 *   requires_ttl_index,
 * ]
 */
(function() {
"use strict";

// Runs TTL monitor constantly to speed up this test.
const conn = MongoRunner.runMongod({setParameter: 'ttlMonitorSleepSecs=1'});

const dbName = jsTestName();
const testDB = conn.getDB(dbName);
const coll = testDB.getCollection('coll');
assert.commandWorked(testDB.createCollection(coll.getName()));
coll.createIndex({a: 1});
const expireAfterSeconds = 5;

const waitForTTL = () => {
    // The 'ttl.passes' metric is incremented when the TTL monitor starts processing the indexes, so
    // we wait for it to be incremented twice to know that the TTL monitor finished processing the
    // indexes at least once.
    const ttlPasses = testDB.serverStatus().metrics.ttl.passes;
    assert.soon(function() {
        return testDB.serverStatus().metrics.ttl.passes > ttlPasses + 1;
    });
};

// Inserts a measurement older than the TTL expiry. The data should be found.
const expired = new Date((new Date()).getTime() - (1000 * 10));
assert.commandWorked(coll.insert({a: expired}));

waitForTTL();
assert.eq(1, coll.find().itcount());

// Converts to a TTL index and checks the data is deleted.
assert.commandWorked(testDB.runCommand({
    collMod: coll.getName(),
    index: {
        keyPattern: {a: 1},
        expireAfterSeconds: expireAfterSeconds,
    }
}));

waitForTTL();
assert.eq(0, coll.find().itcount());

MongoRunner.stopMongod(conn);
})();

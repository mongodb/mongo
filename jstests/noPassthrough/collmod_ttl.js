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

load("jstests/libs/ttl_util.js");

// Runs TTL monitor constantly to speed up this test.
const conn = MongoRunner.runMongod({setParameter: 'ttlMonitorSleepSecs=1'});

const dbName = jsTestName();
const testDB = conn.getDB(dbName);
const coll = testDB.getCollection('coll');
assert.commandWorked(testDB.createCollection(coll.getName()));
coll.createIndex({a: 1});
const expireAfterSeconds = 5;

// Inserts a measurement older than the TTL expiry. The data should be found.
const expired = new Date((new Date()).getTime() - (1000 * 10));
assert.commandWorked(coll.insert({a: expired}));

TTLUtil.waitForPass(testDB);
assert.eq(1, coll.find().itcount());

// Converts to a TTL index and checks the data is deleted.
assert.commandWorked(testDB.runCommand({
    collMod: coll.getName(),
    index: {
        keyPattern: {a: 1},
        expireAfterSeconds: expireAfterSeconds,
    }
}));

TTLUtil.waitForPass(testDB);
assert.eq(0, coll.find().itcount());

MongoRunner.stopMongod(conn);
})();

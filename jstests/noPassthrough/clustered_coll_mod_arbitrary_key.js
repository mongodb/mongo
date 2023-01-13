/**
 * Test collMod command on a clustered collection.
 *
 * @tags: [
 *   requires_fcv_53,
 * ]
 */

(function() {
"use strict";

load("jstests/libs/clustered_collections/clustered_collection_util.js");
load("jstests/libs/ttl_util.js");

// Run TTL monitor constantly to speed up this test.
const conn = MongoRunner.runMongod(
    {setParameter: {ttlMonitorSleepSecs: 1, supportArbitraryClusterKeyIndex: true}});

function testCollMod(coll, clusterKey, clusterKeyName) {
    const collName = coll.getName();
    const clusterKeyFieldName = Object.keys(clusterKey)[0];
    // Set the original expireAfterSeconds to a day.
    const expireAfterSeconds = 60 * 60 * 24;

    assertDropCollection(coll.getDB(), collName);
    assert.commandWorked(coll.getDB().createCollection(
        coll.getName(), {clusteredIndex: {key: clusterKey, unique: true}, expireAfterSeconds}));

    // Insert documents less than a day old so they don't automatically expire.
    const batchSize = 10;
    const now = new Date();
    let docs = [];
    for (let i = 0; i < batchSize; i++) {
        // Make them 5 minutes expired.
        const fiveMinutesPastMS = 5 * 60 * 1000;
        const recentDate = new Date(now - fiveMinutesPastMS - i);
        docs.push({
            [clusterKeyFieldName]: recentDate,
            info: "unexpired",
        });
    }
    assert.commandWorked(coll.insertMany(docs, {ordered: false}));
    assert.eq(coll.find().itcount(), batchSize);

    TTLUtil.waitForPass(coll.getDB());
    assert.eq(coll.find().itcount(), batchSize);

    // Shorten the expireAfterSeconds so all the documents in the collection are expired.
    assert.commandWorked(coll.getDB().runCommand({collMod: collName, expireAfterSeconds: 1}));

    TTLUtil.waitForPass(coll.getDB());
    // Confirm all documents were deleted once the expireAfterSeconds was shortened.
    assert.eq(coll.find().itcount(), 0);

    // Turn TTL off.
    assert.commandWorked(coll.getDB().runCommand({collMod: collName, expireAfterSeconds: "off"}));

    // Ensure there is no outstanding TTL pass in progress that will still remove entries.
    TTLUtil.waitForPass(coll.getDB());

    assert.commandWorked(coll.insert({[clusterKeyFieldName]: now, info: "unexpired"}));

    TTLUtil.waitForPass(coll.getDB());

    assert.eq(coll.find().itcount(), 1);

    assert.commandFailedWithCode(
        coll.getDB().runCommand(
            {collMod: collName, index: {keyPattern: {[clusterKeyFieldName]: 1}, hidden: true}}),
        6011800);

    assert.commandFailedWithCode(
        coll.getDB().runCommand({collMod: collName, index: {name: clusterKeyName, hidden: true}}),
        6011800);
}

testCollMod(conn.getDB("local")["coll"], {ts: 1}, "ts_1");

MongoRunner.stopMongod(conn);
})();

/**
 * Checks that, when async oplog sampling enabled, we aren't blocking any important operations.
 *
 * This test forces sampling to occur very slowly useSlowCollectionTruncateMarkerScanning: true. One
 * oplog entry will be sampled per second. This means that we will sample at a minimum of 250
 * seconds, since this is our initial oplog size. We test that ftdc, DML, DDL, TTL, oplog
 * application operations are not affect by sampling ongoing asynchronously.

 * @tags: [requires_replication, requires_persistence]
 */
import {TTLUtil} from "jstests/libs/ttl_util.js";

function samplingIsIncomplete(primary) {
    const status = primary.getDB("local").serverStatus();
    assert.commandWorked(status);
    jsTestLog(status.oplogTruncation);
    return (
        !status.oplogTruncation.hasOwnProperty("processingMethod") ||
        status.oplogTruncation.processingMethod == "in progress"
    );
}

// Initialize a 2-node replica set with a slow oplog.
const rst = new ReplSetTest({
    nodes: 2,
    nodeOptions: {
        setParameter: {
            ttlMonitorSleepSecs: 1,
            "oplogSamplingAsyncEnabled": true,
            useSlowCollectionTruncateMarkerScanning: true,
        },
    },
});
rst.startSet();
rst.initiate();

// Insert initial documents
jsTestLog("Inserting initial set of documents into the collection.");
let coll = rst.getPrimary().getDB("test").getCollection("yeehaw");
for (let i = 0; i < 250; i++) {
    assert.commandWorked(coll.insert({a: i}));
}

// Stop and restart the replica set
rst.getPrimary().adminCommand({fsync: 1});
rst.stopSet(null, true);
jsTestLog("Replica set stopped for restart.");
rst.startSet(null, true);
jsTestLog("Replica set restarted.");

const primary = rst.getPrimary();
const primaryDb = primary.getDB("test");
let secondary = rst.getSecondary();

// Verify we're still sampling.
assert(samplingIsIncomplete(primary));
assert(samplingIsIncomplete(secondary));

// Check sampling does not block oplog application
jsTestLog("+++ 1");
{
    for (let i = 0; i < 5; i++) {
        assert.commandWorked(
            rst
                .getPrimary()
                .getDB("test")
                .getCollection("sheep")
                .insert({baa: i}, {writeConcern: {w: 2}}),
        );
    }
    rst.awaitReplication();
}
jsTestLog("+++ 2");

// Check sampling does not block TTL
{
    assert.commandWorked(primaryDb.createCollection("cows"));
    assert.commandWorked(primaryDb.cows.createIndex({"lastModifiedDate": 1}, {expireAfterSeconds: 0}));

    for (let i = 0; i < 5; i++) {
        assert.commandWorked(primaryDb.getCollection("cows").insert({"lastModifiedDate": new Date()}));
    }

    // TTL Monitor should now perform passes every second. A timeout here would mean we fail the
    // test.
    TTLUtil.waitForPass(primaryDb, true, 20 * 1000);

    assert.eq(primaryDb.cows.count(), 0, "We should get 0 documents after TTL monitor run");
}
jsTestLog("+++ 3");

// Check sampling does not block any DML or DDL operations
{
    assert.commandWorked(primaryDb.createCollection("pigs"));

    // Insert document
    assert.commandWorked(primaryDb.pigs.insertOne({name: "oink", age: 5}));
    // Update document
    assert.commandWorked(primaryDb.pigs.updateOne({name: "oink"}, {$set: {age: 6}}));
    // Count documents
    assert.eq(primaryDb.pigs.countDocuments({age: {$gte: 3}}), 1);
    // Delete document
    assert.commandWorked(primaryDb.pigs.deleteOne({name: "oink"}));

    assert(primaryDb.pigs.drop());
}

// Verify we're still sampling.
assert(samplingIsIncomplete(primary));
assert(samplingIsIncomplete(secondary));

rst.stopSet();

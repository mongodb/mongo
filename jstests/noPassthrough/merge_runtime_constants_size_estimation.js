/**
 * Test which verifies that $merge accounts for the size of runtime constants when it serializes
 * writes to send to other nodes.
 *
 *  This test creates a replica set, which requires journaling.
 *  @tags: [requires_replication, requires_journaling]
 */
(function() {
"use strict";

load('jstests/libs/fixture_helpers.js');  // For isReplSet().

// Test against a replica set.
const rst = new ReplSetTest({nodes: 2});
rst.startSet();
rst.initiate();
rst.awaitSecondaryNodes();

const conn = rst.getPrimary();
const dbName = "db";
const collName = "merge_let_params";
const dbCollName = dbName + "." + collName;
const outCollName = "outcoll";
const dbOutCollName = dbName + "." + outCollName;

const admin = conn.getDB("admin");
const coll = conn.getCollection(dbCollName);
const outColl = conn.getCollection(dbOutCollName);

coll.drop();
outColl.drop();

// Insert two large documents in both collections. By inserting the documents with the same _id
// values in both collections, this will guarantee that we need to serialize and send update
// command(s) across the wire when targeting the output collection.
const kOneMB = 1024 * 1024;
const kDataString = "a".repeat(4 * kOneMB);
const kDocs = [{_id: 2, data: kDataString}, {_id: -2, data: kDataString}];
assert.commandWorked(coll.insertMany(kDocs, {writeConcern: {w: "majority"}}));
assert.commandWorked(outColl.insertMany(kDocs, {writeConcern: {w: "majority"}}));

// The sizes of the different update command components are deliberately chosen to test the
// batching logic when the update is targeted to another node in the cluster. In particular, the
// update command will contain the 10MB 'largeScopeValue' in 'jsScope' and we will be updating two
// 4MB documents. The 18MB total exceeds the 16MB size limit, so we expect the batching logic to
// split the two documents into separate batches of 14MB each.
const outputValue = "aaaa";
const largeScopeValue = "a".repeat(10 * kOneMB);
let aggCommand = {
    pipeline: [{
        $merge: {
            into: {db: "db", coll: outCollName},
            on: "_id",
            whenMatched: [{$addFields: {out: outputValue}}],
            whenNotMatched: "insert"
        }
    }],
    cursor: {},
    runtimeConstants: {
        localNow: new Date(),
        clusterTime: new Timestamp(0, 0),
        jsScope: {"outField": largeScopeValue}
    },
};

// Given that we are testing against a replica set, we need to target a secondary node to force
// writes to go over the wire.
const aggColl = rst.getSecondary().getCollection(dbCollName);
aggCommand["$readPreference"] = {
    mode: "secondary"
};

// The aggregate should not fail.
assert.commandWorked(aggColl.runCommand("aggregate", aggCommand));

// Verify that each field in 'outColl' now has an 'out' field with a value of 'aaaa'.
function verifyResults() {
    const results = outColl.find().toArray();
    assert.gt(results.length, 0, results);
    for (const res of results) {
        assert(res.hasOwnProperty("out"), results);
        assert.eq(res["out"], outputValue, results);
    }
}

verifyResults();
assert(coll.drop());
assert(outColl.drop());

// Insert four large documents in both collections. As before, this will force updates to be
// sent across the wire, but this will generate double the batches.
const kMoreDocs = [
    {_id: -2, data: kDataString},
    {_id: -1, data: kDataString},
    {_id: 1, data: kDataString},
    {_id: 2, data: kDataString},
];

assert.commandWorked(coll.insertMany(kMoreDocs, {writeConcern: {w: "majority"}}));
assert.commandWorked(outColl.insertMany(kMoreDocs, {writeConcern: {w: "majority"}}));

// The aggregate should not fail.
assert.commandWorked(aggColl.runCommand("aggregate", aggCommand));

verifyResults();
assert(coll.drop());
assert(outColl.drop());

// If the documents and the runtime constants are large enough, the $merge is expected to fail.
const kVeryLargeDataString = "a".repeat(10 * kOneMB);
const kLargeDocs = [{_id: 2, data: kVeryLargeDataString}, {_id: -2, data: kVeryLargeDataString}];
assert.commandWorked(coll.insertMany(kLargeDocs, {writeConcern: {w: "majority"}}));
assert.commandWorked(outColl.insertMany(kLargeDocs, {writeConcern: {w: "majority"}}));
assert.commandFailedWithCode(aggColl.runCommand("aggregate", aggCommand),
                             ErrorCodes.BSONObjectTooLarge);

rst.stopSet();
})();

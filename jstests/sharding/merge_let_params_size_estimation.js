/**
 * Test which verifies that $merge accounts for the size of let parameters and runtime constants
 * when it serializes writes to send to other nodes.
 *
 * @tags: [
 *  # The $merge in this test targets the '_id' field, and requires a unique index.
 *  expects_explicit_underscore_id_index,
 * ]
 */
(function() {
"use strict";

load('jstests/libs/fixture_helpers.js');  // For isReplSet().

// Function to run the test against a test fixture. Accepts an object that contains the following
// fields:
// - testFixture: The fixture to run the test against.
// - conn: Connection to the test fixture specified above.
// - shardLocal and shardOutput: Indicates whether the local/output collection should be sharded in
//  this test run (ignored when not running against a sharded cluster).
function runTest({testFixture, conn, shardLocal, shardOutput}) {
    const dbName = "db";
    const collName = "merge_let_params";
    const dbCollName = dbName + "." + collName;
    const outCollName = "outcoll";
    const dbOutCollName = dbName + "." + outCollName;
    const admin = conn.getDB("admin");
    const isReplSet = FixtureHelpers.isReplSet(admin);

    function shardColls() {
        // When running against a sharded cluster, configure the collections according to
        // 'shardLocal' and 'shardOutput'.
        if (!isReplSet) {
            assert.commandWorked(admin.runCommand({enableSharding: dbName}));
            testFixture.ensurePrimaryShard(dbName, testFixture.shard0.shardName);
            if (shardLocal) {
                testFixture.shardColl(collName, {_id: 1}, {_id: 0}, {_id: 0}, dbName);
            }
            if (shardOutput) {
                testFixture.shardColl(outCollName, {_id: 1}, {_id: 0}, {_id: 0}, dbName);
            }
        }
    }
    const coll = conn.getCollection(dbCollName);
    const outColl = conn.getCollection(dbOutCollName);
    coll.drop();
    outColl.drop();
    shardColls();

    // Insert two large documents in both collections. By inserting the documents with the same _id
    // values in both collections and splitting these documents between chunks, this will guarantee
    // that we need to serialize and send update command(s) across the wire when targeting the
    // output collection.
    const kOneMB = 1024 * 1024;
    const kDataString = "a".repeat(4 * kOneMB);
    const kDocs = [{_id: 2, data: kDataString}, {_id: -2, data: kDataString}];
    assert.commandWorked(coll.insertMany(kDocs));
    assert.commandWorked(outColl.insertMany(kDocs));

    // The sizes of the different update command components are deliberately chosen to test the
    // batching logic when the update is targeted to another node in the cluster. In particular, the
    // update command will contain the 10MB 'outFieldValue' and we will be updating two 4MB
    // documents. The 18MB total exceeds the 16MB size limit, so we expect the batching logic to
    // split the two documents into separate batches of 14MB each.
    const outFieldValue = "a".repeat(10 * kOneMB);
    let aggCommand = {
        pipeline: [{
            $merge: {
                into: {db: "db", coll: outCollName},
                on: "_id",
                whenMatched: [{$addFields: {out: "$$outField"}}],
                whenNotMatched: "insert"
            }
        }],
        cursor: {},
        let : {"outField": outFieldValue}
    };

    // If this is a replica set, we need to target a secondary node to force writes to go over
    // the wire.
    const aggColl = isReplSet ? testFixture.getSecondary().getCollection(dbCollName) : coll;

    if (isReplSet) {
        aggCommand["$readPreference"] = {mode: "secondary"};
    }

    // The aggregate should not fail.
    assert.commandWorked(aggColl.runCommand("aggregate", aggCommand));

    // Verify that each document in the output collection contains the value of 'outField'.
    let outContents = outColl.find().toArray();
    for (const res of outContents) {
        const out = res["out"];
        assert.eq(out, outFieldValue, outContents);
    }

    assert(coll.drop());
    assert(outColl.drop());
    shardColls();

    // Insert four large documents in both collections. As before, this will force updates to be
    // sent across the wire, but this will generate double the batches.
    const kMoreDocs = [
        {_id: -2, data: kDataString},
        {_id: -1, data: kDataString},
        {_id: 1, data: kDataString},
        {_id: 2, data: kDataString},
    ];

    assert.commandWorked(coll.insertMany(kMoreDocs));
    assert.commandWorked(outColl.insertMany(kMoreDocs));

    // The aggregate should not fail.
    assert.commandWorked(aggColl.runCommand("aggregate", aggCommand));

    // Verify that each document in the output collection contains the value of 'outField'.
    outContents = outColl.find().toArray();
    for (const res of outContents) {
        const out = res["out"];
        assert.eq(out, outFieldValue, outContents);
    }

    assert(coll.drop());
    assert(outColl.drop());
    shardColls();

    // If the documents and the let parameters are large enough, the $merge is expected to fail.
    const kVeryLargeDataString = "a".repeat(10 * kOneMB);
    const kLargeDocs =
        [{_id: 2, data: kVeryLargeDataString}, {_id: -2, data: kVeryLargeDataString}];
    assert.commandWorked(coll.insertMany(kLargeDocs));
    assert.commandWorked(outColl.insertMany(kLargeDocs));
    assert.commandFailedWithCode(aggColl.runCommand("aggregate", aggCommand),
                                 ErrorCodes.BSONObjectTooLarge);
}

// Test against a replica set.
const rst = new ReplSetTest({nodes: 2});
rst.startSet();
rst.initiate();
rst.awaitSecondaryNodes();

runTest({testFixture: rst, conn: rst.getPrimary()});

rst.stopSet();

// Test against a sharded cluster.
const st = new ShardingTest({shards: 2, mongos: 1});
runTest({testFixture: st, conn: st.s0, shardLocal: false, shardOutput: false});
runTest({testFixture: st, conn: st.s0, shardLocal: true, shardOutput: false});
runTest({testFixture: st, conn: st.s0, shardLocal: false, shardOutput: true});
runTest({testFixture: st, conn: st.s0, shardLocal: true, shardOutput: true});

st.stop();
})();

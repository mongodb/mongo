/**
 * Tests 'changeStreams.largeEventsFailed' metric.
 */

(function() {
"use strict";

load("jstests/libs/fixture_helpers.js");           // For 'FixtureHelpers'.
load("jstests/libs/collection_drop_recreate.js");  // For 'assertDropAndRecreateCollection()'.

const testDB = db.getSiblingDB(jsTestName());
// Make sure the collection exists, because some validation might get skipped otherwise.
const testColl = assertDropAndRecreateCollection(testDB, "test");

// Compute the size for the large strings used in the subsequent tests.
const kLargeStringSize = (16 * 1024 * 1024) - bsonsize({_id: "aaa", a: "x"}) + 1;

// Insert two large documents into the test collection.
assert.commandWorked(testColl.insertMany([
    {_id: "aaa", a: "x".repeat(kLargeStringSize)},
    {_id: "bbb", a: "x".repeat(kLargeStringSize)}
]));

// For sharded passthrough suites with 2 or more shards, ensure the two inserted documents are on
// different shards.
if (FixtureHelpers.numberOfShardsForCollection(testColl) >= 2) {
    FixtureHelpers.getPrimaries(db).forEach((conn) => {
        assert.lte(conn.getDB(jsTestName()).getCollection(testColl.getName()).find().itcount(),
                   1,
                   "Unexpected document count on connection " + conn);
    });
}

function getChangeStreamMetricSum(metricName) {
    return FixtureHelpers
        .mapOnEachShardNode(
            {db: testDB, func: (db) => db.serverStatus().metrics.changeStreams[metricName]})
        .reduce((total, val) => total + val, 0);
}

// Enable pre- and post-images.
assert.commandWorked(testDB.runCommand(
    {collMod: testColl.getName(), changeStreamPreAndPostImages: {enabled: true}}));

// Open a change stream without pre- and post-images.
let csCursor = testColl.watch([]);

// Record a resume token marking the start point of the test.
const testStartToken = csCursor.getResumeToken();

// Perform ~16MB updates which generate ~16MB change events and ~16MB post-images.
assert.commandWorked(testColl.update({_id: "aaa"}, {$set: {a: "y".repeat(kLargeStringSize)}}));
assert.commandWorked(testColl.update({_id: "bbb"}, {$set: {a: "y".repeat(kLargeStringSize)}}));

{
    // Test that without pre- and post- images there is no error and the error-metric
    // 'changeStreams.largeEventsFailed' is not incremented.
    const oldChangeStreamsLargeEventsFailed = getChangeStreamMetricSum("largeEventsFailed");
    assert.soon(() => csCursor.hasNext());
    const fullEvent = csCursor.next();
    const newChangeStreamsLargeEventsFailed = getChangeStreamMetricSum("largeEventsFailed");
    assert.eq("aaa", fullEvent.documentKey._id);
    assert.eq(newChangeStreamsLargeEventsFailed, oldChangeStreamsLargeEventsFailed);
}

{
    // Test that for events which are over the size limit, $changeStreamSplitLargeEvent is
    // required. Additionally, test that 'changeStreams.largeEventsFailed' metric is counted
    // correctly.
    for (const postImageMode of ["required", "updateLookup"]) {
        const oldChangeStreamsLargeEventsFailed = getChangeStreamMetricSum("largeEventsFailed");

        const csCursor = testColl.watch([], {
            batchSize: 0,  // Ensure same behavior for replica sets and sharded clusters.
            fullDocument: postImageMode,
            fullDocumentBeforeChange: "required",
            resumeAfter: testStartToken
        });
        assert.throwsWithCode(() => assert.soon(() => csCursor.hasNext()),
                              ErrorCodes.BSONObjectTooLarge);

        const newChangeStreamsLargeEventsFailed = getChangeStreamMetricSum("largeEventsFailed");
        // We will hit the 'BSONObjectTooLarge' error once on each shard that encounters a large
        // change event document. The error will occur maximum on 2 shards, because we trigger only
        // 2 change events. The error might occur only on 1 shard when the collection is not sharded
        // or due to the timing of exceptions on sharded clusters.
        assert.contains(newChangeStreamsLargeEventsFailed - oldChangeStreamsLargeEventsFailed,
                        [1, 2]);
    }
}
}());

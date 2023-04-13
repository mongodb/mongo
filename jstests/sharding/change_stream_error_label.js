/**
 * Test that a change stream pipeline which encounters a retryable exception responds to the client
 * with an error object that includes the "ResumableChangeStreamError" label.
 * @tags: [
 *   requires_sharding,
 *   uses_change_streams,
 *    requires_persistence,
 * ]
 */
(function() {
"use strict";

// Skip cross-cluster consistency checks, since this test prematurely shuts down a shard.
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;
TestData.skipCheckingIndexesConsistentAcrossCluster = true;
TestData.skipCheckShardFilteringMetadata = true;

// Create a two-shard cluster so that we can stop one shard to test connection interruptions.
const st = new ShardingTest({
    shards: 2,
    mongos: 1,
    rs: {nodes: 1, setParameter: {writePeriodicNoops: true, periodicNoopIntervalSecs: 1}}
});

const testDB = st.s.getDB(jsTestName());
const coll = testDB.test;

// The set of errors which might be thrown if we attempt to getMore after stopping a shard.
const expectedStopShardErrors = [
    ErrorCodes.HostUnreachable,
    ErrorCodes.HostNotFound,
    ErrorCodes.NetworkTimeout,
    ErrorCodes.SocketException,
    ErrorCodes.ShutdownInProgress,
    ErrorCodes.PrimarySteppedDown,
    ErrorCodes.NotWritablePrimary,
    ErrorCodes.InterruptedAtShutdown,
    ErrorCodes.InterruptedDueToReplStateChange,
    ErrorCodes.NotPrimaryNoSecondaryOk,
    ErrorCodes.NotPrimaryOrSecondary
];

// First, verify that the 'failGetMoreAfterCursorCheckout' failpoint can effectively exercise the
// error label generation logic for change stream getMores.
function testFailGetMoreAfterCursorCheckoutFailpoint({mongos, errorCode, expectedLabel}) {
    errorCode = ErrorCodes.doMongosRewrite(mongos, errorCode);
    // Activate the failpoint and set the exception that it will throw.
    assert.commandWorked(testDB.adminCommand({
        configureFailPoint: "failGetMoreAfterCursorCheckout",
        mode: "alwaysOn",
        data: {"errorCode": errorCode}
    }));

    // Now open a valid $changeStream cursor...
    const aggCmdRes = assert.commandWorked(
        coll.runCommand("aggregate", {pipeline: [{$changeStream: {}}], cursor: {}}));

    // ... run a getMore using the cursorID from the original command response, and confirm that the
    // expected error was thrown...
    const getMoreRes = assert.commandFailedWithCode(
        testDB.runCommand({getMore: aggCmdRes.cursor.id, collection: coll.getName()}), errorCode);

    /// ... and confirm that the label is present or absent depending on the "expectedLabel" value.
    const errorLabels = (getMoreRes.errorLabels || []);
    assert.eq("errorLabels" in getMoreRes, expectedLabel, getMoreRes);
    assert.eq(errorLabels.includes("ResumableChangeStreamError"), expectedLabel, getMoreRes);

    // Finally, disable the failpoint.
    assert.commandWorked(
        testDB.adminCommand({configureFailPoint: "failGetMoreAfterCursorCheckout", mode: "off"}));
}
// Test the expected output for both resumable and non-resumable error codes.
testFailGetMoreAfterCursorCheckoutFailpoint(
    {mongos: st.s, errorCode: ErrorCodes.ShutdownInProgress, expectedLabel: true});
testFailGetMoreAfterCursorCheckoutFailpoint(
    {mongos: st.s, errorCode: ErrorCodes.FailedToParse, expectedLabel: false});

// Now test both aggregate and getMore under conditions of an actual cluster outage. Shard the
// collection on shard0, split at {_id: 0}, and move the upper chunk to the other shard.
assert.commandWorked(st.s.adminCommand({enableSharding: testDB.getName()}));
st.ensurePrimaryShard(testDB.getName(), st.shard0.shardName);
st.shardColl(coll, {_id: 1}, {_id: 0}, {_id: 0});

// Open a change stream on the collection...
const csCursor = coll.watch([]);

// ... insert some documents, ensuring that they are split across both shards...
for (let i = 1; i <= 10; ++i) {
    assert.commandWorked(coll.insert([{_id: -i}, {_id: i}]));
}

// ... and read all the corresponding events from the stream.
assert.soon(() => {
    return (csCursor.hasNext() && csCursor.next().documentKey._id === 10);
});

// Issue a "find" query to retrieve the first few documents, leaving the cursor open.
const findCursor = coll.find({}).sort({_id: 1}).batchSize(2);
assert.docEq({_id: -10}, findCursor.next());
assert.docEq({_id: -9}, findCursor.next());
assert.eq(findCursor.objsLeftInBatch(), 0);

// Open a non-$changeStream agg cursor. Set the batchSize to 0, since otherwise the aggregation will
// pull all documents from the shards at once and cache them on mongoS, meaning that the subsequent
// getMore will not attempt to contact a shard and will not throw the expected network exception.
const aggCursor = coll.aggregate([{$match: {}}, {$sort: {_id: 1}}], {cursor: {batchSize: 0}});

// Now stop shard0...
if (!TestData.catalogShard) {
    st.rs0.stopSet();
} else {
    // The config server is shard0 in catalog shard mode and we'll restart it later.
    st.rs0.stopSet(undefined, true /* forRestart */);
}

// ...  and confirm that getMore on the $changeStream throws one of the expected exceptions.
let err = assert.throws(() => assert.soon(() => csCursor.hasNext()));
assert.commandFailedWithCode(err, expectedStopShardErrors);

// Confirm that the response includes the "ResumableChangeStreamError" error label.
assert("errorLabels" in err, err);
assert.contains("ResumableChangeStreamError", err.errorLabels, err);

// Confirm that getMore on the find cursor throws the same exception...
err = assert.throws(() => assert.soon(() => findCursor.hasNext()));
assert.commandFailedWithCode(err, expectedStopShardErrors);

// ... but does NOT have the "ResumableChangeStreamError" error label.
assert(!("errorLabels" in err), err);

// Confirm that getMore on the non-$changeStream agg cursor throws the same exception...
err = assert.throws(() => assert.soon(() => aggCursor.hasNext()));
assert.commandFailedWithCode(err, expectedStopShardErrors);

// ... but does NOT have the "ResumableChangeStreamError" error label.
assert(!("errorLabels" in err), err);

// Now confirm that attempting to open a new stream fails on the initial aggregate.
err = assert.throws(() => coll.watch([]));
assert.commandFailedWithCode(err, ErrorCodes.FailedToSatisfyReadPreference);

// Confirm that the response includes the "ResumableChangeStreamError" error label.
assert("errorLabels" in err, err);
assert.contains("ResumableChangeStreamError", err.errorLabels, err);

// Attempting to issue a non-$changeStream aggregation also fails...
err = assert.throws(() => coll.aggregate([{$match: {}}, {$sort: {_id: 1}}]).itcount());
assert.commandFailedWithCode(err, ErrorCodes.FailedToSatisfyReadPreference);

//... but does NOT include the "ResumableChangeStreamError" error label.
assert(!("errorLabels" in err), err);

// Attempting to issue a new "find" query also fails...
err = assert.throws(() => coll.find({}).sort({_id: 1}).itcount());
assert.commandFailedWithCode(err, ErrorCodes.FailedToSatisfyReadPreference);

// ... but does NOT include the "ResumableChangeStreamError" error label.
assert(!("errorLabels" in err), err);

if (TestData.catalogShard) {
    // shard0 is the config server and it needs to be up for ShardingTest shutdown.
    st.rs0.startSet(undefined, true /* forRestart */);
}
st.stop();
}());

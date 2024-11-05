/**
 * Tests that a change event which exceeds the 16MB limit will be split into multiple fragments.
 */

import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

const testDB = db.getSiblingDB(jsTestName());
// Make sure the collection exists, because some validation might get skipped otherwise.
const testColl = assertDropAndRecreateCollection(testDB, "test");

{
    // Test that $changeStreamSplitLargeEvent cannot be used in a non-$changeStream pipeline.
    assert.commandFailedWithCode(testDB.runCommand({
        aggregate: testColl.getName(),
        pipeline: [{$changeStreamSplitLargeEvent: {}}],
        cursor: {}
    }),
                                 ErrorCodes.IllegalOperation);
}

{
    // Test that $changeStreamSplitLargeEvent can only be used once in the pipeline.
    assert.commandFailedWithCode(testDB.runCommand({
        aggregate: testColl.getName(),
        pipeline: [
            {$changeStream: {}},
            {$changeStreamSplitLargeEvent: {}},
            {$changeStreamSplitLargeEvent: {}}
        ],
        cursor: {}
    }),
                                 7182802);
}

{
    // Test that $changeStreamSplitLargeEvent can only be the last stage in the pipeline.
    assert.commandFailedWithCode(testDB.runCommand({
        aggregate: testColl.getName(),
        pipeline: [
            {$changeStream: {}},
            {$changeStreamSplitLargeEvent: {}},
            {$project: {fullDocument: 0}}
        ],
        cursor: {}
    }),
                                 7182802);
}

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

const decodedToken = decodeResumeToken(testStartToken);
assert.eq(decodedToken.tokenType, highWaterMarkResumeTokenType);
assert.eq(decodedToken.version, 2);
assert.eq(decodedToken.txnOpIndex, 0);
assert.eq(decodedToken.tokenType, 0);
assert.eq(decodedToken.fromInvalidate, false);
assert.gt(decodedToken.clusterTime, new Timestamp(0, 0));
assert.eq(decodedToken.uuid, undefined);
assert.eq(decodedToken.fragmentNum, undefined);

// Perform ~16MB updates which generate ~16MB change events and ~16MB post-images.
assert.commandWorked(testColl.update({_id: "aaa"}, {$set: {a: "y".repeat(kLargeStringSize)}}));
assert.commandWorked(testColl.update({_id: "bbb"}, {$set: {a: "y".repeat(kLargeStringSize)}}));

{
    // Test that without pre- and post- images the $changeStreamSplitLargeEvent stage is not
    // required.
    assert.soon(() => csCursor.hasNext());
    const fullEvent = csCursor.next();
    assert.eq("aaa", fullEvent.documentKey._id);
    assert(!fullEvent.splitEvent);
}

{
    // Test that for events which are not over the size limit, $changeStreamSplitLargeEvent does not
    // change anything.
    const csCursor =
        testColl.watch([{$changeStreamSplitLargeEvent: {}}], {resumeAfter: testStartToken});
    assert.soon(() => csCursor.hasNext());
    const fullEvent = csCursor.next();
    assert.eq("aaa", fullEvent.documentKey._id);
    assert(!fullEvent.splitEvent);
}

/**
 * Helper function to reconstruct the fragments of a split event into the original event. The
 * fragments are expected to be the next 'expectedFragmentCount' events retrieved from the cursor.
 * Also returns an array containing the resume tokens for each fragment.
 */
function reconstructSplitEvent(cursor, expectedFragmentCount) {
    let event = {}, resumeTokens = [];

    for (let fragmentNumber = 1; fragmentNumber <= expectedFragmentCount; ++fragmentNumber) {
        assert.soon(() => cursor.hasNext());
        const fragment = cursor.next();
        assert.docEq({fragment: fragmentNumber, of: expectedFragmentCount}, fragment.splitEvent);
        Object.assign(event, fragment);
        resumeTokens.push(fragment._id);
        delete event.splitEvent;
        delete event._id;
    }

    return [event, resumeTokens];
}

// Helper function to validate the reconstructed event.
function validateReconstructedEvent(event, expectedId) {
    assert.eq("update", event.operationType);
    assert.eq(expectedId, event.documentKey._id);
    assert.eq(expectedId, event.fullDocument._id);
    assert.eq(kLargeStringSize, event.fullDocument.a.length);
    assert.eq(expectedId, event.fullDocumentBeforeChange._id);
    assert.eq(kLargeStringSize, event.fullDocumentBeforeChange.a.length);
    assert.eq(kLargeStringSize, event.updateDescription.updatedFields.a.length);
}

// Helper function to validate a collection of resume tokens.
function validateResumeTokens(resumeTokens, numFragments) {
    resumeTokens.forEach((resumeToken, idx) => {
        const decodedToken = decodeResumeToken(resumeToken);
        const eventIdentifier = {"operationType": "update", "documentKey": {"_id": "aaa"}};
        assert.eq(decodedToken.eventIdentifier, eventIdentifier);
        assert.eq(decodedToken.tokenType, eventResumeTokenType);
        assert.eq(decodedToken.version, 2);
        assert.eq(decodedToken.txnOpIndex, 0);
        assert.eq(decodedToken.tokenType, 128);
        assert.eq(decodedToken.fromInvalidate, false);
        assert.gt(decodedToken.clusterTime, new Timestamp(0, 0));
        assert.eq(decodedToken.fragmentNum, idx);
        assert.neq(decodedToken.uuid, undefined);
    });
}

// We declare 'resumeTokens' array outside of the for-scope to collect and share resume tokens
// across several test-cases.
let resumeTokens = [];

for (const postImageMode of ["required", "updateLookup"]) {
    {
        // Test that for events which are over the size limit, $changeStreamSplitLargeEvent is
        // required. Additionally, test that 'changeStreams.largeEventsFailed' metric is counted
        // correctly.

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

    {
        // Test that oversized events are split into fragments and can be reassembled to form the
        // original event, and that the largeEventSplit metric counter is correctly incremented.

        const csCursor = testColl.watch(
            [{$changeStreamSplitLargeEvent: {}}],
            {
                batchSize: 0,  // Ensure same behavior for replica sets and sharded clusters.
                fullDocument: postImageMode,
                fullDocumentBeforeChange: "required",
                resumeAfter: testStartToken
            });

        const oldChangeStreamsLargeEventsSplit = getChangeStreamMetricSum("largeEventSplit");

        var reconstructedEvent;
        [reconstructedEvent, resumeTokens] = reconstructSplitEvent(csCursor, 3);
        validateReconstructedEvent(reconstructedEvent, "aaa");
        validateResumeTokens(resumeTokens, 3);

        const [reconstructedEvent2, _] = reconstructSplitEvent(csCursor, 3);
        validateReconstructedEvent(reconstructedEvent2, "bbb");

        const newChangeStreamsLargeEventsSplit = getChangeStreamMetricSum("largeEventSplit");
        assert.eq(oldChangeStreamsLargeEventsSplit + 2, newChangeStreamsLargeEventsSplit);
    }

    {
        // Test that we can filter on fields that sum to more than 16MB without throwing. Note that
        // we construct this $match as an $or of the three large fields so that pipeline
        // optimization cannot split this $match into multiple predicates and scatter them through
        // the pipeline.
        const csCursor = testColl.watch(
            [
                {
                    $match: {
                        $or: [
                            {"fullDocument": {$exists: true}},
                            {"fullDocumentBeforeChange": {$exists: true}},
                            {"updateDescription": {$exists: true}}
                        ]
                    }
                },
                {$changeStreamSplitLargeEvent: {}}
            ],
            {
                fullDocument: postImageMode,
                fullDocumentBeforeChange: "required",
                resumeAfter: testStartToken
            });
        assert.docEq(resumeTokens, reconstructSplitEvent(csCursor, 3)[1]);
        validateResumeTokens(resumeTokens, 3);
    }

    {
        // Resume the stream from the second-last fragment and test that we see only the last
        // fragment.
        const csCursor = testColl.watch([{$changeStreamSplitLargeEvent: {}}], {
            fullDocument: postImageMode,
            fullDocumentBeforeChange: "required",
            resumeAfter: resumeTokens[resumeTokens.length - 2]
        });
        assert.soon(() => csCursor.hasNext());
        const resumedEvent = csCursor.next();
        assert.eq(resumedEvent.updateDescription.updatedFields.a.length, kLargeStringSize);
        assert.docEq({fragment: resumeTokens.length, of: resumeTokens.length},
                     resumedEvent.splitEvent);
    }

    {
        // Test that projecting out one of the large fields in the resumed pipeline changes the
        // split such that the resume point won't be generated, and we therefore throw an exception.
        const csCursor = testColl.watch(
            [{$project: {"fullDocument.a": 0}}, {$changeStreamSplitLargeEvent: {}}], {
                batchSize: 0,  // Ensure same behavior for replica sets and sharded clusters.
                fullDocument: postImageMode,
                fullDocumentBeforeChange: "required",
                resumeAfter: resumeTokens[resumeTokens.length - 1]
            });
        assert.throwsWithCode(() => assert.soon(() => csCursor.hasNext()),
                              ErrorCodes.ChangeStreamFatalError);
    }
}

{
    // Test that inhibiting pipeline optimization will cause $changeStreamSplitLargeEvent to throw
    // if it cannot move to the correct position in the pipeline.
    assert.commandFailedWithCode(testDB.runCommand({
        aggregate: testColl.getName(),
        pipeline: [
            {$changeStream: {resumeAfter: resumeTokens[resumeTokens.length - 2]}},
            {$_internalInhibitOptimization: {}},
            {$changeStreamSplitLargeEvent: {}}
        ],
        cursor: {}
    }),
                                 7182803);
}

{
    // Test that resuming from a split event token without requesting pre- and post- images fails,
    // because the resulting event is too small to be split.
    const csCursor = testColl.watch([{$changeStreamSplitLargeEvent: {}}], {
        batchSize: 0,  // Ensure same behavior for replica sets and sharded clusters.
        resumeAfter: resumeTokens[resumeTokens.length - 1]
    });
    assert.throwsWithCode(() => assert.soon(() => csCursor.hasNext()),
                          ErrorCodes.ChangeStreamFatalError);
}

{
    // Test that resuming from split event without the $changeStreamSplitLargeEvent stage fails.
    assert.throwsWithCode(
        () => testColl.watch([], {resumeAfter: resumeTokens[resumeTokens.length - 2]}),
        ErrorCodes.ChangeStreamFatalError);
}

{
    // Get a resume token from a real event as opposed to a post-batch resume token.
    const csCursor1 = testColl.watch([]);
    assert.commandWorked(testColl.insertOne({_id: "ccc"}));
    assert.soon(() => csCursor1.hasNext());
    const eventResumeToken = csCursor1.next()._id;
    csCursor1.close();

    // Test that $changeStreamSplitLargeEvent works correctly in the presence of a $match stage that
    // cannot be pushed down (moved ahead of other stages).
    const csCursor2 = testColl.watch(
        [
            {
                // $jsonSchema expressions with nested properties belong to expression category
                // 'other' and therefore will block its $match stage from moving ahead of other
                // stages, unless those are the internal change stream stages allowed in the router
                // (mongoS) pipeline.
                // TODO SERVER-55492: Update the comment above when there are rename checks for
                // 'other' match expressions.
                $match:
                    {$jsonSchema: {properties: {fullDocument: {properties: {a: {type: "number"}}}}}}
            },
            {$changeStreamSplitLargeEvent: {}}
        ],
        {resumeAfter: eventResumeToken});

    // Assert the change stream pipeline works and can produce events.
    assert.commandWorked(testColl.insertOne({_id: "ddd", a: 42}));
    assert.soon(() => csCursor2.hasNext());
    const event = csCursor2.next();
    assert.eq("ddd", event.documentKey._id);
    assert.eq(42, event.fullDocument.a);

    csCursor2.close();
}

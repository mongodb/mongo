/**
 * Tests that a change event which exceeds the 16MB limit will be split into multiple fragments.
 * @tags: [requires_fcv_70]
 */

(function() {
"use strict";

load("jstests/libs/fixture_helpers.js");           // For 'FixtureHelpers'.
load("jstests/libs/collection_drop_recreate.js");  // For 'assertDropAndRecreateCollection()'.

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
if (FixtureHelpers.isMongos(db) && FixtureHelpers.isSharded(testColl)) {
    const primaries = FixtureHelpers.getPrimaries(db);
    if (primaries.length >= 2) {
        primaries.forEach((conn) => {
            assert.lte(1,
                       conn.getDB(jsTestName()).getCollection(testColl.getName()).find().itcount(),
                       "Unexpected document count on connection " + conn);
        });
    }
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

// Open a change stream with $changeStreamSplitLargeEvent and request both pre- and post-images.
csCursor = testColl.watch(
    [{$changeStreamSplitLargeEvent: {}}],
    {fullDocument: "required", fullDocumentBeforeChange: "required", resumeAfter: testStartToken});

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

const [reconstructedEvent, resumeTokens] = reconstructSplitEvent(csCursor, 3);
const fragmentCount = resumeTokens.length;
validateReconstructedEvent(reconstructedEvent, "aaa");

const [reconstructedEvent2, _] = reconstructSplitEvent(csCursor, 3);
validateReconstructedEvent(reconstructedEvent2, "bbb");

{
    // Test that we can filter on fields that sum to more than 16MB without throwing. Note that
    // we construct this $match as an $or of the three large fields so that pipeline optimization
    // cannot split this $match into multiple predicates and scatter them through the pipeline.
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
            fullDocument: "required",
            fullDocumentBeforeChange: "required",
            resumeAfter: testStartToken
        });
    assert.docEq(resumeTokens, reconstructSplitEvent(csCursor, 3)[1]);
}

{
    // Resume the stream from the second-last fragment and test that we see only the last fragment.
    const csCursor = testColl.watch([{$changeStreamSplitLargeEvent: {}}], {
        fullDocument: "required",
        fullDocumentBeforeChange: "required",
        resumeAfter: resumeTokens[fragmentCount - 2]
    });
    assert.soon(() => csCursor.hasNext());
    const resumedEvent = csCursor.next();
    assert.eq(resumedEvent.updateDescription.updatedFields.a.length, kLargeStringSize);
    assert.docEq({fragment: fragmentCount, of: fragmentCount}, resumedEvent.splitEvent);
}

{
    // Test that inhibiting pipeline optimization will cause $changeStreamSplitLargeEvent to throw
    // if it cannot move to the correct position in the pipeline.
    assert.commandFailedWithCode(testDB.runCommand({
        aggregate: testColl.getName(),
        pipeline: [
            {$changeStream: {resumeAfter: resumeTokens[fragmentCount - 2]}},
            {$_internalInhibitOptimization: {}},
            {$changeStreamSplitLargeEvent: {}}
        ],
        cursor: {}
    }),
                                 7182803);
}

{
    // Test that projecting out one of the large fields in the resumed pipeline changes the split
    // such that the resume point won't be generated, and we therefore throw an exception.
    const csCursor =
        testColl.watch([{$project: {"fullDocument.a": 0}}, {$changeStreamSplitLargeEvent: {}}], {
            batchSize: 0,  // Ensure same behavior for replica sets and sharded clusters.
            fullDocument: "required",
            fullDocumentBeforeChange: "required",
            resumeAfter: resumeTokens[fragmentCount - 1]
        });
    assert.throwsWithCode(() => assert.soon(() => csCursor.hasNext()),
                          ErrorCodes.ChangeStreamFatalError);
}
}());

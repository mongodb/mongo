/**
 * Tests that a change stream can take a user-specified collation, does not inherit the collection's
 * default collation, and uses the simple collation if none is provided.
 */
import {
    assertCreateCollection,
    assertDropAndRecreateCollection,
    assertDropCollection,
} from "jstests/libs/collection_drop_recreate.js";
import {
    assertChangeStreamEventEq,
    ChangeStreamTest,
    isChangeStreamPassthrough,
    runCommandChangeStreamPassthroughAware,
} from "jstests/libs/query/change_stream_util.js";

let cst = new ChangeStreamTest(db);

const caseInsensitive = {
    locale: "en_US",
    strength: 2,
};

let caseInsensitiveCollection = "change_stream_case_insensitive";
assertDropCollection(db, caseInsensitiveCollection);

// Test that you can open a change stream before the collection exists, and it will use the
// simple collation. Tag this stream as 'doNotModifyInPassthroughs', since only individual
// collections have the concept of a default collation.
const simpleCollationStream = cst.startWatchingChanges({
    pipeline: [
        {$changeStream: {}},
        {$match: {$or: [{"fullDocument._id": "INSERT_ONE"}, {"fullDocument._id": "INSERT_TWO"}]}},
        {$project: {docId: "$fullDocument._id"}},
    ],
    collection: caseInsensitiveCollection,
    doNotModifyInPassthroughs: true,
});

// Create the collection with a non-default collation. The stream should continue to use the
// simple collation.
caseInsensitiveCollection = assertCreateCollection(db, caseInsensitiveCollection, {collation: caseInsensitive});
assert.commandWorked(caseInsensitiveCollection.insert([{_id: "insert_one"}, {_id: "INSERT_TWO"}]));
cst.assertNextChangesEqual({cursor: simpleCollationStream, expectedChanges: [{docId: "INSERT_TWO"}]});

const caseInsensitivePipeline = [
    {$changeStream: {}},
    {$match: {"fullDocument.text": "abc"}},
    {$project: {docId: "$documentKey._id"}},
];

// Test that $changeStream will not implicitly adopt the default collation of the collection on
// which it is run. Tag this stream as 'doNotModifyInPassthroughs'; whole-db and cluster-wide
// streams do not have default collations.
const didNotInheritCollationStream = cst.startWatchingChanges({
    pipeline: caseInsensitivePipeline,
    collection: caseInsensitiveCollection,
    doNotModifyInPassthroughs: true,
});
// Test that a collation can be explicitly specified for the $changeStream. This does not need
// to be tagged 'doNotModifyInPassthroughs', since whole-db and cluster-wide changeStreams will
// use an explicit collation if present.
let explicitCaseInsensitiveStream = cst.startWatchingChanges({
    pipeline: caseInsensitivePipeline,
    collection: caseInsensitiveCollection,
    aggregateOptions: {collation: caseInsensitive},
});

assert.commandWorked(caseInsensitiveCollection.insert({_id: 0, text: "aBc"}));
assert.commandWorked(caseInsensitiveCollection.insert({_id: 1, text: "abc"}));

// 'didNotInheritCollationStream' should not have inherited the collection's case-insensitive
// default collation, and should only see the second insert.  'explicitCaseInsensitiveStream'
// should see both inserts.
cst.assertNextChangesEqual({cursor: didNotInheritCollationStream, expectedChanges: [{docId: 1}]});
cst.assertNextChangesEqual({cursor: explicitCaseInsensitiveStream, expectedChanges: [{docId: 0}, {docId: 1}]});

// Test that the collation does not apply to the scan over the oplog.
const similarNameCollection = assertDropAndRecreateCollection(db, "cHaNgE_sTrEaM_cAsE_iNsEnSiTiVe", {
    collation: {locale: "en_US"},
});

// We must recreate the explicitCaseInsensitiveStream and set 'doNotModifyInPassthroughs'. Whole
// db and cluster-wide streams use the simple collation while scanning the oplog, but they don't
// filter the oplog by collection name. The subsequent $match stage which we inject into the
// pipeline to filter for a specific collection will obey the pipeline's case-insensitive
// collation, meaning that 'cHaNgE_sTrEaM_cAsE_iNsEnSiTiVe' will match
// 'change_stream_case_insensitive'.
explicitCaseInsensitiveStream = cst.startWatchingChanges({
    pipeline: caseInsensitivePipeline,
    collection: caseInsensitiveCollection,
    aggregateOptions: {collation: caseInsensitive},
    doNotModifyInPassthroughs: true,
});

assert.commandWorked(similarNameCollection.insert({_id: 0, text: "aBc"}));
assert.commandWorked(caseInsensitiveCollection.insert({_id: 2, text: "ABC"}));

// The case-insensitive stream should not see the first insert (to the other collection), only
// the second. We do not expect to see the insert in 'didNotInheritCollationStream'.
cst.assertNextChangesEqual({cursor: explicitCaseInsensitiveStream, expectedChanges: [{docId: 2}]});

// Test that creating a collection without a collation does not invalidate any change streams
// that were opened before the collection existed.
(function () {
    let noCollationCollection = "change_stream_no_collation";
    assertDropCollection(db, noCollationCollection);

    const streamCreatedBeforeNoCollationCollection = cst.startWatchingChanges({
        pipeline: [{$changeStream: {}}, {$project: {docId: "$documentKey._id"}}],
        collection: noCollationCollection,
    });

    noCollationCollection = assertCreateCollection(db, noCollationCollection);
    assert.commandWorked(noCollationCollection.insert({_id: 0}));

    cst.assertNextChangesEqual({cursor: streamCreatedBeforeNoCollationCollection, expectedChanges: [{docId: 0}]});
})();

// Test that creating a collection and explicitly specifying the simple collation does not
// invalidate any change streams that were opened before the collection existed.
(function () {
    let simpleCollationCollection = "change_stream_simple_collation";
    assertDropCollection(db, simpleCollationCollection);

    const streamCreatedBeforeSimpleCollationCollection = cst.startWatchingChanges({
        pipeline: [{$changeStream: {}}, {$project: {docId: "$documentKey._id"}}],
        collection: simpleCollationCollection,
    });

    simpleCollationCollection = assertCreateCollection(db, simpleCollationCollection, {collation: {locale: "simple"}});
    assert.commandWorked(simpleCollationCollection.insert({_id: 0}));

    cst.assertNextChangesEqual({cursor: streamCreatedBeforeSimpleCollationCollection, expectedChanges: [{docId: 0}]});
})();

// Test that creating a change stream with a non-default collation, then creating a collection
// with the same collation will not invalidate the change stream.
(function () {
    let frenchCollection = "change_stream_french_collation";
    assertDropCollection(db, frenchCollection);

    const frenchChangeStream = cst.startWatchingChanges({
        pipeline: [{$changeStream: {}}, {$project: {docId: "$documentKey._id"}}],
        aggregateOptions: {collation: {locale: "fr"}},
        collection: frenchCollection,
    });

    frenchCollection = assertCreateCollection(db, frenchCollection, {collation: {locale: "fr"}});
    assert.commandWorked(frenchCollection.insert({_id: 0}));

    cst.assertNextChangesEqual({cursor: frenchChangeStream, expectedChanges: [{docId: 0}]});
})();

// Test that creating a change stream with a non-default collation, then creating a collection
// with *a different* collation will not invalidate the change stream.
(function () {
    let germanCollection = "change_stream_german_collation";
    assertDropCollection(db, germanCollection);

    const englishCaseInsensitiveStream = cst.startWatchingChanges({
        pipeline: [
            {$changeStream: {}},
            {$match: {"fullDocument.text": "abc"}},
            {$project: {docId: "$documentKey._id"}},
        ],
        aggregateOptions: {collation: caseInsensitive},
        collection: germanCollection,
    });

    germanCollection = assertCreateCollection(db, germanCollection, {collation: {locale: "de"}});
    assert.commandWorked(germanCollection.insert({_id: 0, text: "aBc"}));

    cst.assertNextChangesEqual({cursor: englishCaseInsensitiveStream, expectedChanges: [{docId: 0}]});
})();

// Test that creating a change stream with a non-default collation against a collection that has
// a non-simple default collation will use the collation specified on the operation.
(function () {
    const caseInsensitiveCollection = assertDropAndRecreateCollection(db, "change_stream_case_insensitive", {
        collation: caseInsensitive,
    });

    const englishCaseSensitiveStream = cst.startWatchingChanges({
        pipeline: [
            {$changeStream: {}},
            {$match: {"fullDocument.text": "abc"}},
            {$project: {docId: "$documentKey._id"}},
        ],
        aggregateOptions: {collation: {locale: "en_US"}},
        collection: caseInsensitiveCollection,
    });

    assert.commandWorked(caseInsensitiveCollection.insert({_id: 0, text: "aBc"}));
    assert.commandWorked(caseInsensitiveCollection.insert({_id: 1, text: "abc"}));

    cst.assertNextChangesEqual({cursor: englishCaseSensitiveStream, expectedChanges: [{docId: 1}]});
})();

// Test that collation is supported by the shell helper. Test that creating a change stream with
// a non-default collation against a collection that has a simple default collation will use the
// collation specified on the operation.
(function () {
    const noCollationCollection = assertDropAndRecreateCollection(db, "change_stream_no_collation");

    const cursor = noCollationCollection.watch(
        [{$match: {"fullDocument.text": "abc"}}, {$project: {docId: "$documentKey._id"}}],
        {collation: caseInsensitive},
    );
    assert(!cursor.hasNext());
    assert.commandWorked(noCollationCollection.insert({_id: 0, text: "aBc"}));
    assert.commandWorked(noCollationCollection.insert({_id: 1, text: "abc"}));
    assert.soon(() => cursor.hasNext());
    assertChangeStreamEventEq(cursor.next(), {docId: 0});
    assert.soon(() => cursor.hasNext());
    assertChangeStreamEventEq(cursor.next(), {docId: 1});
    assert(!cursor.hasNext());
})();

// Test that we can resume a change stream on a collection that has been dropped without
// requiring the user to explicitly specify the collation.
(function () {
    const collName = "change_stream_case_insensitive";
    let caseInsensitiveCollection = assertDropAndRecreateCollection(db, collName, {collation: caseInsensitive});

    let changeStream = caseInsensitiveCollection.watch([{$match: {"fullDocument.text": "abc"}}], {
        collation: caseInsensitive,
    });

    assert.commandWorked(caseInsensitiveCollection.insert({_id: 0, text: "abc"}));

    assert.soon(() => changeStream.hasNext());
    const next = changeStream.next();
    assert.docEq({_id: 0}, next.documentKey);
    const resumeToken = next._id;

    // Insert a second document to see after resuming.
    assert.commandWorked(caseInsensitiveCollection.insert({_id: "dropped_coll", text: "ABC"}));

    // Drop the collection to invalidate the stream.
    assertDropCollection(db, collName);

    // Test that a $changeStream is allowed to resume on the dropped collection with an explicit
    // collation, even if it doesn't match the original collection's default collation.
    changeStream = caseInsensitiveCollection.watch(
        [{$match: {$or: [{"_id": resumeToken}, {"fullDocument.text": "ABC"}]}}],
        {resumeAfter: resumeToken, collation: {locale: "simple"}},
    );

    assert.soon(() => changeStream.hasNext());
    assert.docEq({_id: "dropped_coll"}, changeStream.next().documentKey);

    // Test that a pipeline without an explicit collation is allowed to resume the change stream
    // after the collection has been dropped, and it will use the simple collation. Do not
    // modify this in the passthrough suite(s) since only individual collections have the
    // concept of a default collation.
    const doNotModifyInPassthroughs = true;
    const cmdRes = assert.commandWorked(
        runCommandChangeStreamPassthroughAware(
            db,
            {
                aggregate: collName,
                pipeline: [{$changeStream: {resumeAfter: resumeToken}}],
                cursor: {},
            },
            doNotModifyInPassthroughs,
        ),
    );

    changeStream = new DBCommandCursor(db, cmdRes);
    assert.soon(() => changeStream.hasNext());
    assert.docEq({_id: "dropped_coll"}, changeStream.next().documentKey);
})();

// Test that the default collation of a new version of the collection is not applied when
// resuming a change stream from before a collection drop.
(function () {
    const collName = "change_stream_case_insensitive";
    let caseInsensitiveCollection = assertDropAndRecreateCollection(db, collName, {collation: caseInsensitive});

    let changeStream = caseInsensitiveCollection.watch([{$match: {"fullDocument.text": "abc"}}], {
        collation: caseInsensitive,
    });

    assert.commandWorked(caseInsensitiveCollection.insert({_id: 0, text: "abc"}));

    assert.soon(() => changeStream.hasNext());
    const next = changeStream.next();
    assert.docEq({_id: 0}, next.documentKey);
    const resumeToken = next._id;

    // Insert a second document to see after resuming.
    assert.commandWorked(caseInsensitiveCollection.insert({_id: "dropped_coll", text: "ABC"}));

    // Recreate the collection with a different collation.
    caseInsensitiveCollection = assertDropAndRecreateCollection(db, caseInsensitiveCollection.getName(), {
        collation: {locale: "simple"},
    });
    assert.commandWorked(caseInsensitiveCollection.insert({_id: "new collection", text: "abc"}));

    // Verify that the stream sees the insert before the drop and then is exhausted. We won't
    // see the invalidate because the pipeline has a $match stage after the $changeStream.
    assert.soon(() => changeStream.hasNext());
    assert.docEq({_id: "dropped_coll", text: "ABC"}, changeStream.next().fullDocument);
    // Only single-collection streams will be exhausted from the drop. Use 'next()' instead of
    // 'isExhausted()' to force a getMore since the previous getMore may not include the
    // collection drop, which is more likely with sharded collections on slow machines.
    if (!isChangeStreamPassthrough()) {
        assert.throws(() => changeStream.next());
    }

    // Test that a pipeline with an explicit collation is allowed to resume from before the
    // collection is dropped and recreated.
    changeStream = caseInsensitiveCollection.watch(
        [{$match: {$or: [{"_id": resumeToken}, {"fullDocument.text": "ABC"}]}}],
        {resumeAfter: resumeToken, collation: {locale: "fr"}},
    );

    assert.soon(() => changeStream.hasNext());
    assert.docEq({_id: "dropped_coll"}, changeStream.next().documentKey);
    // Only single-collection streams will be exhausted from the drop. Use 'next()' instead of
    // 'isExhausted()' to force a getMore since the previous getMore may not include the
    // collection drop, which is more likely with sharded collections on slow machines.
    if (!isChangeStreamPassthrough()) {
        assert.throws(() => changeStream.next());
    }

    // Test that a pipeline without an explicit collation is allowed to resume, even though the
    // collection has been recreated with the same default collation as it had previously. Do
    // not modify this command in the passthrough suite(s) since only individual collections
    // have the concept of a default collation.
    const doNotModifyInPassthroughs = true;
    const cmdRes = assert.commandWorked(
        runCommandChangeStreamPassthroughAware(
            db,
            {aggregate: collName, pipeline: [{$changeStream: {resumeAfter: resumeToken}}], cursor: {}},
            doNotModifyInPassthroughs,
        ),
    );

    changeStream = new DBCommandCursor(db, cmdRes);
    assert.soon(() => changeStream.hasNext());
    assert.docEq({_id: "dropped_coll"}, changeStream.next().documentKey);
})();

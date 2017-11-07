/**
 * Tests that a change stream can use a user-specified, or collection-default collation.
 *
 * This test assumes that it will be able to drop and then re-create a collection with non-default
 * options.
 * @tags: [assumes_no_implicit_collection_creation_after_drop]
 */
(function() {
    "use strict";

    load("jstests/libs/change_stream_util.js");  // For 'ChangeStreamTest'.

    let cst = new ChangeStreamTest(db);

    const caseInsensitive = {locale: "en_US", strength: 2};
    const caseInsensitiveCollection = db.change_stream_case_insensitive;
    caseInsensitiveCollection.drop();

    // Test that you can open a change stream before the collection exists, and it will use the
    // simple collation.
    const simpleCollationStream = cst.startWatchingChanges(
        {pipeline: [{$changeStream: {}}], collection: caseInsensitiveCollection});

    // Create the collection with a non-default collation - this should invalidate the stream we
    // opened before it existed.
    assert.commandWorked(
        db.runCommand({create: caseInsensitiveCollection.getName(), collation: caseInsensitive}));
    cst.assertNextChangesEqual({
        cursor: simpleCollationStream,
        expectedChanges: [{operationType: "invalidate"}],
        expectInvalidate: true
    });

    const implicitCaseInsensitiveStream = cst.startWatchingChanges({
        pipeline: [
            {$changeStream: {}},
            {$match: {"fullDocument.text": "abc"}},
            // Be careful not to use _id in this projection, as startWatchingChanges() will exclude
            // it by default, assuming it is the resume token.
            {$project: {docId: "$documentKey._id"}}
        ],
        collection: caseInsensitiveCollection
    });
    const explicitCaseInsensitiveStream = cst.startWatchingChanges({
        pipeline: [
            {$changeStream: {}},
            {$match: {"fullDocument.text": "abc"}},
            {$project: {docId: "$documentKey._id"}}
        ],
        collection: caseInsensitiveCollection,
        aggregateOptions: {collation: caseInsensitive}
    });

    assert.writeOK(caseInsensitiveCollection.insert({_id: 0, text: "aBc"}));
    assert.writeOK(caseInsensitiveCollection.insert({_id: 1, text: "abc"}));

    cst.assertNextChangesEqual(
        {cursor: implicitCaseInsensitiveStream, expectedChanges: [{docId: 0}, {docId: 1}]});
    cst.assertNextChangesEqual(
        {cursor: explicitCaseInsensitiveStream, expectedChanges: [{docId: 0}, {docId: 1}]});

    // Test that the collation does not apply to the scan over the oplog.
    const similarNameCollection = db.cHaNgE_sTrEaM_cAsE_iNsEnSiTiVe;
    similarNameCollection.drop();
    assert.commandWorked(
        db.runCommand({create: similarNameCollection.getName(), collation: {locale: "en_US"}}));

    assert.writeOK(similarNameCollection.insert({_id: 0, text: "aBc"}));

    assert.writeOK(caseInsensitiveCollection.insert({_id: 2, text: "ABC"}));

    // The existing stream should not see the first insert (to the other collection), but should see
    // the second.
    cst.assertNextChangesEqual(
        {cursor: implicitCaseInsensitiveStream, expectedChanges: [{docId: 2}]});
    cst.assertNextChangesEqual(
        {cursor: explicitCaseInsensitiveStream, expectedChanges: [{docId: 2}]});

    // Test that creating a collection without a collation does not invalidate any change streams
    // that were opened before the collection existed.
    (function() {
        const noCollationCollection = db.change_stream_no_collation;
        noCollationCollection.drop();

        const streamCreatedBeforeNoCollationCollection = cst.startWatchingChanges({
            pipeline: [{$changeStream: {}}, {$project: {docId: "$documentKey._id"}}],
            collection: noCollationCollection
        });

        assert.commandWorked(db.runCommand({create: noCollationCollection.getName()}));
        assert.writeOK(noCollationCollection.insert({_id: 0}));

        cst.assertNextChangesEqual(
            {cursor: streamCreatedBeforeNoCollationCollection, expectedChanges: [{docId: 0}]});
    }());

    // Test that creating a collection and explicitly specifying the simple collation does not
    // invalidate any change streams that were opened before the collection existed.
    (function() {
        const simpleCollationCollection = db.change_stream_simple_collation;
        simpleCollationCollection.drop();

        const streamCreatedBeforeSimpleCollationCollection = cst.startWatchingChanges({
            pipeline: [{$changeStream: {}}, {$project: {docId: "$documentKey._id"}}],
            collection: simpleCollationCollection
        });

        assert.commandWorked(db.runCommand(
            {create: simpleCollationCollection.getName(), collation: {locale: "simple"}}));
        assert.writeOK(simpleCollationCollection.insert({_id: 0}));

        cst.assertNextChangesEqual(
            {cursor: streamCreatedBeforeSimpleCollationCollection, expectedChanges: [{docId: 0}]});
    }());

    // Test that creating a change stream with a non-default collation, then creating a collection
    // with the same collation will not invalidate the change stream.
    (function() {
        const frenchCollection = db.change_stream_french_collation;
        frenchCollection.drop();

        const frenchChangeStream = cst.startWatchingChanges({
            pipeline: [{$changeStream: {}}, {$project: {docId: "$documentKey._id"}}],
            aggregateOptions: {collation: {locale: "fr"}},
            collection: frenchCollection
        });

        assert.commandWorked(
            db.runCommand({create: frenchCollection.getName(), collation: {locale: "fr"}}));
        assert.writeOK(frenchCollection.insert({_id: 0}));

        cst.assertNextChangesEqual({cursor: frenchChangeStream, expectedChanges: [{docId: 0}]});
    }());

    // Test that creating a change stream with a non-default collation, then creating a collection
    // with *a different* collation will not invalidate the change stream.
    (function() {
        const germanCollection = db.change_stream_german_collation;
        germanCollection.drop();

        const englishCaseInsensitiveStream = cst.startWatchingChanges({
            pipeline: [
                {$changeStream: {}},
                {$match: {"fullDocument.text": "abc"}},
                {$project: {docId: "$documentKey._id"}}
            ],
            aggregateOptions: {collation: caseInsensitive},
            collection: germanCollection
        });

        assert.commandWorked(
            db.runCommand({create: germanCollection.getName(), collation: {locale: "de"}}));
        assert.writeOK(germanCollection.insert({_id: 0, text: "aBc"}));

        cst.assertNextChangesEqual(
            {cursor: englishCaseInsensitiveStream, expectedChanges: [{docId: 0}]});
    }());

    // Test that creating a change stream with a non-default collation against a collection that has
    // a non-simple default collation will use the collation specified on the operation.
    (function() {
        const caseInsensitiveCollection = db.change_stream_case_insensitive;
        caseInsensitiveCollection.drop();
        assert.commandWorked(db.runCommand(
            {create: caseInsensitiveCollection.getName(), collation: caseInsensitive}));

        const englishCaseSensitiveStream = cst.startWatchingChanges({
            pipeline: [
                {$changeStream: {}},
                {$match: {"fullDocument.text": "abc"}},
                {$project: {docId: "$documentKey._id"}}
            ],
            aggregateOptions: {collation: {locale: "en_US"}},
            collection: caseInsensitiveCollection
        });

        assert.writeOK(caseInsensitiveCollection.insert({_id: 0, text: "aBc"}));
        assert.writeOK(caseInsensitiveCollection.insert({_id: 1, text: "abc"}));

        cst.assertNextChangesEqual(
            {cursor: englishCaseSensitiveStream, expectedChanges: [{docId: 1}]});
    }());

    // Test that collation is supported by the shell helper.
    // Test that creating a change stream with a non-default collation against a collection that has
    // a simple default collation will use the collation specified on the operation.
    (function() {
        const noCollationCollection = db.change_stream_no_collation;
        noCollationCollection.drop();
        assert.commandWorked(db.runCommand({create: noCollationCollection.getName()}));

        const cursor = noCollationCollection.watch(
            [
              {$match: {"fullDocument.text": "abc"}},
              {$project: {docId: "$documentKey._id", _id: 0}}
            ],
            {collation: caseInsensitive});
        assert(!cursor.hasNext());
        assert.writeOK(noCollationCollection.insert({_id: 0, text: "aBc"}));
        assert.writeOK(noCollationCollection.insert({_id: 1, text: "abc"}));
        assert(cursor.hasNext());
        assert.docEq(cursor.next(), {docId: 0});
        assert(cursor.hasNext());
        assert.docEq(cursor.next(), {docId: 1});
        assert(!cursor.hasNext());
    }());

})();

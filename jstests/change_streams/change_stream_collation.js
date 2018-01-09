/**
 * Tests that a change stream can use a user-specified, or collection-default collation.
 */
(function() {
    "use strict";

    load("jstests/libs/collection_drop_recreate.js");  // For assert[Drop|Create]Collection.
    load("jstests/libs/change_stream_util.js");        // For 'ChangeStreamTest'.

    let cst = new ChangeStreamTest(db);

    const caseInsensitive = {locale: "en_US", strength: 2};

    // $changeStream cannot run on a non-existent database. Create an unrelated collection to ensure
    // that the database is present before testing.
    assertDropAndRecreateCollection(db, "change_stream_ensure_db_exists");

    let caseInsensitiveCollection = "change_stream_case_insensitive";
    assertDropCollection(db, caseInsensitiveCollection);

    // Test that you can open a change stream before the collection exists, and it will use the
    // simple collation.
    const simpleCollationStream = cst.startWatchingChanges(
        {pipeline: [{$changeStream: {}}], collection: caseInsensitiveCollection});

    // Create the collection with a non-default collation - this should invalidate the stream we
    // opened before it existed.
    caseInsensitiveCollection =
        assertCreateCollection(db, caseInsensitiveCollection, {collation: caseInsensitive});
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
    const similarNameCollection = assertDropAndRecreateCollection(
        db, "cHaNgE_sTrEaM_cAsE_iNsEnSiTiVe", {collation: {locale: "en_US"}});

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
        let noCollationCollection = "change_stream_no_collation";
        assertDropCollection(db, noCollationCollection);

        const streamCreatedBeforeNoCollationCollection = cst.startWatchingChanges({
            pipeline: [{$changeStream: {}}, {$project: {docId: "$documentKey._id"}}],
            collection: noCollationCollection
        });

        noCollationCollection = assertCreateCollection(db, noCollationCollection);
        assert.writeOK(noCollationCollection.insert({_id: 0}));

        cst.assertNextChangesEqual(
            {cursor: streamCreatedBeforeNoCollationCollection, expectedChanges: [{docId: 0}]});
    }());

    // Test that creating a collection and explicitly specifying the simple collation does not
    // invalidate any change streams that were opened before the collection existed.
    (function() {
        let simpleCollationCollection = "change_stream_simple_collation";
        assertDropCollection(db, simpleCollationCollection);

        const streamCreatedBeforeSimpleCollationCollection = cst.startWatchingChanges({
            pipeline: [{$changeStream: {}}, {$project: {docId: "$documentKey._id"}}],
            collection: simpleCollationCollection
        });

        simpleCollationCollection =
            assertCreateCollection(db, simpleCollationCollection, {collation: {locale: "simple"}});
        assert.writeOK(simpleCollationCollection.insert({_id: 0}));

        cst.assertNextChangesEqual(
            {cursor: streamCreatedBeforeSimpleCollationCollection, expectedChanges: [{docId: 0}]});
    }());

    // Test that creating a change stream with a non-default collation, then creating a collection
    // with the same collation will not invalidate the change stream.
    (function() {
        let frenchCollection = "change_stream_french_collation";
        assertDropCollection(db, frenchCollection);

        const frenchChangeStream = cst.startWatchingChanges({
            pipeline: [{$changeStream: {}}, {$project: {docId: "$documentKey._id"}}],
            aggregateOptions: {collation: {locale: "fr"}},
            collection: frenchCollection
        });

        frenchCollection =
            assertCreateCollection(db, frenchCollection, {collation: {locale: "fr"}});
        assert.writeOK(frenchCollection.insert({_id: 0}));

        cst.assertNextChangesEqual({cursor: frenchChangeStream, expectedChanges: [{docId: 0}]});
    }());

    // Test that creating a change stream with a non-default collation, then creating a collection
    // with *a different* collation will not invalidate the change stream.
    (function() {
        let germanCollection = "change_stream_german_collation";
        assertDropCollection(db, germanCollection);

        const englishCaseInsensitiveStream = cst.startWatchingChanges({
            pipeline: [
                {$changeStream: {}},
                {$match: {"fullDocument.text": "abc"}},
                {$project: {docId: "$documentKey._id"}}
            ],
            aggregateOptions: {collation: caseInsensitive},
            collection: germanCollection
        });

        germanCollection =
            assertCreateCollection(db, germanCollection, {collation: {locale: "de"}});
        assert.writeOK(germanCollection.insert({_id: 0, text: "aBc"}));

        cst.assertNextChangesEqual(
            {cursor: englishCaseInsensitiveStream, expectedChanges: [{docId: 0}]});
    }());

    // Test that creating a change stream with a non-default collation against a collection that has
    // a non-simple default collation will use the collation specified on the operation.
    (function() {
        const caseInsensitiveCollection = assertDropAndRecreateCollection(
            db, "change_stream_case_insensitive", {collation: caseInsensitive});

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
        const noCollationCollection =
            assertDropAndRecreateCollection(db, "change_stream_no_collation");

        const cursor = noCollationCollection.watch(
            [
              {$match: {"fullDocument.text": "abc"}},
              {$project: {docId: "$documentKey._id", _id: 0}}
            ],
            {collation: caseInsensitive});
        assert(!cursor.hasNext());
        assert.writeOK(noCollationCollection.insert({_id: 0, text: "aBc"}));
        assert.writeOK(noCollationCollection.insert({_id: 1, text: "abc"}));
        assert.soon(() => cursor.hasNext());
        assert.docEq(cursor.next(), {docId: 0});
        assert(cursor.hasNext());
        assert.docEq(cursor.next(), {docId: 1});
        assert(!cursor.hasNext());
    }());

})();

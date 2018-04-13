/**
 * Tests that a change stream can use a user-specified, or collection-default collation.
 */
(function() {
    "use strict";

    load("jstests/libs/collection_drop_recreate.js");  // For assert[Drop|Create]Collection.
    load("jstests/libs/change_stream_util.js");        // For 'ChangeStreamTest' and
                                                       // 'runCommandChangeStreamPassthroughAware'.
    load("jstests/libs/fixture_helpers.js");           // For 'isMongos'.

    if (FixtureHelpers.isMongos(db)) {
        // TODO: SERVER-33944 Change streams on sharded collection with non-simple default
        // collation may be erroneously invalidated if a chunk migration occurs. When this bug is
        // fixed, chunk migrations should be allowed in this test, and we should remove this call
        // to stopBalancer().
        sh.stopBalancer();
    }

    try {
        let cst = new ChangeStreamTest(db);

        const caseInsensitive = {locale: "en_US", strength: 2};

        let caseInsensitiveCollection = "change_stream_case_insensitive";
        assertDropCollection(db, caseInsensitiveCollection);

        // Test that you can open a change stream before the collection exists, and it will use the
        // simple collation. Tag this stream as 'doNotModifyInPassthroughs', since whole-db and
        // cluster-wide streams do not adhere to the same collation rules that we will be testing
        // with this cursor.
        const simpleCollationStream = cst.startWatchingChanges({
            pipeline: [{$changeStream: {}}],
            collection: caseInsensitiveCollection,
            doNotModifyInPassthroughs: true
        });

        // Create the collection with a non-default collation - this should invalidate the stream we
        // opened before it existed.
        caseInsensitiveCollection =
            assertCreateCollection(db, caseInsensitiveCollection, {collation: caseInsensitive});
        cst.assertNextChangesEqual({
            cursor: simpleCollationStream,
            expectedChanges: [{operationType: "invalidate"}],
            expectInvalidate: true
        });

        const caseInsensitivePipeline = [
            {$changeStream: {}},
            {$match: {"fullDocument.text": "abc"}},
            {$project: {docId: "$documentKey._id"}}
        ];

        // Test that $changeStream will implicitly adopt the default collation of the collection on
        // which it is run. Tag this stream as 'doNotModifyInPassthroughs', since whole-db and
        // cluster-wide streams do not have default collations.
        const implicitCaseInsensitiveStream = cst.startWatchingChanges({
            pipeline: caseInsensitivePipeline,
            collection: caseInsensitiveCollection,
            doNotModifyInPassthroughs: true
        });
        // Test that a collation can be explicitly specified for the $changeStream. This does not
        // need to be tagged 'doNotModifyInPassthroughs', since whole-db and cluster-wide
        // changeStreams will use an explicit collation if present.
        let explicitCaseInsensitiveStream = cst.startWatchingChanges({
            pipeline: caseInsensitivePipeline,
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

        // We must recreate the explicitCaseInsensitiveStream and set 'doNotModifyInPassthroughs'.
        // Whole-db and cluster-wide streams use the simple collation while scanning the oplog, but
        // do not filter the oplog by collection name. The subsequent $match stage which we inject
        // into the pipeline to filter for a specific collection will obey the pipeline's
        // case-insensitive collation, meaning that 'cHaNgE_sTrEaM_cAsE_iNsEnSiTiVe' will match
        // 'change_stream_case_insensitive'.
        explicitCaseInsensitiveStream = cst.startWatchingChanges({
            pipeline: caseInsensitivePipeline,
            collection: caseInsensitiveCollection,
            aggregateOptions: {collation: caseInsensitive},
            doNotModifyInPassthroughs: true
        });

        assert.writeOK(similarNameCollection.insert({_id: 0, text: "aBc"}));
        assert.writeOK(caseInsensitiveCollection.insert({_id: 2, text: "ABC"}));

        // The existing stream should not see the first insert (to the other collection), but
        // should see the second.
        cst.assertNextChangesEqual(
            {cursor: implicitCaseInsensitiveStream, expectedChanges: [{docId: 2}]});
        cst.assertNextChangesEqual(
            {cursor: explicitCaseInsensitiveStream, expectedChanges: [{docId: 2}]});

        // Test that creating a collection without a collation does not invalidate any change
        // streams that were opened before the collection existed.
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

            simpleCollationCollection = assertCreateCollection(
                db, simpleCollationCollection, {collation: {locale: "simple"}});
            assert.writeOK(simpleCollationCollection.insert({_id: 0}));

            cst.assertNextChangesEqual({
                cursor: streamCreatedBeforeSimpleCollationCollection,
                expectedChanges: [{docId: 0}]
            });
        }());

        // Test that creating a change stream with a non-default collation, then creating a
        // collection with the same collation will not invalidate the change stream.
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

        // Test that creating a change stream with a non-default collation, then creating a
        // collection with *a different* collation will not invalidate the change stream.
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

        // Test that creating a change stream with a non-default collation against a collection
        // that has a non-simple default collation will use the collation specified on the
        // operation.
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

        // Test that collation is supported by the shell helper.  Test that creating a change
        // stream with a non-default collation against a collection that has a simple default
        // collation will use the collation specified on the operation.
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
            assert.soon(() => cursor.hasNext());
            assert.docEq(cursor.next(), {docId: 1});
            assert(!cursor.hasNext());
        }());

        // Test that resuming a change stream on a collection that has been dropped requires the
        // user to explicitly specify the collation. This is testing that we cannot resume if we
        // need to retrieve the collection metadata.
        (function() {
            const collName = "change_stream_case_insensitive";
            let caseInsensitiveCollection =
                assertDropAndRecreateCollection(db, collName, {collation: caseInsensitive});

            let changeStream = caseInsensitiveCollection.watch(
                [{$match: {"fullDocument.text": "abc"}}], {collation: caseInsensitive});

            assert.writeOK(caseInsensitiveCollection.insert({_id: 0, text: "abc"}));

            assert.soon(() => changeStream.hasNext());
            const next = changeStream.next();
            assert.docEq(next.documentKey, {_id: 0});
            const resumeToken = next._id;

            // Insert a second document to see after resuming.
            assert.writeOK(caseInsensitiveCollection.insert({_id: "dropped_coll", text: "ABC"}));

            // Drop the collection to invalidate the stream.
            assertDropCollection(db, collName);

            // Test that a $changeStream is allowed to resume on the dropped collection if an
            // explicit collation is provided, even if it doesn't match the original collection
            // default collation.
            changeStream = caseInsensitiveCollection.watch(
                [{$match: {"fullDocument.text": "ABC"}}],
                {resumeAfter: resumeToken, collation: {locale: "simple"}});

            assert.soon(() => changeStream.hasNext());
            assert.docEq(changeStream.next().documentKey, {_id: "dropped_coll"});

            // Test that a pipeline without an explicit collation is not allowed to resume the
            // change stream after the collection has been dropped. Do not modify this command in
            // the passthrough suite(s) since whole-db and whole-cluster change streams are allowed
            // to resume without an explicit collation.
            assert.commandFailedWithCode(
                runCommandChangeStreamPassthroughAware(
                    db,
                    {
                      aggregate: collName,
                      pipeline: [{$changeStream: {resumeAfter: resumeToken}}],
                      cursor: {},
                    },
                    true),  // doNotModifyInPassthroughs
                ErrorCodes.InvalidResumeToken);
        }());

        // Test that the default collation of a new version of the collection is not applied when
        // resuming a change stream from before a collection drop.
        (function() {
            const collName = "change_stream_case_insensitive";
            let caseInsensitiveCollection =
                assertDropAndRecreateCollection(db, collName, {collation: caseInsensitive});

            let changeStream = caseInsensitiveCollection.watch(
                [{$match: {"fullDocument.text": "abc"}}], {collation: caseInsensitive});

            assert.writeOK(caseInsensitiveCollection.insert({_id: 0, text: "abc"}));

            assert.soon(() => changeStream.hasNext());
            const next = changeStream.next();
            assert.docEq(next.documentKey, {_id: 0});
            const resumeToken = next._id;

            // Insert a second document to see after resuming.
            assert.writeOK(caseInsensitiveCollection.insert({_id: "dropped_coll", text: "ABC"}));

            // Recreate the collection with a different collation.
            caseInsensitiveCollection = assertDropAndRecreateCollection(
                db, caseInsensitiveCollection.getName(), {collation: {locale: "simple"}});
            assert.writeOK(caseInsensitiveCollection.insert({_id: "new collection", text: "abc"}));

            // Verify that the stream sees the insert before the drop and then is exhausted. We
            // won't see the invalidate because the pipeline has a $match stage after the
            // $changeStream.
            assert.soon(() => changeStream.hasNext());
            assert.docEq(changeStream.next().fullDocument, {_id: "dropped_coll", text: "ABC"});
            // Only single-collection streams will be exhausted from the drop.
            assert(changeStream.isExhausted() || isChangeStreamPassthrough());

            // Test that a pipeline with an explicit collation is allowed to resume from before the
            // collection is dropped and recreated.
            changeStream = caseInsensitiveCollection.watch(
                [{$match: {"fullDocument.text": "ABC"}}],
                {resumeAfter: resumeToken, collation: {locale: "fr"}});

            assert.soon(() => changeStream.hasNext());
            assert.docEq(changeStream.next().documentKey, {_id: "dropped_coll"});
            // Only single-collection streams will be exhausted from the drop.
            assert(changeStream.isExhausted() || isChangeStreamPassthrough());

            // Test that a pipeline without an explicit collation is not allowed to resume,
            // even though the collection has been recreated with the same default collation as it
            // had previously. Do not modify this command in the passthrough suite(s) since whole-db
            // and whole-cluster change streams are allowed to resume without an explicit collation.
            assert.commandFailedWithCode(
                runCommandChangeStreamPassthroughAware(
                    db,
                    {
                      aggregate: collName,
                      pipeline: [{$changeStream: {resumeAfter: resumeToken}}],
                      cursor: {}
                    },
                    true),  // doNotModifyInPassthroughs
                ErrorCodes.InvalidResumeToken);
        }());

    } finally {
        if (FixtureHelpers.isMongos(db)) {
            // TODO: SERVER-33944 Change streams on sharded collection with non-simple default
            // collation may be erroneously invalidated if a chunk migration occurs. When this bug
            // is fixed, chunk migrations should be allowed in this test, and we should remove this
            // call to startBalancer() as well as the earlier call to stopBalancer().
            sh.startBalancer();
        }
    }
})();

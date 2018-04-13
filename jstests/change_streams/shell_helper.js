// Test change streams related shell helpers and options passed to them. Note that, while we only
// call the DBCollection.watch helper in this file, it will be redirected to the DB.watch or
// Mongo.watch equivalents in the whole_db and whole_cluster passthroughs.
(function() {
    "use strict";

    load("jstests/libs/collection_drop_recreate.js");  // For assert[Drop|Create]Collection.
    load("jstests/libs/fixture_helpers.js");           // For FixtureHelpers.
    load("jstests/libs/change_stream_util.js");        // For assertInvalidateOp.

    const coll = assertDropAndRecreateCollection(db, "change_stream_shell_helper");

    function checkNextChange(cursor, expected) {
        assert.soon(() => cursor.hasNext());
        const nextObj = cursor.next();
        delete nextObj._id;
        delete nextObj.clusterTime;
        assert.docEq(nextObj, expected);
    }

    function testCommandIsCalled(testFunc, checkFunc) {
        const mongoRunCommandOriginal = Mongo.prototype.runCommand;

        const sentinel = {};
        let cmdObjSeen = sentinel;

        Mongo.prototype.runCommand = function runCommandSpy(dbName, cmdObj, options) {
            cmdObjSeen = cmdObj;
            return mongoRunCommandOriginal.apply(this, arguments);
        };

        try {
            assert.doesNotThrow(testFunc);
        } finally {
            Mongo.prototype.runCommand = mongoRunCommandOriginal;
        }

        if (cmdObjSeen === sentinel) {
            throw new Error("Mongo.prototype.runCommand() was never called: " +
                            testFunc.toString());
        }

        checkFunc(cmdObjSeen);
    }

    jsTestLog("Testing watch() without options");
    let changeStreamCursor = coll.watch();

    assert(!changeStreamCursor.hasNext());

    // Write the first document into the collection. We will save the resume token from this change.
    assert.writeOK(coll.insert({_id: 0, x: 1}));
    let resumeToken;

    // Test that each of the change stream cursors picks up the change.
    assert.soon(() => changeStreamCursor.hasNext());
    let change = changeStreamCursor.next();
    assert(!changeStreamCursor.hasNext());
    let expected = {
        documentKey: {_id: 0},
        fullDocument: {_id: 0, x: 1},
        ns: {db: "test", coll: coll.getName()},
        operationType: "insert",
    };
    assert("_id" in change, "Got unexpected change: " + tojson(change));
    // Remember the _id of the first op to resume the stream.
    resumeToken = change._id;
    // Remove the fields we cannot predict, then test that the change is as expected.
    delete change._id;
    delete change.clusterTime;
    assert.docEq(change, expected);

    jsTestLog("Testing watch() with pipeline");
    changeStreamCursor = coll.watch([{$project: {_id: 0, docId: "$documentKey._id"}}]);

    // Store the cluster time of the insert as the timestamp to start from.
    const resumeTime =
        assert.commandWorked(db.runCommand({insert: coll.getName(), documents: [{_id: 1, x: 1}]}))
            .operationTime;

    checkNextChange(changeStreamCursor, {docId: 1});

    jsTestLog("Testing watch() with pipeline and resumeAfter");
    changeStreamCursor =
        coll.watch([{$project: {_id: 0, docId: "$documentKey._id"}}], {resumeAfter: resumeToken});
    checkNextChange(changeStreamCursor, {docId: 1});

    jsTestLog("Testing watch() with pipeline and startAtOperationTime");
    changeStreamCursor = coll.watch([{$project: {_id: 0, docId: "$documentKey._id"}}],
                                    {startAtOperationTime: resumeTime});
    checkNextChange(changeStreamCursor, {docId: 1});

    jsTestLog("Testing watch() with updateLookup");
    changeStreamCursor = coll.watch([{$project: {_id: 0}}], {fullDocument: "updateLookup"});

    assert.writeOK(coll.update({_id: 0}, {$set: {x: 10}}));
    expected = {
        documentKey: {_id: 0},
        fullDocument: {_id: 0, x: 10},
        ns: {db: "test", coll: coll.getName()},
        operationType: "update",
        updateDescription: {removedFields: [], updatedFields: {x: 10}},
    };
    checkNextChange(changeStreamCursor, expected);

    jsTestLog("Testing watch() with batchSize");
    // Only test mongod because mongos uses batch size 0 for aggregate commands internally to
    // establish cursors quickly. GetMore on mongos doesn't respect batch size due to SERVER-31992.
    const isMongos = FixtureHelpers.isMongos(db);
    if (!isMongos) {
        // Increase a field by 5 times and verify the batch size is respected.
        for (let i = 0; i < 5; i++) {
            assert.writeOK(coll.update({_id: 1}, {$inc: {x: 1}}));
        }

        // Only watch the "update" changes of the specific doc since the beginning.
        changeStreamCursor = coll.watch(
            [{$match: {documentKey: {_id: 1}, operationType: "update"}}, {$project: {_id: 0}}],
            {resumeAfter: resumeToken, batchSize: 2});

        // Check the first batch.
        assert.eq(changeStreamCursor.objsLeftInBatch(), 2);
        // Consume the first batch.
        assert(changeStreamCursor.hasNext());
        changeStreamCursor.next();
        assert(changeStreamCursor.hasNext());
        changeStreamCursor.next();
        // Confirm that the batch is empty.
        assert.eq(changeStreamCursor.objsLeftInBatch(), 0);

        // Check the batch returned by getMore.
        assert(changeStreamCursor.hasNext());
        assert.eq(changeStreamCursor.objsLeftInBatch(), 2);
        changeStreamCursor.next();
        assert(changeStreamCursor.hasNext());
        changeStreamCursor.next();
        assert.eq(changeStreamCursor.objsLeftInBatch(), 0);
        // There are more changes coming, just not in the batch.
        assert(changeStreamCursor.hasNext());
    }

    jsTestLog("Testing watch() with maxAwaitTimeMS");
    changeStreamCursor = coll.watch([], {maxAwaitTimeMS: 500});
    testCommandIsCalled(() => assert(!changeStreamCursor.hasNext()), (cmdObj) => {
        assert.eq("getMore",
                  Object.keys(cmdObj)[0],
                  "expected getMore command, but was: " + tojson(cmdObj));
        assert(cmdObj.hasOwnProperty("maxTimeMS"), "unexpected getMore command: " + tojson(cmdObj));
        assert.eq(500, cmdObj.maxTimeMS, "unexpected getMore command: " + tojson(cmdObj));
    });

    jsTestLog("Testing the cursor gets closed when the collection gets dropped");
    changeStreamCursor = coll.watch([{$project: {_id: 0, clusterTime: 0}}]);
    assert.writeOK(coll.insert({_id: 2, x: 1}));
    expected = {
        documentKey: {_id: 2},
        fullDocument: {_id: 2, x: 1},
        ns: {db: "test", coll: coll.getName()},
        operationType: "insert",
    };
    checkNextChange(changeStreamCursor, expected);
    assert(!changeStreamCursor.hasNext());
    assert(!changeStreamCursor.isClosed());
    assert(!changeStreamCursor.isExhausted());

    // Dropping the collection should trigger a drop notification.
    assertDropCollection(db, coll.getName());
    assert.soon(() => changeStreamCursor.hasNext());
    assert(!changeStreamCursor.isExhausted());
    expected = {operationType: "drop", ns: {db: db.getName(), coll: coll.getName()}};
    checkNextChange(changeStreamCursor, expected);
    // For single collection change streams, the drop should invalidate the stream.
    assertInvalidateOp({cursor: changeStreamCursor, opType: "drop"});
}());

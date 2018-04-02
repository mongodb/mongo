// Test change streams related shell helpers and options passed to them.
(function() {
    "use strict";

    load("jstests/libs/collection_drop_recreate.js");  // For assert[Drop|Create]Collection.

    const coll = assertDropAndRecreateCollection(db, "change_stream_shell_helper");

    function checkNextChange(cursor, expected) {
        assert.soon(() => cursor.hasNext());
        assert.docEq(cursor.next(), expected);
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
    let cursor = coll.watch();
    let wholeDbCursor = db.watch();
    assert(!cursor.hasNext());
    assert.writeOK(coll.insert({_id: 0, x: 1}));
    assert.soon(() => cursor.hasNext());
    let change = cursor.next();
    assert(!cursor.hasNext());
    let expected = {
        documentKey: {_id: 0},
        fullDocument: {_id: 0, x: 1},
        ns: {db: "test", coll: coll.getName()},
        operationType: "insert",
    };
    assert("_id" in change, "Got unexpected change: " + tojson(change));
    // Remember the _id of the first op to resume the stream.
    const resumeToken = change._id;
    delete change._id;
    assert.docEq(change, expected);

    // Test that the change stream on the entire databse picks up the change as well.
    assert.soon(() => wholeDbCursor.hasNext());
    change = wholeDbCursor.next();
    assert(!wholeDbCursor.hasNext());
    assert("_id" in change, "Got unexpected change: " + tojson(change));
    delete change._id;
    assert.docEq(change, expected);

    jsTestLog("Testing watch() with pipeline");
    cursor = coll.watch([{$project: {_id: 0, docId: "$documentKey._id"}}]);
    wholeDbCursor = db.watch([{$project: {_id: 0, docId: "$documentKey._id"}}]);
    assert.writeOK(coll.insert({_id: 1, x: 1}));
    checkNextChange(cursor, {docId: 1});
    checkNextChange(wholeDbCursor, {docId: 1});

    jsTestLog("Testing watch() with pipeline and resumeAfter");
    cursor =
        coll.watch([{$project: {_id: 0, docId: "$documentKey._id"}}], {resumeAfter: resumeToken});
    wholeDbCursor =
        db.watch([{$project: {_id: 0, docId: "$documentKey._id"}}], {resumeAfter: resumeToken});
    checkNextChange(cursor, {docId: 1});
    checkNextChange(wholeDbCursor, {docId: 1});

    jsTestLog("Testing watch() with pipeline and startAtClusterTime");
    // Store the cluster time of the last insert as the timestamp to start from.
    const resumeTime = db.runCommand({isMaster: 1}).$clusterTime.clusterTime;
    cursor = coll.watch([{$project: {_id: 0, docId: "$documentKey._id"}}],
                        {startAtClusterTime: {ts: resumeTime}});
    wholeDbCursor = db.watch([{$project: {_id: 0, docId: "$documentKey._id"}}],
                             {startAtClusterTime: {ts: resumeTime}});
    checkNextChange(cursor, {docId: 1});
    checkNextChange(wholeDbCursor, {docId: 1});

    jsTestLog("Testing watch() with updateLookup");
    cursor = coll.watch([{$project: {_id: 0}}], {fullDocument: "updateLookup"});
    wholeDbCursor = db.watch([{$project: {_id: 0}}], {fullDocument: "updateLookup"});

    assert.writeOK(coll.update({_id: 0}, {$set: {x: 10}}));
    expected = {
        documentKey: {_id: 0},
        fullDocument: {_id: 0, x: 10},
        ns: {db: "test", coll: coll.getName()},
        operationType: "update",
        updateDescription: {removedFields: [], updatedFields: {x: 10}},
    };
    checkNextChange(cursor, expected);
    checkNextChange(wholeDbCursor, expected);

    jsTestLog("Testing watch() with batchSize");
    // Only test mongod because mongos uses batch size 0 for aggregate commands internally to
    // establish cursors quickly.
    // GetMore on mongos doesn't respect batch size either due to SERVER-31992.
    const isMongos = db.runCommand({isdbgrid: 1}).isdbgrid;
    if (!isMongos) {
        // Increase a field by 5 times and verify the batch size is respected.
        for (let i = 0; i < 5; i++) {
            assert.writeOK(coll.update({_id: 1}, {$inc: {x: 1}}));
        }

        // Only watch the "update" changes of the specific doc since the beginning.
        cursor = coll.watch(
            [{$match: {documentKey: {_id: 1}, operationType: "update"}}, {$project: {_id: 0}}],
            {resumeAfter: resumeToken, batchSize: 2});
        wholeDbCursor = db.watch(
            [{$match: {documentKey: {_id: 1}, operationType: "update"}}, {$project: {_id: 0}}],
            {resumeAfter: resumeToken, batchSize: 2});

        // Check the first batch.
        assert.eq(cursor.objsLeftInBatch(), 2);
        // Consume the first batch.
        assert(cursor.hasNext());
        cursor.next();
        assert(cursor.hasNext());
        cursor.next();
        assert.eq(cursor.objsLeftInBatch(), 0);

        // Ditto for the whole db change stream.
        assert.eq(wholeDbCursor.objsLeftInBatch(), 2);
        assert(wholeDbCursor.hasNext());
        wholeDbCursor.next();
        assert(wholeDbCursor.hasNext());
        wholeDbCursor.next();
        assert.eq(wholeDbCursor.objsLeftInBatch(), 0);

        // Check the batch returned by getMore.
        assert(cursor.hasNext());
        assert.eq(cursor.objsLeftInBatch(), 2);
        cursor.next();
        assert(cursor.hasNext());
        cursor.next();
        assert.eq(cursor.objsLeftInBatch(), 0);
        // There are more changes coming, just not in the batch.
        assert(cursor.hasNext());

        // Ditto for the whole db change stream.
        assert(wholeDbCursor.hasNext());
        assert.eq(wholeDbCursor.objsLeftInBatch(), 2);
        wholeDbCursor.next();
        assert(wholeDbCursor.hasNext());
        wholeDbCursor.next();
        assert.eq(wholeDbCursor.objsLeftInBatch(), 0);
        // There are more changes coming, just not in the batch.
        assert(wholeDbCursor.hasNext());
    }

    jsTestLog("Testing watch() with maxAwaitTimeMS");
    cursor = coll.watch([], {maxAwaitTimeMS: 500});
    testCommandIsCalled(() => assert(!cursor.hasNext()), (cmdObj) => {
        assert.eq("getMore",
                  Object.keys(cmdObj)[0],
                  "expected getMore command, but was: " + tojson(cmdObj));
        assert(cmdObj.hasOwnProperty("maxTimeMS"), "unexpected getMore command: " + tojson(cmdObj));
        assert.eq(500, cmdObj.maxTimeMS, "unexpected getMore command: " + tojson(cmdObj));
    });

    wholeDbCursor = db.watch([], {maxAwaitTimeMS: 500});
    testCommandIsCalled(() => assert(!wholeDbCursor.hasNext()), (cmdObj) => {
        assert.eq("getMore",
                  Object.keys(cmdObj)[0],
                  "expected getMore command, but was: " + tojson(cmdObj));
        assert(cmdObj.hasOwnProperty("maxTimeMS"), "unexpected getMore command: " + tojson(cmdObj));
        assert.eq(500, cmdObj.maxTimeMS, "unexpected getMore command: " + tojson(cmdObj));
    });

    jsTestLog("Testing the cursor gets closed when the collection gets dropped");
    cursor = coll.watch([{$project: {_id: 0}}]);
    wholeDbCursor = db.watch([{$project: {_id: 0}}]);
    assert.writeOK(coll.insert({_id: 2, x: 1}));
    expected = {
        documentKey: {_id: 2},
        fullDocument: {_id: 2, x: 1},
        ns: {db: "test", coll: coll.getName()},
        operationType: "insert",
    };
    checkNextChange(cursor, expected);
    assert(!cursor.hasNext());
    assert(!cursor.isClosed());
    assert(!cursor.isExhausted());

    checkNextChange(wholeDbCursor, expected);
    assert(!wholeDbCursor.hasNext());
    assert(!wholeDbCursor.isClosed());
    assert(!wholeDbCursor.isExhausted());

    // Dropping the collection should invalidate any open change streams.
    assertDropCollection(db, coll.getName());
    assert.soon(() => cursor.hasNext());
    assert(cursor.isClosed());
    assert(!cursor.isExhausted());
    expected = {operationType: "invalidate"};
    checkNextChange(cursor, expected);
    assert(!cursor.hasNext());
    assert(cursor.isClosed());
    assert(cursor.isExhausted());

    assert.soon(() => wholeDbCursor.hasNext());
    assert(wholeDbCursor.isClosed());
    assert(!wholeDbCursor.isExhausted());
    expected = {operationType: "invalidate"};
    checkNextChange(wholeDbCursor, expected);
    assert(!wholeDbCursor.hasNext());
    assert(wholeDbCursor.isClosed());
    assert(wholeDbCursor.isExhausted());
}());

// Test change streams related shell helpers and options passed to them.
(function() {
    "use strict";

    load("jstests/libs/collection_drop_recreate.js");  // For assert[Drop|Create]Collection.

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
    let singleCollCursor = coll.watch();
    let wholeDbCursor = db.watch();
    let wholeClusterCursor = db.getMongo().watch();

    [singleCollCursor, wholeDbCursor, wholeClusterCursor].forEach((cursor) =>
                                                                      assert(!cursor.hasNext()));

    // Write the first document into the collection. We will save the resume token from this change.
    assert.writeOK(coll.insert({_id: 0, x: 1}));
    let resumeToken;

    // Test that each of the change stream cursors picks up the change.
    for (let cursor of[singleCollCursor, wholeDbCursor, wholeClusterCursor]) {
        print(`Running test on namespace '${cursor._ns}'`);
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
        resumeToken = change._id;
        // Remove the fields we cannot predict, then test that the change is as expected.
        delete change._id;
        delete change.clusterTime;
        assert.docEq(change, expected);
    }

    jsTestLog("Testing watch() with pipeline");
    singleCollCursor = coll.watch([{$project: {_id: 0, docId: "$documentKey._id"}}]);
    wholeDbCursor = db.watch([{$project: {_id: 0, docId: "$documentKey._id"}}]);
    wholeClusterCursor = db.getMongo().watch([{$project: {_id: 0, docId: "$documentKey._id"}}]);

    // Store the cluster time of the insert as the timestamp to start from.
    const resumeTime =
        assert.commandWorked(db.runCommand({insert: coll.getName(), documents: [{_id: 1, x: 1}]}))
            .$clusterTime.clusterTime;

    checkNextChange(singleCollCursor, {docId: 1});
    checkNextChange(wholeDbCursor, {docId: 1});
    checkNextChange(wholeClusterCursor, {docId: 1});

    jsTestLog("Testing watch() with pipeline and resumeAfter");
    singleCollCursor =
        coll.watch([{$project: {_id: 0, docId: "$documentKey._id"}}], {resumeAfter: resumeToken});
    wholeDbCursor =
        db.watch([{$project: {_id: 0, docId: "$documentKey._id"}}], {resumeAfter: resumeToken});
    wholeClusterCursor = db.getMongo().watch([{$project: {_id: 0, docId: "$documentKey._id"}}],
                                             {resumeAfter: resumeToken});
    checkNextChange(singleCollCursor, {docId: 1});
    checkNextChange(wholeDbCursor, {docId: 1});
    checkNextChange(wholeClusterCursor, {docId: 1});

    jsTestLog("Testing watch() with pipeline and startAtClusterTime");
    singleCollCursor = coll.watch([{$project: {_id: 0, docId: "$documentKey._id"}}],
                                  {startAtClusterTime: {ts: resumeTime}});
    wholeDbCursor = db.watch([{$project: {_id: 0, docId: "$documentKey._id"}}],
                             {startAtClusterTime: {ts: resumeTime}});
    wholeClusterCursor = db.getMongo().watch([{$project: {_id: 0, docId: "$documentKey._id"}}],
                                             {startAtClusterTime: {ts: resumeTime}});
    checkNextChange(singleCollCursor, {docId: 1});
    checkNextChange(wholeDbCursor, {docId: 1});
    checkNextChange(wholeClusterCursor, {docId: 1});

    jsTestLog("Testing watch() with updateLookup");
    singleCollCursor = coll.watch([{$project: {_id: 0}}], {fullDocument: "updateLookup"});
    wholeDbCursor = db.watch([{$project: {_id: 0}}], {fullDocument: "updateLookup"});
    wholeClusterCursor =
        db.getMongo().watch([{$project: {_id: 0}}], {fullDocument: "updateLookup"});

    assert.writeOK(coll.update({_id: 0}, {$set: {x: 10}}));
    let expected = {
        documentKey: {_id: 0},
        fullDocument: {_id: 0, x: 10},
        ns: {db: "test", coll: coll.getName()},
        operationType: "update",
        updateDescription: {removedFields: [], updatedFields: {x: 10}},
    };
    checkNextChange(singleCollCursor, expected);
    checkNextChange(wholeDbCursor, expected);
    checkNextChange(wholeClusterCursor, expected);

    jsTestLog("Testing watch() with batchSize");
    // Only test mongod because mongos uses batch size 0 for aggregate commands internally to
    // establish cursors quickly. GetMore on mongos doesn't respect batch size due to SERVER-31992.
    const isMongos = db.runCommand({isdbgrid: 1}).isdbgrid;
    if (!isMongos) {
        // Increase a field by 5 times and verify the batch size is respected.
        for (let i = 0; i < 5; i++) {
            assert.writeOK(coll.update({_id: 1}, {$inc: {x: 1}}));
        }

        // Only watch the "update" changes of the specific doc since the beginning.
        singleCollCursor = coll.watch(
            [{$match: {documentKey: {_id: 1}, operationType: "update"}}, {$project: {_id: 0}}],
            {resumeAfter: resumeToken, batchSize: 2});
        wholeDbCursor = db.watch(
            [{$match: {documentKey: {_id: 1}, operationType: "update"}}, {$project: {_id: 0}}],
            {resumeAfter: resumeToken, batchSize: 2});
        wholeClusterCursor = db.getMongo().watch(
            [{$match: {documentKey: {_id: 1}, operationType: "update"}}, {$project: {_id: 0}}],
            {resumeAfter: resumeToken, batchSize: 2});

        for (let cursor of[singleCollCursor, wholeDbCursor, wholeClusterCursor]) {
            print(`Running test on namespace '${cursor._ns}'`);
            // Check the first batch.
            assert.eq(cursor.objsLeftInBatch(), 2);
            // Consume the first batch.
            assert(cursor.hasNext());
            cursor.next();
            assert(cursor.hasNext());
            cursor.next();
            // Confirm that the batch is empty.
            assert.eq(cursor.objsLeftInBatch(), 0);

            // Check the batch returned by getMore.
            assert(cursor.hasNext());
            assert.eq(cursor.objsLeftInBatch(), 2);
            cursor.next();
            assert(cursor.hasNext());
            cursor.next();
            assert.eq(cursor.objsLeftInBatch(), 0);
            // There are more changes coming, just not in the batch.
            assert(cursor.hasNext());
        }
    }

    jsTestLog("Testing watch() with maxAwaitTimeMS");
    singleCollCursor = coll.watch([], {maxAwaitTimeMS: 500});
    testCommandIsCalled(() => assert(!singleCollCursor.hasNext()), (cmdObj) => {
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

    wholeClusterCursor = db.getMongo().watch([], {maxAwaitTimeMS: 500});
    testCommandIsCalled(() => assert(!wholeClusterCursor.hasNext()), (cmdObj) => {
        assert.eq("getMore",
                  Object.keys(cmdObj)[0],
                  "expected getMore command, but was: " + tojson(cmdObj));
        assert(cmdObj.hasOwnProperty("maxTimeMS"), "unexpected getMore command: " + tojson(cmdObj));
        assert.eq(500, cmdObj.maxTimeMS, "unexpected getMore command: " + tojson(cmdObj));
    });

    jsTestLog("Testing the cursor gets closed when the collection gets dropped");
    singleCollCursor = coll.watch([{$project: {_id: 0, clusterTime: 0}}]);
    wholeDbCursor = db.watch([{$project: {_id: 0, clusterTime: 0}}]);
    wholeClusterCursor = db.getMongo().watch([{$project: {_id: 0, clusterTime: 0}}]);
    assert.writeOK(coll.insert({_id: 2, x: 1}));
    expected = {
        documentKey: {_id: 2},
        fullDocument: {_id: 2, x: 1},
        ns: {db: "test", coll: coll.getName()},
        operationType: "insert",
    };
    checkNextChange(singleCollCursor, expected);
    assert(!singleCollCursor.hasNext());
    assert(!singleCollCursor.isClosed());
    assert(!singleCollCursor.isExhausted());

    checkNextChange(wholeDbCursor, expected);
    assert(!wholeDbCursor.hasNext());
    assert(!wholeDbCursor.isClosed());
    assert(!wholeDbCursor.isExhausted());

    checkNextChange(wholeClusterCursor, expected);
    assert(!wholeClusterCursor.hasNext());
    assert(!wholeClusterCursor.isClosed());
    assert(!wholeClusterCursor.isExhausted());

    // Dropping the collection should invalidate any open change streams.
    assertDropCollection(db, coll.getName());
    for (let cursor of[singleCollCursor, wholeDbCursor, wholeClusterCursor]) {
        print(`Running test on namespace '${cursor._ns}'`);
        assert.soon(() => cursor.hasNext());
        assert(cursor.isClosed());
        assert(!cursor.isExhausted());
        expected = {operationType: "invalidate"};
        checkNextChange(cursor, expected);
        assert(!cursor.hasNext());
        assert(cursor.isClosed());
        assert(cursor.isExhausted());
    }
}());

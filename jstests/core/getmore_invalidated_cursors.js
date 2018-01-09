// @tags: [requires_getmore, requires_non_retryable_commands]

// Tests that running a getMore on a cursor that has been invalidated by something like a collection
// drop will return an appropriate error message.
(function() {
    'use strict';

    const testDB = db.getSiblingDB("getmore_invalidated_cursors");
    const coll = testDB.test;

    const nDocs = 100;
    const batchSize = nDocs - 1;

    function setupCollection() {
        coll.drop();
        const bulk = coll.initializeUnorderedBulkOp();
        for (let i = 0; i < nDocs; ++i) {
            bulk.insert({_id: i, x: i});
        }
        assert.writeOK(bulk.execute());
        assert.commandWorked(coll.createIndex({x: 1}));
    }

    // Test that dropping the database between a find and a getMore will return an appropriate error
    // code and message.
    setupCollection();

    const isShardedCollection = coll.stats().sharded;
    const shellReadMode = testDB.getMongo().readMode();

    let cursor = coll.find().batchSize(batchSize);
    cursor.next();  // Send the query to the server.

    assert.commandWorked(testDB.dropDatabase());

    let error = assert.throws(() => cursor.itcount());

    if (testDB.runCommand({isdbgrid: 1}).isdbgrid && shellReadMode == 'legacy') {
        // The cursor will be invalidated on mongos, and we won't be able to find it.
        assert.neq(-1, error.message.indexOf('didn\'t exist on server'), error.message);
    } else {
        assert.eq(error.code, ErrorCodes.OperationFailed, tojson(error));
        assert.neq(-1, error.message.indexOf('collection dropped'), error.message);
    }

    // Test that dropping the collection between a find and a getMore will return an appropriate
    // error code and message.
    setupCollection();
    cursor = coll.find().batchSize(batchSize);
    cursor.next();  // Send the query to the server.

    coll.drop();

    error = assert.throws(() => cursor.itcount());
    if (isShardedCollection) {
        // The cursor will be invalidated on mongos, and we won't be able to find it.
        if (shellReadMode == 'legacy') {
            assert.neq(-1, error.message.indexOf('didn\'t exist on server'), error.message);
        } else {
            assert.eq(error.code, ErrorCodes.CursorNotFound, tojson(error));
            assert.neq(-1, error.message.indexOf('not found'), error.message);
        }
    } else {
        assert.eq(error.code, ErrorCodes.OperationFailed, tojson(error));
        assert.neq(-1, error.message.indexOf('collection dropped'), error.message);
    }

    // Test that dropping an index between a find and a getMore will return an appropriate error
    // code and message.
    setupCollection();
    cursor = coll.find().batchSize(batchSize);
    cursor.next();  // Send the query to the server.

    assert.commandWorked(testDB.runCommand({dropIndexes: coll.getName(), index: {x: 1}}));

    error = assert.throws(() => cursor.itcount());
    assert.eq(error.code, ErrorCodes.QueryPlanKilled, tojson(error));
    assert.neq(-1, error.message.indexOf('index \'x_1\' dropped'), error.message);

    // Test that killing a cursor between a find and a getMore will return an appropriate error
    // code and message.

    setupCollection();
    // Use the find command so that we can extract the cursor id to pass to the killCursors command.
    let cursorId = assert
                       .commandWorked(testDB.runCommand(
                           {find: coll.getName(), filter: {}, batchSize: batchSize}))
                       .cursor.id;
    assert.commandWorked(testDB.runCommand({killCursors: coll.getName(), cursors: [cursorId]}));
    assert.commandFailedWithCode(testDB.runCommand({getMore: cursorId, collection: coll.getName()}),
                                 ErrorCodes.CursorNotFound);

    // Test that all cursors on collections to be renamed get invalidated. Note that we can't do
    // renames on sharded collections.
    if (!isShardedCollection) {
        setupCollection();
        const collRenamed = testDB.test_rename;
        collRenamed.drop();
        cursor = coll.find().batchSize(batchSize);
        assert(cursor.hasNext(), "Expected more data from find call on " + coll.getName());
        assert.commandWorked(testDB.adminCommand({
            renameCollection: testDB.getName() + "." + coll.getName(),
            to: testDB.getName() + "." + collRenamed.getName()
        }));

        // Ensure getMore fails with an appropriate error code and message.
        error = assert.throws(() => cursor.itcount());
        assert.eq(error.code, ErrorCodes.OperationFailed, tojson(error));
        assert.neq(-1, error.message.indexOf('collection dropped'), error.message);
    }

}());

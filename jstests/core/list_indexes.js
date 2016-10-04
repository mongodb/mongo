// Basic functional tests for the listIndexes command.

(function() {
    "use strict";

    var coll = db.list_indexes1;
    var cursor;
    var res;
    var specs;

    //
    // Test basic command output.
    //

    coll.drop();
    assert.commandWorked(coll.getDB().createCollection(coll.getName()));
    res = coll.runCommand("listIndexes");
    assert.commandWorked(res);
    assert.eq("object", typeof(res.cursor));
    assert.eq(0, res.cursor.id);
    assert.eq("string", typeof(res.cursor.ns));
    assert.eq(1, res.cursor.firstBatch.length);
    assert.eq("_id_", res.cursor.firstBatch[0].name);

    //
    // Test basic usage with DBCommandCursor.
    //

    var getListIndexesCursor = function(coll, options, subsequentBatchSize) {
        return new DBCommandCursor(
            coll.getDB().getMongo(), coll.runCommand("listIndexes", options), subsequentBatchSize);
    };

    var cursorGetIndexSpecs = function(cursor) {
        return cursor.toArray().sort(function(a, b) {
            return a.name > b.name;
        });
    };

    var cursorGetIndexNames = function(cursor) {
        return cursorGetIndexSpecs(cursor).map(function(spec) {
            return spec.name;
        });
    };

    coll.drop();
    assert.commandWorked(coll.getDB().createCollection(coll.getName()));
    assert.eq(["_id_"], cursorGetIndexNames(getListIndexesCursor(coll)));

    //
    // Test that the index metadata object is returned correctly.
    //

    coll.drop();
    assert.commandWorked(coll.getDB().createCollection(coll.getName()));
    assert.commandWorked(coll.ensureIndex({a: 1}, {unique: true}));
    specs = cursorGetIndexSpecs(getListIndexesCursor(coll));
    assert.eq(2, specs.length);
    assert.eq("_id_", specs[0].name);
    assert.eq(coll.getFullName(), specs[0].ns);
    assert.eq({_id: 1}, specs[0].key);
    assert(!specs[0].hasOwnProperty("unique"));
    assert.eq("a_1", specs[1].name);
    assert.eq(coll.getFullName(), specs[1].ns);
    assert.eq({a: 1}, specs[1].key);
    assert.eq(true, specs[1].unique);

    //
    // Test that the command does not accept invalid values for the collection.
    //

    assert.commandFailed(coll.getDB().runCommand({listIndexes: ""}));
    assert.commandFailed(coll.getDB().runCommand({listIndexes: 1}));
    assert.commandFailed(coll.getDB().runCommand({listIndexes: {}}));
    assert.commandFailed(coll.getDB().runCommand({listIndexes: []}));

    //
    // Test basic usage of "cursor.batchSize" option.
    //

    coll.drop();
    assert.commandWorked(coll.getDB().createCollection(coll.getName()));
    assert.commandWorked(coll.ensureIndex({a: 1}, {unique: true}));

    cursor = getListIndexesCursor(coll, {cursor: {batchSize: 2}});
    assert.eq(2, cursor.objsLeftInBatch());
    assert.eq(["_id_", "a_1"], cursorGetIndexNames(cursor));

    cursor = getListIndexesCursor(coll, {cursor: {batchSize: 1}});
    assert.eq(1, cursor.objsLeftInBatch());
    assert.eq(["_id_", "a_1"], cursorGetIndexNames(cursor));

    cursor = getListIndexesCursor(coll, {cursor: {batchSize: 0}});
    assert.eq(0, cursor.objsLeftInBatch());
    assert.eq(["_id_", "a_1"], cursorGetIndexNames(cursor));

    cursor = getListIndexesCursor(coll, {cursor: {batchSize: NumberInt(2)}});
    assert.eq(2, cursor.objsLeftInBatch());
    assert.eq(["_id_", "a_1"], cursorGetIndexNames(cursor));

    cursor = getListIndexesCursor(coll, {cursor: {batchSize: NumberLong(2)}});
    assert.eq(2, cursor.objsLeftInBatch());
    assert.eq(["_id_", "a_1"], cursorGetIndexNames(cursor));

    cursor = getListIndexesCursor(coll, {cursor: {batchSize: Math.pow(2, 62)}});
    assert.eq(2, cursor.objsLeftInBatch());
    assert.eq(["_id_", "a_1"], cursorGetIndexNames(cursor));

    // Ensure that the server accepts an empty object for "cursor".  This is equivalent to not
    // specifying "cursor" at all.
    //
    // We do not test for objsLeftInBatch() here, since the default batch size for this command is
    // not specified.
    cursor = getListIndexesCursor(coll, {cursor: {}});
    assert.eq(["_id_", "a_1"], cursorGetIndexNames(cursor));

    //
    // Test more than 2 batches of results.
    //

    coll.drop();
    assert.commandWorked(coll.getDB().createCollection(coll.getName()));
    assert.commandWorked(coll.ensureIndex({a: 1}, {unique: true}));
    assert.commandWorked(coll.ensureIndex({b: 1}, {unique: true}));
    assert.commandWorked(coll.ensureIndex({c: 1}, {unique: true}));

    cursor = getListIndexesCursor(coll, {cursor: {batchSize: 0}}, 2);
    assert.eq(0, cursor.objsLeftInBatch());
    assert(cursor.hasNext());
    assert.eq(2, cursor.objsLeftInBatch());

    cursor.next();
    assert(cursor.hasNext());
    assert.eq(1, cursor.objsLeftInBatch());

    cursor.next();
    assert(cursor.hasNext());
    assert.eq(2, cursor.objsLeftInBatch());

    cursor.next();
    assert(cursor.hasNext());
    assert.eq(1, cursor.objsLeftInBatch());

    cursor.next();
    assert(!cursor.hasNext());

    //
    // Test on collection with no indexes.
    //

    coll.drop();
    assert.commandWorked(coll.getDB().createCollection(coll.getName(), {autoIndexId: false}));
    assert.eq([], cursorGetIndexNames(getListIndexesCursor(coll)));

    //
    // Test killCursors against a listCollections cursor.
    //

    coll.drop();
    assert.commandWorked(coll.getDB().createCollection(coll.getName()));
    assert.commandWorked(coll.ensureIndex({a: 1}, {unique: true}));
    assert.commandWorked(coll.ensureIndex({b: 1}, {unique: true}));
    assert.commandWorked(coll.ensureIndex({c: 1}, {unique: true}));

    res = coll.runCommand("listIndexes", {cursor: {batchSize: 0}});
    cursor = new DBCommandCursor(coll.getDB().getMongo(), res, 2);
    cursor.close();
    cursor = new DBCommandCursor(coll.getDB().getMongo(), res, 2);
    assert.throws(function() {
        cursor.hasNext();
    });
}());

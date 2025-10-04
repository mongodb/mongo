// Cannot implicitly shard accessed collections because renameCollection command not supported
// on sharded collections.
// @tags: [
//   assumes_unsharded_collection,
//   requires_non_retryable_commands,
//   requires_fastcount,
//   requires_getmore
// ]

let collName = "system_indexes_invalidations";
let collNameRenamed = "renamed_collection";
let coll = db[collName];
let collRenamed = db[collNameRenamed];

function testIndexInvalidation(isRename) {
    coll.drop();
    collRenamed.drop();
    assert.commandWorked(coll.createIndexes([{a: 1}, {b: 1}, {c: 1}]));

    // Get the first two indexes.
    let cmd = {listIndexes: collName};
    Object.extend(cmd, {cursor: {batchSize: 2}});
    let res = db.runCommand(cmd);
    assert.commandWorked(res, "could not run " + tojson(cmd));
    printjson(res);

    // Ensure the cursor has data, rename or drop the collection, and exhaust the cursor.
    let cursor = new DBCommandCursor(db, res);
    let errMsg = "expected more data from command " + tojson(cmd) + ", with result " + tojson(res);
    assert(cursor.hasNext(), errMsg);
    assert(res.cursor.id !== NumberLong("0"), errMsg);
    if (isRename) {
        assert.commandWorked(coll.renameCollection(collNameRenamed));
    } else {
        assert(coll.drop());
    }
    assert.gt(cursor.itcount(), 0, errMsg);
}

// Test that we invalidate indexes for both collection drops and renames.
testIndexInvalidation(false);
testIndexInvalidation(true);

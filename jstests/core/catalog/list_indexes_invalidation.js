// Cannot implicitly shard accessed collections because renameCollection command not supported
// on sharded collections.
// @tags: [
//   requires_non_retryable_commands,
//   requires_fastcount,
//   requires_getmore,
//   # listIndexes command is not running all its getMore commands sequentially, causing
//   # network_error_and_txn_override.js to fail since getMore may be executed as the first command
//   # in a transaction.
//   does_not_support_transactions,
// ]

let collName = "system_indexes_invalidations";
let collNameRenamed = "renamed_collection";
let coll = db[collName];
let collRenamed = db[collNameRenamed];

// Detect if collections are implicitly sharded
const isImplicitlyShardedCollection = typeof globalThis.ImplicitlyShardAccessCollSettings !== "undefined";

function dropCollectionWithoutImplicitRecreate(coll) {
    if (isImplicitlyShardedCollection) {
        const originalImplicitlyShardOnCreateCollectionOnly = TestData.implicitlyShardOnCreateCollectionOnly;
        try {
            TestData.implicitlyShardOnCreateCollectionOnly = true;
            assert(coll.drop());
        } finally {
            TestData.implicitlyShardOnCreateCollectionOnly = originalImplicitlyShardOnCreateCollectionOnly;
        }
    } else {
        assert(coll.drop());
    }
}

function testIndexInvalidation(isRename) {
    dropCollectionWithoutImplicitRecreate(collRenamed);

    coll.drop();
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

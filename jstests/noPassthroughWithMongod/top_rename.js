/**
 * Ensures that the data managed by Top is removed for the source namespace when renaming the
 * collection.
 *
 * @tags: [incompatible_with_embedded]
 */
(function() {

load("jstests/libs/stats.js");

const dbName = "top_rename";
const sourceCollection = "source";
const destCollection = "dest";
const mapReduceCollection = "final";

const testDB = db.getSiblingDB(dbName);
testDB.getCollection(sourceCollection).drop();
testDB.getCollection(destCollection).drop();
testDB.getCollection(mapReduceCollection).drop();

assert.commandWorked(testDB.createCollection(sourceCollection));
assert.doesNotThrow(() => {
    getTop(testDB.getCollection(sourceCollection));
});

assert.commandWorked(testDB.adminCommand(
    {renameCollection: dbName + "." + sourceCollection, to: dbName + "." + destCollection}));
assert.throws(() => {
    getTop(testDB.getCollection(sourceCollection));
});

// Perform an operation on the renamed collection so that it is present in the "top" command's
// output.
assert.commandWorked(testDB.getCollection(destCollection).insert({}));
assert.doesNotThrow(() => {
    getTop(testDB.getCollection(destCollection));
});

// MapReduce creates temporary collections before renaming them in place. Test that these temporary
// collections are not kept in Top's output after the command finishes.
assert.commandWorked(
    testDB.getCollection(destCollection)
        .mapReduce(function() {}, function() {}, {out: {merge: mapReduceCollection}}));

const top = testDB.adminCommand("top");
assert.commandWorked(top);

const mapReduceTmp = ".tmp.";
for (const collection in top.totals) {
    assert.eq(false, collection.includes(mapReduceTmp));
}
}());

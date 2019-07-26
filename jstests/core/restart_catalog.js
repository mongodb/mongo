/**
 * Forces the server to restart the catalog and rebuild its in-memory catalog data structures, then
 * asserts that the server works normally.
 * @tags: [
 *  assumes_read_concern_unchanged, requires_majority_read_concern,
 *
 *  # restartCatalog command is not available on embedded
 *  incompatible_with_embedded,
 *
 *  # This test assumes that reads happen on the same node as the 'restartCatalog' command.
 *  assumes_read_preference_unchanged
 * ]
 */
(function() {
"use strict";

// Only run this test if the storage engine is "wiredTiger" or "inMemory".
const acceptedStorageEngines = ["wiredTiger", "inMemory"];
const currentStorageEngine = jsTest.options().storageEngine || "wiredTiger";
if (!acceptedStorageEngines.includes(currentStorageEngine)) {
    jsTest.log("Refusing to run restartCatalog test on " + currentStorageEngine +
               " storage engine");
    return;
}

// Helper function for sorting documents in JavaScript.
function sortOnId(doc1, doc2) {
    return bsonWoCompare({_: doc1._id}, {_: doc2._id});
}

const testDB = db.getSiblingDB("restart_catalog");
const artistsColl = testDB.getCollection("artists");
const songsColl = testDB.getCollection("songs");
artistsColl.drop();
songsColl.drop();

// Populate some data into the collection.
const artists = [
    {_id: "beyonce"},
    {_id: "fenech-soler"},
    {_id: "gallant"},
];
for (let artist of artists) {
    assert.commandWorked(artistsColl.insert(artist));
}

const songs = [
    {_id: "flawless", artist: "beyonce", sales: 5000},
    {_id: "conversation", artist: "fenech-soler", sales: 75.5},
    {_id: "kaleidoscope", artist: "fenech-soler", sales: 30.0},
    {_id: "miyazaki", artist: "gallant", sales: 400.3},
    {_id: "percogesic", artist: "gallant", sales: 550.8},
    {_id: "shotgun", artist: "gallant", sales: 300.0},
];
for (let song of songs) {
    assert.commandWorked(songsColl.insert(song, {writeConcern: {w: "majority"}}));
}

// Perform some queries.
function assertQueriesFindExpectedData() {
    assert.eq(artistsColl.find().sort({_id: 1}).toArray(), artists);
    assert.eq(songsColl.find().sort({_id: 1}).toArray(), songs.sort(sortOnId));

    const songsWithLotsOfSales = songs.filter(song => song.sales > 500).sort(sortOnId);
    assert.eq(songsColl.find({sales: {$gt: 500}}).sort({_id: 1}).toArray(), songsWithLotsOfSales);

    const songsByGallant = songs.filter(song => song.artist === "gallant").sort(sortOnId);
    assert.eq(songsColl.aggregate([{$match: {artist: "gallant"}}, {$sort: {_id: 1}}]).toArray(),
              songsByGallant);

    const initialValue = 0;
    const totalSales = songs.reduce((total, song) => total + song.sales, initialValue);
    assert.eq(songsColl
                  .aggregate([{$group: {_id: null, totalSales: {$sum: "$sales"}}}],
                             {readConcern: {level: "majority"}})
                  .toArray(),
              [{_id: null, totalSales: totalSales}]);
}
assertQueriesFindExpectedData();

// Remember what indexes are present, then restart the catalog.
const songIndexesBeforeRestart = songsColl.getIndexes().sort(sortOnId);
const artistIndexesBeforeRestart = artistsColl.getIndexes().sort(sortOnId);
assert.commandWorked(db.adminCommand({restartCatalog: 1}));

// Access the query plan cache. (This makes no assumptions about the state of the plan cache
// after restart; however, the database definitely should not crash.)
[songsColl, artistsColl].forEach(coll => {
    assert.commandWorked(coll.runCommand("planCacheListPlans", {query: {_id: 1}}));
    assert.commandWorked(coll.runCommand("planCacheListQueryShapes"));
    assert.commandWorked(coll.runCommand("planCacheClear"));
});

// Verify that the data in the collections has not changed.
assertQueriesFindExpectedData();

// Verify that both collections have the same indexes as prior to the restart.
const songIndexesAfterRestart = songsColl.getIndexes().sort(sortOnId);
assert.eq(songIndexesBeforeRestart, songIndexesAfterRestart);
const artistIndexesAfterRestart = artistsColl.getIndexes().sort(sortOnId);
assert.eq(artistIndexesBeforeRestart, artistIndexesAfterRestart);

// Create new indexes and run more queries.
assert.commandWorked(songsColl.createIndex({sales: 1}));
assert.commandWorked(songsColl.createIndex({artist: 1, sales: 1}));
assertQueriesFindExpectedData();

// Modify an existing collection.
assert.commandWorked(artistsColl.runCommand("collMod", {validator: {_id: {$type: "string"}}}));
assert.writeErrorWithCode(artistsColl.insert({_id: 7}), ErrorCodes.DocumentValidationFailure);

// Perform another write, implicitly creating a new collection and database.
const secondTestDB = db.getSiblingDB("restart_catalog_2");
const foodColl = secondTestDB.getCollection("food");
foodColl.drop();
const doc = {
    _id: "apple",
    category: "fruit"
};
assert.commandWorked(foodColl.insert(doc));
assert.eq(foodColl.find().toArray(), [doc]);

// Build a new index on the new collection.
assert.commandWorked(foodColl.createIndex({category: -1}));
assert.eq(foodColl.find().hint({category: -1}).toArray(), [doc]);

// The restartCatalog command kills all cursors. Test that a getMore on a cursor that existed
// during restartCatalog fails with the appropriate error code. We insert a second document so
// that we can make a query happen in two batches.
assert.commandWorked(foodColl.insert({_id: "orange"}));
let cursorResponse = assert.commandWorked(
    secondTestDB.runCommand({find: foodColl.getName(), filter: {}, batchSize: 1}));
assert.eq(cursorResponse.cursor.firstBatch.length, 1);
assert.neq(cursorResponse.cursor.id, 0);
assert.commandWorked(secondTestDB.adminCommand({restartCatalog: 1}));
assert.commandFailedWithCode(
    secondTestDB.runCommand({getMore: cursorResponse.cursor.id, collection: foodColl.getName()}),
    ErrorCodes.QueryPlanKilled);
}());

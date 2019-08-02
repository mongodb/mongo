/**
 * Tests varying lengths of large collection names to ensure that the system continues to operate as
 * expected when handling large collection names.
 *
 * @tags: [requires_collstats]
 */
(function() {
'use strict';

const name = "long_collection_names";
const testDB = db.getSiblingDB(name);

function doTest(collection, ns) {
    assert.eq(collection.stats().ns, ns);

    for (let i = 0; i < 5; i++) {
        assert.commandWorked(collection.insert({a: i}));
        assert.commandWorked(collection.insert({b: i * i}));
    }

    assert.commandWorked(collection.createIndexes([{a: 1}, {b: 1}]));
    assert.eq(true, collection.drop());
}

// Case 1: Collection name is well below the BSON document limit. We'll use collection names of
// large sizes that are common page sizes in WiredTiger.
const wiredTigerCommonPageSizes = [4000, 32000, 128000];
wiredTigerCommonPageSizes.forEach((pageSize) => {
    const collectionName = 'a'.repeat(pageSize);

    assert.commandWorked(testDB.createCollection(collectionName));
    const collection = testDB.getCollection(collectionName);
    doTest(collection, testDB + "." + collectionName);
});

// Case 2: Collection names are large but well below the BSON document limit. There is an implicit
// limit set by the BSON document limit, but there is also other overhead we need to take into
// consideration when using large collection names.
const largeCollectionSizes = [1000000, 2000000, 4000000];
largeCollectionSizes.forEach((collSize) => {
    const collectionName = 'b'.repeat(collSize);

    assert.commandWorked(testDB.createCollection(collectionName));
    const collection = testDB.getCollection(collectionName);
    doTest(collection, testDB + "." + collectionName);
});

// Case 3: The collection name is large enough to exceed the BSON document limit when adding a new
// entry into the DurableCatalog.
const maxBsonObjectSize = testDB.isMaster().maxBsonObjectSize;
const veryLargeCollectionSizes = [maxBsonObjectSize - 1, maxBsonObjectSize];
veryLargeCollectionSizes.forEach((collSize) => {
    const collectionName = 'c'.repeat(collSize);

    assert.commandFailedWithCode(
        testDB.runCommand({create: collectionName}),
        [ErrorCodes.BSONObjectTooLarge, 17420 /* Document to upsert is too large */]);
});
}());

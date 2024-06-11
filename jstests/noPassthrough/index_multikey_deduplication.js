/**
 * Tests that IndexScan deduplicates RecordIds using Roaring Bitmaps correctly.
 */

const conn = MongoRunner.runMongod({
    setParameter: {
        internalUseRoaringBitmapsForRecordIDDeduplication: true,
        // Set the threshold small to force HashRoaringSet's transition earlier.
        internalRoaringBitmapsThreshold: 100,
        internalRoaringBitmapsMinimalDensity: 0.00001,
        // Set the batch small to keep the transition process for longer.
        internalRoaringBitmapsBatchSize: 1,
    }
});

function assertNumberOfDocuments(coll, query, expectedNumberOfDocuments) {
    const result = coll.find(query).toArray();
    assert.eq(result.length, expectedNumberOfDocuments);
}

//*********************************************************
// Prepare test data.

Random.setRandomSeed(358177015);

const docs = [];
for (let i = 0; i < 300; ++i) {
    const array = Array.from({length: 100}, (_, j) => i + j);
    Array.shuffle(array);
    docs.push({i, multiKey: array});
}
Array.shuffle(docs);
docs.forEach((doc, index) => {
    doc.stringIndex = `index${index + 1000}`;
});

// This query should return 249 documents from {i: 51} to {i: 299}
const query = {
    multiKey: {$gte: 150}
};
const expectedNumberOfDocuments = 249;

//*********************************************************
// Prepare test collections.

const db = conn.getDB('roaring');

// Ordinary collection.
db.coll.drop();
assert.commandWorked(db.coll.insertMany(docs));

// Collection with a clustered index.
db.clusteredColl.drop();
db.createCollection(
    'clusteredColl',
    {clusteredIndex: {key: {stringIndex: 1}, unique: true, name: 'clustered_index'}});
db.clusteredColl.insertMany(docs);

//*********************************************************
// Case 1. Sanity check: collection scan.
assertNumberOfDocuments(db.coll, query, expectedNumberOfDocuments);
assertNumberOfDocuments(db.clusteredColl, query, expectedNumberOfDocuments);

// Must use index.
assert.commandWorked(db.adminCommand({setParameter: 1, notablescan: true}));

//*********************************************************
// Case 2. Test that IndexScan correctly deduplicates documents.
assert.commandWorked(db.coll.createIndex({multiKey: 1}));
assertNumberOfDocuments(db.coll, query, expectedNumberOfDocuments);

//*********************************************************
// Case 3. Test that IndexScan on a collection with a clustered index correctly deduplicates
// documents.
assert.commandWorked(db.clusteredColl.createIndex({multiKey: 1}));
assertNumberOfDocuments(db.clusteredColl, query, expectedNumberOfDocuments);

MongoRunner.stopMongod(conn);

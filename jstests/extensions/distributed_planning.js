/**
 * Tests a source stage that reads documents from disk. $readNDocuments desugars to a source stage that produces _ids
 * followed by an idLookup, so it will mimic reading from a collection with a limit.
 *
 * This test exists to verify the pipeline splitting behavior for a source stage followed by idLookup in sharded contexts.
 *
 * @tags: [featureFlagExtensionsAPI]
 */

const collName = jsTestName();
const coll = db[collName];
coll.drop();

const documents = [
    {_id: 0, dog: "labradoodle"},
    {_id: 1, dog: "golden retriever"},
    {_id: 2, dog: "cavalier king charles spaniel"},
    {_id: 3, dog: "bichon frise"},
];
coll.insertMany(documents);

// EOF case.
let results = coll.aggregate([{$readNDocuments: {numDocs: 0}}]).toArray();
assert.eq(results.length, 0, results);

// Return some documents in the collection.
results = coll.aggregate([{$readNDocuments: {numDocs: 2}}]).toArray();
assert.sameMembers(results, documents.slice(0, 2));

// Return all documents in the collection. Since it should be using idLookup, it can't return more than what is in the collection.
results = coll.aggregate([{$readNDocuments: {numDocs: 6}}]).toArray();
assert.sameMembers(results, documents);

// Return all documents in the collection, sorted by _id.
results = coll.aggregate([{$readNDocuments: {numDocs: 6, sortById: true}}]).toArray();
assert.eq(results, documents);

// TODO SERVER-113930 Test in lookup and unionWith.
// results = coll.aggregate([{$sort: {_id: 1}}, {$limit: 1}, {$lookup: {from: collName, pipeline: [{$readNDocuments: {numDocs: 2, sortById: true}}], as: "dogs"}}]).toArray();
// assert.eq(results, [{_id: 0, dog: "labradoodle", dogs: documents.slice(0, 2)}]);

// results = coll.aggregate([{$sort: {_id: 1}}, {$limit: 1}, {$lookup: {from: collName, pipeline: [{$readNDocuments: {numDocs: 2}}], as: "dogs"}}]).toArray();
// assert.eq(results.length, 1, results);
// assert(results[0].hasOwnProperty("dogs"));
// assert.sameMembers(documents.slice(0, 2), results[0].dogs);

// results = coll.aggregate([{$unionWith: {coll: collName, pipeline: [{$readNDocuments: {numDocs: 2}}]}}]).toArray();
// assert.sameMembers(results, documents.concat(documents.slice(0, 2)));
// results = coll.aggregate([{$unionWith: {coll: collName, pipeline: [{$readNDocuments: {numDocs: 2, sortById: true}}]}}]).toArray();
// assert.eq(results, documents.concat(documents.slice(0, 2)));

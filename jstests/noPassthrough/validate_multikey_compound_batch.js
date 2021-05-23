/**
 * Validates multikey compound index in a collection with a single insert command
 * containing documents that update different paths in the multikey index.
 *
 * @tags: [
 *     requires_replication,
 * ]
 */
(function() {
'use strict';

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

let primary = rst.getPrimary();
let testColl = primary.getCollection('test.validate_multikey_compound_batch');

assert.commandWorked(testColl.getDB().createCollection(testColl.getName()));

assert.commandWorked(testColl.createIndex({a: 1, b: 1}));

// Insert 2 documents. Only the first and last documents are valid.
assert.commandWorked(
    testColl.insert([{_id: 0, a: [1, 2, 3], b: 'abc'}, {_id: 1, a: 456, b: ['d', 'e', 'f']}]));

jsTestLog('Checking documents in collection');
let docs = testColl.find().sort({_id: 1}).toArray();
assert.eq(2, docs.length, 'too many docs in collection: ' + tojson(docs));
assert.eq(0, docs[0]._id, 'unexpected document content in collection: ' + tojson(docs));
assert.eq(1, docs[1]._id, 'unexpected document content in collection: ' + tojson(docs));

jsTestLog('Validating collection');
const result = assert.commandWorked(testColl.validate({full: true}));

jsTestLog('Validation result: ' + tojson(result));
assert.eq(testColl.getFullName(), result.ns, tojson(result));
assert.eq(0, result.nInvalidDocuments, tojson(result));
assert.eq(2, result.nrecords, tojson(result));
assert.eq(2, result.nIndexes, tojson(result));

// Check non-multikey indexes.
assert.eq(2, result.keysPerIndex._id_, tojson(result));
assert(result.indexDetails._id_.valid, tojson(result));

// Check multikey index.
assert.eq(6, result.keysPerIndex.a_1_b_1, tojson(result));
assert(result.indexDetails.a_1_b_1.valid, tojson(result));

assert(result.valid, tojson(result));

rst.stopSet();
})();

/**
 * Validates multikey index in a collection that also contains a hashed index.
 * The scenario tested here involves restarting the node after a failed insert due
 * to contraints imposed by the hashed index. The validation behavior observed after
 * restarting is that, even though we inserted another document with an array in the
 * indexed field for the non-hashed index, we fail to save the index state as multikey.
 * @tags: [
 *     requires_replication,
 *     requires_persistence,
 * ]
 */
(function() {
'use strict';

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

let primary = rst.getPrimary();
let testColl = primary.getCollection('test.validate_multikey_restart');

assert.commandWorked(testColl.getDB().createCollection(testColl.getName()));

assert.commandWorked(testColl.createIndex({a: 1}));
assert.commandWorked(testColl.createIndex({b: 'hashed'}));

// 16766 is the error code returned by ExpressionKeysPrivate::getHashKeys() for
// "hashed indexes do not currently support array values".
assert.commandFailedWithCode(testColl.insert({_id: 0, a: [1, 2, 3], b: ['a', 'b', 'c']}), 16766);

assert.commandWorked(testColl.insert({_id: 1, a: [4, 5, 6], b: 'def'}));

jsTestLog('Checking documents in collection before restart');
let docs = testColl.find().sort({_id: 1}).toArray();
assert.eq(1, docs.length, 'too many docs in collection: ' + tojson(docs));
assert.eq(1, docs[0]._id, 'unexpected document content in collection: ' + tojson(docs));

jsTestLog('Checking multikey query before restart');
let multikeyQueryDocs = testColl.find({a: {$in: [4, 5, 6]}}).toArray();
assert.eq(1,
          multikeyQueryDocs.length,
          'too many docs in multikey query result: ' + tojson(multikeyQueryDocs));
assert.eq(1,
          multikeyQueryDocs[0]._id,
          'unexpected document content in multikey query result: ' + tojson(multikeyQueryDocs));

// For the purpose of reproducing the validation error in a_1, it is important to skip validation
// when restarting the primary node. Enabling validation here has an effect on the validate
// command's behavior after restarting.
primary = rst.restart(primary, {skipValidation: true}, /*signal=*/undefined, /*wait=*/true);
testColl = primary.getCollection(testColl.getFullName());

jsTestLog('Checking documents in collection after restart');
rst.awaitReplication();
docs = testColl.find().sort({_id: 1}).toArray();
assert.eq(1, docs.length, 'too many docs in collection: ' + tojson(docs));
assert.eq(1, docs[0]._id, 'unexpected document content in collection: ' + tojson(docs));

jsTestLog('Checking multikey query after restart');
multikeyQueryDocs = testColl.find({a: {$in: [4, 5, 6]}}).toArray();
assert.eq(1,
          multikeyQueryDocs.length,
          'too many docs in multikey query result: ' + tojson(multikeyQueryDocs));
assert.eq(1,
          multikeyQueryDocs[0]._id,
          'unexpected document content in multikey query result: ' + tojson(multikeyQueryDocs));

jsTestLog('Validating collection after restart');
const result = assert.commandWorked(testColl.validate({full: true}));

jsTestLog('Validation result: ' + tojson(result));
assert.eq(testColl.getFullName(), result.ns, tojson(result));
assert.eq(0, result.nInvalidDocuments, tojson(result));
assert.eq(1, result.nrecords, tojson(result));
assert.eq(3, result.nIndexes, tojson(result));

// Check non-multikey indexes.
assert.eq(1, result.keysPerIndex._id_, tojson(result));
assert.eq(1, result.keysPerIndex.b_hashed, tojson(result));
assert(result.indexDetails._id_.valid, tojson(result));
assert(result.indexDetails.b_hashed.valid, tojson(result));

// Check multikey index.
assert.eq(3, result.keysPerIndex.a_1, tojson(result));
assert(result.indexDetails.a_1.valid, tojson(result));

assert(result.valid, tojson(result));

rst.stopSet();
})();

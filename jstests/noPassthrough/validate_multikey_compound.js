/**
 * Validates multikey compound index in a collection that also contains a hashed index.
 * The scenario tested here involves restarting the node after a failed insert due
 * to contraints imposed by the hashed index. The validation behavior observed after
 * restarting is that, even though we inserted documents that contain arrays in every
 * indexed field in the compound index, we fail to save all the multikey path information
 * in the catalog.
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
let testColl = primary.getCollection('test.validate_multikey_compound');

assert.commandWorked(testColl.getDB().createCollection(testColl.getName()));

assert.commandWorked(testColl.createIndex({a: 1, b: 1}));
assert.commandWorked(testColl.createIndex({c: 'hashed'}));

// Insert 3 documents. Only the first and last documents are valid.
assert.commandWorked(testColl.insert({_id: 0, a: [1, 2, 3], b: 'abc', c: 'valid_hash_0'}));

// 16766 is the error code returned by ExpressionKeysPrivate::getHashKeys() for
// "hashed indexes do not currently support array values".
assert.commandFailedWithCode(
    testColl.insert({_id: 1, a: 456, b: ['d', 'e', 'f'], c: ['invalid', 'hash']}), 16766);

assert.commandWorked(testColl.insert({_id: 2, a: 789, b: ['g', 'h', ' i'], c: 'valid_hash_2'}));

jsTestLog('Checking documents in collection before restart');
let docs = testColl.find().sort({_id: 1}).toArray();
assert.eq(2, docs.length, 'too many docs in collection: ' + tojson(docs));
assert.eq(0, docs[0]._id, 'unexpected document content in collection: ' + tojson(docs));
assert.eq(2, docs[1]._id, 'unexpected document content in collection: ' + tojson(docs));

// For the purpose of reproducing the validation error in a_1, it is important to skip validation
// when restarting the primary node. Enabling validation here has an effect on the validate
// command's behavior after restarting.
primary = rst.restart(primary, {skipValidation: true}, /*signal=*/undefined, /*wait=*/true);
testColl = primary.getCollection(testColl.getFullName());

jsTestLog('Checking documents in collection after restart');
rst.awaitReplication();
docs = testColl.find().sort({_id: 1}).toArray();
assert.eq(2, docs.length, 'too many docs in collection: ' + tojson(docs));
assert.eq(0, docs[0]._id, 'unexpected document content in collection: ' + tojson(docs));
assert.eq(2, docs[1]._id, 'unexpected document content in collection: ' + tojson(docs));

jsTestLog('Validating collection after restart');
const result = assert.commandWorked(testColl.validate({full: true}));

jsTestLog('Validation result: ' + tojson(result));
assert.eq(testColl.getFullName(), result.ns, tojson(result));
assert.eq(0, result.nInvalidDocuments, tojson(result));
assert.eq(2, result.nrecords, tojson(result));
assert.eq(3, result.nIndexes, tojson(result));

// Check non-multikey indexes.
assert.eq(2, result.keysPerIndex._id_, tojson(result));
assert.eq(2, result.keysPerIndex.c_hashed, tojson(result));
assert(result.indexDetails._id_.valid, tojson(result));
assert(result.indexDetails.c_hashed.valid, tojson(result));

// Check multikey index.
assert.eq(6, result.keysPerIndex.a_1_b_1, tojson(result));
assert(result.indexDetails.a_1_b_1.valid, tojson(result));

assert(result.valid, tojson(result));

rst.stopSet();
})();

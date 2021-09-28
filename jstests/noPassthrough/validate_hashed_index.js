/**
 * Tests that validate completes without warnings on a collection with a hashed index after a
 * failed insert (due to hashed index restrictions).
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
let testColl = primary.getCollection('test.validate_hashed_index');

assert.commandWorked(testColl.getDB().createCollection(testColl.getName()));

assert.commandWorked(testColl.createIndex({a: 'hashed'}));

// Log WiredTigerRecoveryUnit timestamps during failed insert.
const previousLogLevel = primary.setLogLevel(3, 'storage').was.storage.verbosity;

// 16766 is the error code returned by ExpressionKeysPrivate::getHashKeys() for
// "hashed indexes do not currently support array values".
assert.commandFailedWithCode(testColl.insert({_id: 0, a: ['a', 'b', 'c']}), 16766);

primary.setLogLevel(previousLogLevel, 'storage');

jsTestLog('Checking documents in collection');
let docs = testColl.find().sort({_id: 1}).toArray();
assert.eq(0, docs.length, 'too many docs in collection: ' + tojson(docs));

jsTestLog('Validating collection until we no longer see "transient - collection in use" warnings');
assert.soon(() => {
    const result = assert.commandWorked(testColl.validate({full: true}));
    jsTestLog('Validation result: ' + tojson(result));
    assert.eq(testColl.getFullName(), result.ns);
    assert.eq(0, result.nInvalidDocuments);
    assert.eq(0, result.nrecords);
    assert.eq(2, result.nIndexes);

    assert.eq(0, result.keysPerIndex._id_);
    assert.eq(0, result.keysPerIndex.a_hashed);
    assert(result.indexDetails._id_.valid);
    assert(result.indexDetails.a_hashed.valid);

    assert(result.valid);

    if (result.hasOwnProperty('warnings') && result.warnings.length > 0) {
        jsTestLog('Validation reported warnings - retrying after fsync: ' +
                  tojson(result.warnings));
        assert.commandWorked(primary.adminCommand({fsync: 1}));
        return false;
    }

    return true;
}, 'full validation reported warnings');

rst.stopSet();
})();

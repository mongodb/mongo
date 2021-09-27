/**
 * Tests that validate completes without warnings on an empty collection.
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
let testColl = primary.getCollection('test.validate_empty_collection');

// Log WiredTigerRecoveryUnit timestamps during collection creation.
const previousLogLevel = primary.setLogLevel(3, 'storage').was.storage.verbosity;
assert.commandWorked(testColl.getDB().createCollection(testColl.getName()));
primary.setLogLevel(previousLogLevel, 'storage');

jsTestLog('Checking documents in collection');
let docs = testColl.find().sort({_id: 1}).toArray();
assert.eq(0, docs.length, 'too many docs in collection: ' + tojson(docs));

jsTestLog('Validating collection until we no longer see "transient - collection in use" warnings');
assert.soon(() => {
    const result = assert.commandWorked(testColl.validate({full: true}));
    jsTestLog('Validation result: ' + tojson(result));

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

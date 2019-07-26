/**
 * Long index namespaces exceeding 127 characters are supported starting in 4.2.
 * However, we should still disallow long index namespaces under FCV 4.0.
 * TODO: remove this test in 4.4.
 */
(function() {
'use strict';

TestData.replSetFeatureCompatibilityVersion = '4.0';
const rst = new ReplSetTest({
    nodes: [
        {binVersion: 'latest'},
        {rsConfig: {priority: 0, votes: 0}},
    ]
});
rst.startSet();
rst.initiate();
rst.restart(1, {binVersion: '4.0'});

const primary = rst.getPrimary();
const mydb = primary.getDB('test');
const coll = mydb.getCollection('long_index_name');

// Compute maximum index name length for this collection under FCV 4.0.
const maxNsLength = 127;
const maxIndexNameLength = maxNsLength - (coll.getFullName() + ".$").length;
jsTestLog('Max index name length under FCV 4.0 = ' + maxIndexNameLength);

// Create an index with the longest name allowed for this collection.
assert.commandWorked(coll.createIndex({a: 1}, {name: 'a'.repeat(maxIndexNameLength)}));

// If this command succeeds unexpectedly, it will cause an fassert on the 4.0 secondary which
// cannot handle long index namespaces, with a "CannotCreateIndex: ... index name ... too long"
// error message.
assert.commandFailedWithCode(coll.createIndex({b: 1}, {name: 'b'.repeat(maxIndexNameLength + 1)}),
                             ErrorCodes.CannotCreateIndex);

// The existing index on {x: 1} has an index name that is the longest supported under FCV 4.0
// for the current collection name.
// Any attempt to rename this collection with a longer name must fail. Otherwise, the invalid
// index namespace will cause the 4.0 secondary to fassert.
assert.commandFailedWithCode(coll.renameCollection(coll.getName() + 'z'), ErrorCodes.InvalidLength);

rst.stopSet();
})();

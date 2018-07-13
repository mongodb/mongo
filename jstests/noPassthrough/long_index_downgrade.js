/**
 * Long index namespaces exceeding 127 characters are supported starting in 4.2.
 * Since these are not supported in 4.0, we should not allow a server to be downgraded to FCV 4.0
 * when there are indexes with long namespaces.
 * @tags: [requires_replication]
 * TODO: remove this test in 4.4.
 */
(function() {
    'use strict';

    const rst = new ReplSetTest({nodes: 1});

    rst.startSet();
    rst.initiate();

    const primary = rst.getPrimary();
    const mydb = primary.getDB('test');
    const coll = mydb.getCollection('long_index_name');

    // Compute maximum index name length for this collection under FCV 4.0.
    const maxNsLength = 127;
    const maxIndexNameLength = maxNsLength - (coll.getFullName() + ".$").length;
    jsTestLog('Max index name length under FCV 4.0 = ' + maxIndexNameLength);

    // Create an index with the longest name allowed for this collection under FCV 4.0.
    assert.commandWorked(coll.createIndex({a: 1}, {name: 'a'.repeat(maxIndexNameLength)}));

    // Create an index with a name that exceeds the limit in 4.0.
    const longName = 'b'.repeat(maxIndexNameLength + 1);
    assert.commandWorked(coll.createIndex({b: 1}, {name: longName}));

    // Downgrades should fail while we have indexes that are not compatible with 4.0.
    assert.commandFailedWithCode(mydb.adminCommand({setFeatureCompatibilityVersion: '4.0'}),
                                 ErrorCodes.IndexNamespaceTooLong);

    // Drop index with long name before retrying downgrade.
    assert.commandWorked(coll.dropIndex(longName));
    assert.commandWorked(mydb.adminCommand({setFeatureCompatibilityVersion: '4.0'}));

    rst.stopSet();
})();

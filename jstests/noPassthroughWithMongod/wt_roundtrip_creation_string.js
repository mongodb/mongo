/**
 * Tests that the creation string returned by the collStats command can be used to create a
 * collection or index with the same WiredTiger options.
 */
(function() {
    'use strict';

    // Skip this test if not running with the "wiredTiger" storage engine.
    if (db.serverStatus().storageEngine.name !== 'wiredTiger') {
        jsTest.log('Skipping test because storageEngine is not "wiredTiger"');
        return;
    }

    var collNamePrefix = 'wt_roundtrip_creation_string';

    // Drop the collections used by the test to ensure that the create commands don't fail because
    // the collections already exist.
    db[collNamePrefix].source.drop();
    db[collNamePrefix].dest.drop();

    assert.commandWorked(db.createCollection(collNamePrefix + '.source'));
    assert.commandWorked(db[collNamePrefix].source.createIndex({a: 1}, {name: 'a_1'}));

    var collStats = db.runCommand({collStats: collNamePrefix + '.source'});
    assert.commandWorked(collStats);

    assert.commandWorked(
        db.runCommand({
            create: collNamePrefix + '.dest',
            storageEngine: {wiredTiger: {configString: collStats.wiredTiger.creationString}}
        }),
        'unable to create collection using the creation string of another collection');

    assert.commandWorked(db.runCommand({
        createIndexes: collNamePrefix + '.dest',
        indexes: [{
            key: {b: 1},
            name: 'b_1',
            storageEngine:
                {wiredTiger: {configString: collStats.indexDetails.a_1.creationString}}
        }]
    }),
                         'unable to create index using the creation string of another index');
})();

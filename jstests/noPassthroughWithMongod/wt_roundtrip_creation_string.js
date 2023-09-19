/**
 * Tests that the creation string returned by the collStats command can be used to create a
 * collection or index with the same WiredTiger options.
 */
// Skip this test if not running with the "wiredTiger" storage engine.
if (db.serverStatus().storageEngine.name !== 'wiredTiger') {
    jsTest.log('Skipping test because storageEngine is not "wiredTiger"');
    quit();
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

const encryptionRegex = /,encryption=\(?[^)]*,?\),/;
const sanitisedCollConfigString = collStats.wiredTiger.creationString.replace(encryptionRegex, ",");

assert.commandWorked(
    db.runCommand({
        create: collNamePrefix + '.dest',
        storageEngine: {wiredTiger: {configString: sanitisedCollConfigString}}
    }),
    'unable to create collection using the sanitised creation string of another collection');

const sanitisedIndexConfigString =
    collStats.indexDetails.a_1.creationString.replace(encryptionRegex, ",");

assert.commandWorked(db.runCommand({
    createIndexes: collNamePrefix + '.dest',
    indexes: [{
        key: {b: 1},
        name: 'b_1',
        storageEngine: {wiredTiger: {configString: sanitisedIndexConfigString}}
    }]
}),
                     'unable to create index using the sanitised creation string of another index');

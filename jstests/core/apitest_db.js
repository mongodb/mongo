/**
 *   Tests for the db object enhancement
 */

assert("test" == db, "wrong database currently not test");

dd = function(x) {
    // print( x );
};

dd("a");

dd("b");

/*
 *  be sure the public collection API is complete
 */
assert(db.createCollection, "createCollection");
assert(db.getProfilingLevel, "getProfilingLevel");
assert(db.setProfilingLevel, "setProfilingLevel");
assert(db.dbEval, "dbEval");
assert(db.group, "group");

dd("c");

/*
 * test createCollection
 */

db.getCollection("test").drop();
db.getCollectionNames().forEach(function(x) {
    assert(x != "test");
});

dd("d");

db.createCollection("test");
var found = false;
db.getCollectionNames().forEach(function(x) {
    if (x == "test")
        found = true;
});
assert(found, "found test.test in collection infos");

// storageEngine in collection options must:
// - be a document
// - all fields of the document:
// -- must have names that are registered storage engines
// -- must be objects
db.getCollection('test').drop();
var storageEngineName = db.serverStatus().storageEngine.name;
assert.commandFailed(db.createCollection('test', {storageEngine: 'not a document'}));
assert.commandWorked(db.createCollection('test', {storageEngine: {}}));
assert.commandFailed(db.createCollection('test', {storageEngine: {unknownStorageEngine: {}}}));
var invalidStorageEngineOptions = {};
invalidStorageEngineOptions[storageEngineName] = 12345;
assert.commandFailed(db.createCollection('test', {storageEngine: invalidStorageEngineOptions}));

// Test round trip of storageEngine in collection options.
// Assume that empty document for storageEngine-specific options is acceptable.
var validStorageEngineOptions = {};
validStorageEngineOptions[storageEngineName] = {};
db.getCollection('test').drop();
assert.commandWorked(db.createCollection('test', {storageEngine: validStorageEngineOptions}));

var collectionInfos = db.getCollectionInfos({name: 'test'});
assert.eq(1, collectionInfos.length, "'test' collection not created");
assert.eq('test', collectionInfos[0].name, "'test' collection not created");
assert.docEq(validStorageEngineOptions,
             collectionInfos[0].options.storageEngine,
             'storage engine options not found in listCommands result');

// The indexOptionDefaults must be a document that contains only a storageEngine field.
db.idxOptions.drop();
assert.commandFailed(db.createCollection('idxOptions', {indexOptionDefaults: 'not a document'}));
assert.commandFailed(
    db.createCollection('idxOptions', {indexOptionDefaults: {unknownOption: true}}),
    'created a collection with an unknown option to indexOptionDefaults');
assert.commandWorked(db.createCollection('idxOptions', {indexOptionDefaults: {}}),
                     'should have been able to specify an empty object for indexOptionDefaults');
assert(db.idxOptions.drop());
assert.commandWorked(db.createCollection('idxOptions', {indexOptionDefaults: {storageEngine: {}}}),
                     'should have been able to configure zero storage engines');
assert(db.idxOptions.drop());

// The storageEngine subdocument must configure only registered storage engines.
assert.commandFailed(
    db.createCollection('idxOptions',
                        {indexOptionDefaults: {storageEngine: {unknownStorageEngine: {}}}}),
    'configured an unregistered storage engine');

// The storageEngine subdocument must contain valid storage engine options.
assert.commandFailed(
    db.createCollection('idxOptions',
                        {indexOptionDefaults: {storageEngine: invalidStorageEngineOptions}}),
    'configured a storage engine with invalid options');

// Tests that a non-active storage engine can be configured so long as it is registered.
var alternateStorageEngine =
    db.serverBuildInfo().storageEngines.find(engine => engine !== storageEngineName);
if (alternateStorageEngine) {
    var indexOptions = {storageEngine: {[alternateStorageEngine]: {}}};
    assert.commandWorked(db.createCollection('idxOptions', {indexOptionDefaults: indexOptions}),
                         'should have been able to configure a non-active storage engine');
    assert(db.idxOptions.drop());
}

// Tests that the indexOptionDefaults are retrievable from the collection options.
assert.commandWorked(db.createCollection(
    'idxOptions', {indexOptionDefaults: {storageEngine: validStorageEngineOptions}}));

var collectionInfos = db.getCollectionInfos({name: 'idxOptions'});
assert.eq(1, collectionInfos.length, "'idxOptions' collection not created");
assert.docEq({storageEngine: validStorageEngineOptions},
             collectionInfos[0].options.indexOptionDefaults,
             'indexOptionDefaults were not applied: ' + tojson(collectionInfos));

dd("e");

/*
 *  profile level
 */

// A test-specific database is used for profiler testing so as not to interfere with other tests
// that modify profiler level, when run in parallel.
var profileLevelDB = db.getSiblingDB("apitest_db_profile_level");

profileLevelDB.setProfilingLevel(0);
assert(profileLevelDB.getProfilingLevel() == 0, "prof level 0");

profileLevelDB.setProfilingLevel(1);
assert(profileLevelDB.getProfilingLevel() == 1, "p1");

profileLevelDB.setProfilingLevel(2);
assert(profileLevelDB.getProfilingLevel() == 2, "p2");

profileLevelDB.setProfilingLevel(0);
assert(profileLevelDB.getProfilingLevel() == 0, "prof level 0");

dd("f");
asserted = false;
try {
    profileLevelDB.setProfilingLevel(10);
    assert(false);
} catch (e) {
    asserted = true;
    assert(e.dbSetProfilingException);
}
assert(asserted, "should have asserted");

dd("g");

assert.eq("foo", db.getSisterDB("foo").getName());
assert.eq("foo", db.getSiblingDB("foo").getName());

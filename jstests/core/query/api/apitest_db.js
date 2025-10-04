// @tags: [
//   does_not_support_stepdowns,
// ]

/**
 *   Tests for the db object enhancement
 */

assert("test" == db, "wrong database currently not test");

const collName = jsTestName();

let dd = function (x) {
    // print( x );
};

dd("a");

dd("b");

/*
 *  be sure the public collection API is complete
 */
assert(db.createCollection, "createCollection");

dd("c");

/*
 * test createCollection
 */

db.getCollection(collName).drop();
db.getCollectionNames().forEach(function (x) {
    assert(x != collName);
});

dd("d");

db.createCollection(collName);
let found = false;
db.getCollectionNames().forEach(function (x) {
    if (x == collName) found = true;
});
assert(found, "found test." + collName + " in collection infos");

// storageEngine in collection options must:
// - be a document
// - all fields of the document:
// -- must have names that are registered storage engines
// -- must be objects
db.getCollection(collName).drop();
let storageEngineName = db.serverStatus().storageEngine.name;
assert.commandFailed(db.createCollection(collName, {storageEngine: "not a document"}));
assert.commandWorked(db.createCollection(collName, {storageEngine: {}}));
assert.commandFailed(db.createCollection(collName, {storageEngine: {unknownStorageEngine: {}}}));
let invalidStorageEngineOptions = {};
invalidStorageEngineOptions[storageEngineName] = 12345;
assert.commandFailed(db.createCollection(collName, {storageEngine: invalidStorageEngineOptions}));

// Test round trip of storageEngine in collection options.
// Assume that empty document for storageEngine-specific options is acceptable.
let validStorageEngineOptions = {};
validStorageEngineOptions[storageEngineName] = {};
db.getCollection(collName).drop();
assert.commandWorked(db.createCollection(collName, {storageEngine: validStorageEngineOptions}));

var collectionInfos = db.getCollectionInfos({name: collName});
assert.eq(1, collectionInfos.length, "'" + collName + "' collection not created");
assert.eq(collName, collectionInfos[0].name, "'" + collName + "' collection not created");
assert.docEq(
    validStorageEngineOptions,
    collectionInfos[0].options.storageEngine,
    "storage engine options not found in listCommands result",
);

// The indexOptionDefaults must be a document that contains only a storageEngine field.
db.idxOptions.drop();
assert.commandFailed(db.createCollection("idxOptions", {indexOptionDefaults: "not a document"}));
assert.commandFailed(
    db.createCollection("idxOptions", {indexOptionDefaults: {unknownOption: true}}),
    "created a collection with an unknown option to indexOptionDefaults",
);
assert.commandWorked(
    db.createCollection("idxOptions", {indexOptionDefaults: {}}),
    "should have been able to specify an empty object for indexOptionDefaults",
);
assert(db.idxOptions.drop());
assert.commandWorked(
    db.createCollection("idxOptions", {indexOptionDefaults: {storageEngine: {}}}),
    "should have been able to configure zero storage engines",
);
assert(db.idxOptions.drop());

// The storageEngine subdocument must configure only registered storage engines.
assert.commandFailed(
    db.createCollection("idxOptions", {indexOptionDefaults: {storageEngine: {unknownStorageEngine: {}}}}),
    "configured an unregistered storage engine",
);

// The storageEngine subdocument must contain valid storage engine options.
assert.commandFailed(
    db.createCollection("idxOptions", {indexOptionDefaults: {storageEngine: invalidStorageEngineOptions}}),
    "configured a storage engine with invalid options",
);

// Tests that a non-active storage engine can be configured so long as it is registered.
let alternateStorageEngine = db
    .getServerBuildInfo()
    .rawData()
    .storageEngines.find((engine) => engine !== storageEngineName);
if (alternateStorageEngine) {
    let indexOptions = {storageEngine: {[alternateStorageEngine]: {}}};
    assert.commandWorked(
        db.createCollection("idxOptions", {indexOptionDefaults: indexOptions}),
        "should have been able to configure a non-active storage engine",
    );
    assert(db.idxOptions.drop());
}

// Tests that the indexOptionDefaults are retrievable from the collection options.
assert.commandWorked(
    db.createCollection("idxOptions", {indexOptionDefaults: {storageEngine: validStorageEngineOptions}}),
);

var collectionInfos = db.getCollectionInfos({name: "idxOptions"});
assert.eq(1, collectionInfos.length, "'idxOptions' collection not created");
assert.docEq(
    {storageEngine: validStorageEngineOptions},
    collectionInfos[0].options.indexOptionDefaults,
    "indexOptionDefaults were not applied: " + tojson(collectionInfos),
);

dd("e");

assert.eq("foo", db.getSiblingDB("foo").getName());

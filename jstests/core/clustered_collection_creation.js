/**
 * Tests the options used to create a clustered collection. Validates the created collection's
 * listIndexes and listCollections outputs and ensures the clusteredIndex cannot be dropped
 * regardless of the creation options used.
 *
 * @tags: [
 *   requires_fcv_51,
 *   assumes_against_mongod_not_mongos,
 *   assumes_no_implicit_collection_creation_after_drop,
 *   does_not_support_stepdowns,
 * ]
 */
(function() {
"use strict";

const clusteredIndexesEnabled = assert
                                    .commandWorked(db.getMongo().adminCommand(
                                        {getParameter: 1, featureFlagClusteredIndexes: 1}))
                                    .featureFlagClusteredIndexes.value;

if (!clusteredIndexesEnabled) {
    jsTestLog('Skipping test because the clustered indexes feature flag is disabled');
    return;
}

// listCollections should include the clusteredIndex.
const validateListCollections = function(db, collName, fullCreationOptions) {
    const listColls =
        assert.commandWorked(db.runCommand({listCollections: 1, filter: {name: collName}}));
    const listCollsOptions = listColls.cursor.firstBatch[0].options;
    assert(listCollsOptions.clusteredIndex);
    assert.docEq(listCollsOptions.clusteredIndex, fullCreationOptions.clusteredIndex);
};

// The clusteredIndex should appear in listIndexes with additional "clustered" field.
const validateListIndexes = function(db, collName, fullCreationOptions) {
    const listIndexes = assert.commandWorked(db[collName].runCommand("listIndexes"));
    const expectedListIndexesOutput =
        Object.extend({clustered: true}, fullCreationOptions.clusteredIndex);
    assert.docEq(listIndexes.cursor.firstBatch[0], expectedListIndexesOutput);
};

// It is illegal to drop the clusteredIndex. Verify that the various ways of dropping the
// clusteredIndex fail accordingly.
const validateClusteredIndexUndroppable = function(db, collName, fullCreationOptions) {
    const expectedIndexName = fullCreationOptions.clusteredIndex.name;

    assert.commandFailedWithCode(db[collName].dropIndex({_id: 1}), 5979800);

    assert.commandFailedWithCode(db[collName].dropIndex(expectedIndexName), 5979800);

    assert.commandFailedWithCode(db.runCommand({dropIndexes: collName, index: [expectedIndexName]}),
                                 5979800);
};

const validateCreatedCollection = function(db, collName, creationOptions) {
    // Upon creating a collection, fields absent in the user provided creation options are filled in
    // with default values. The fullCreationOptions should contain default values for the fields not
    // specified by the user.
    let fullCreationOptions = creationOptions;

    // If the creationOptions don't specify the name, expect the default.
    if (!creationOptions.clusteredIndex.name) {
        fullCreationOptions.clusteredIndex.name = "_id_";
    }

    // If the creationOptions don't specify 'v', expect the default.
    if (!creationOptions.clusteredIndex.v) {
        fullCreationOptions.clusteredIndex.v = 2;
    }

    validateListCollections(db, collName, fullCreationOptions);
    validateListIndexes(db, collName, fullCreationOptions);
    validateClusteredIndexUndroppable(db, collName, fullCreationOptions);
};

/**
 * Creates, validates, and drops a clustered collection with the provided creationOptions.
 */
const runSuccessfulCreate = function(db, coll, creationOptions) {
    assert.commandWorked(db.createCollection(coll.getName(), creationOptions));
    validateCreatedCollection(testDB, coll.getName(), creationOptions);
    coll.drop();
};

const testDB = db.getSiblingDB(jsTestName());
const coll = testDB.coll;
coll.drop();

runSuccessfulCreate(
    testDB, coll, {clusteredIndex: {key: {_id: 1}, unique: true}, expireAfterSeconds: 5});

coll.drop();

runSuccessfulCreate(testDB, coll, {clusteredIndex: {key: {_id: 1}, unique: true}});

runSuccessfulCreate(
    testDB, coll, {clusteredIndex: {key: {_id: 1}, unique: true}, expireAfterSeconds: 5});

runSuccessfulCreate(
    testDB, coll, {clusteredIndex: {key: {_id: 1}, name: "index_on_id", unique: true}});

runSuccessfulCreate(
    testDB, coll, {clusteredIndex: {key: {_id: 1}, name: "index_on_id", unique: true, v: 2}});

// Validate that it's not possible to create a clustered collection as a view.
assert.commandFailedWithCode(
    testDB.createCollection(coll.getName(),
                            {clusteredIndex: {key: {_id: 1}, unique: true}, viewOn: "sourceColl"}),
    6026500);

// Validate that it's not possible to create a clustered collection with {autoIndexId: false}.
assert.commandFailedWithCode(
    testDB.createCollection(coll.getName(),
                            {clusteredIndex: {key: {_id: 1}, unique: true}, autoIndexId: false}),
    6026501);

assert.commandFailedWithCode(
    testDB.createCollection(coll.getName(), {clusteredIndex: {key: {_id: 1}, unique: false}}),
    5979700);

assert.commandFailedWithCode(
    testDB.createCollection(coll.getName(), {clusteredIndex: {key: {randKey: 1}, unique: true}}),
    5979701);

// Clustered index legacy format { clusteredIndex: <bool> } is only supported on certain internal
// namespaces (e.g time-series buckets collections). Additionally, collections that support the
// legacy format are prohibited from using the other format.
const bucketsCollName = 'system.buckets.' + coll.getName();
assert.commandFailedWithCode(
    testDB.createCollection(bucketsCollName, {clusteredIndex: {key: {_id: 1}, unique: true}}),
    5979703);
assert.commandFailedWithCode(testDB.createCollection(coll.getName(), {clusteredIndex: true}),
                             5979703);

assert.commandFailedWithCode(
    testDB.createCollection(coll.getName(),
                            {clusteredIndex: {key: {_id: 1}, unique: true, randField: 1}}),
    40415);

assert.commandFailedWithCode(
    testDB.createCollection(coll.getName(),
                            {clusteredIndex: {key: {_id: 1}, unique: true, v: 12345}}),
    5979704);

// Invalid 'expireAfterSeconds'.
assert.commandFailedWithCode(
    testDB.createCollection(
        coll.getName(), {clusteredIndex: {key: {_id: 1}, unique: true}, expireAfterSeconds: -10}),
    ErrorCodes.InvalidOptions);
})();

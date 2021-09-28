/**
 * Tests the options used to create a clustered collection and verifies the options match in the
 * listCollections output.
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

const validateListCollections = function(db, collName, creationOptions) {
    const listColls =
        assert.commandWorked(db.runCommand({listCollections: 1, filter: {name: collName}}));
    const listCollsOptions = listColls.cursor.firstBatch[0].options;
    assert(listCollsOptions.clusteredIndex);

    let expectedOptions = creationOptions;

    // If the creationOptions don't specify the name, expect the default.
    if (!creationOptions.clusteredIndex.name) {
        expectedOptions.clusteredIndex.name = "_id_1";
    }

    // If the creationOptions don't specify 'v', expect the default.
    if (!creationOptions.clusteredIndex.v) {
        expectedOptions.clusteredIndex.v = 2;
    }

    assert.docEq(listCollsOptions.clusteredIndex, expectedOptions.clusteredIndex);
};

/**
 * Creates, validates, and drops a clustered collection with the provided creationOptions.
 */
const runSuccessfulCreate = function(db, coll, creationOptions) {
    assert.commandWorked(db.createCollection(coll.getName(), creationOptions));
    validateListCollections(testDB, coll.getName(), creationOptions);
    coll.drop();
};
const testDB = db.getSiblingDB(jsTestName());
const coll = testDB.coll;

runSuccessfulCreate(testDB, coll, {clusteredIndex: {key: {_id: 1}, unique: true}});

runSuccessfulCreate(
    testDB, coll, {clusteredIndex: {key: {_id: 1}, unique: true}, expireAfterSeconds: 5});

runSuccessfulCreate(
    testDB, coll, {clusteredIndex: {key: {_id: 1}, name: "index_on_id", unique: true}});

runSuccessfulCreate(
    testDB, coll, {clusteredIndex: {key: {_id: 1}, name: "index_on_id", unique: true, v: 2}});

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

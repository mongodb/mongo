/**
 * Tests the options used to create a clustered collection. Validates the created collection's
 * listIndexes and listCollections outputs and ensures the clusteredIndex cannot be dropped
 * regardless of the create options used.
 * Covers clustering on {_id: 1} for replicated collections, and clustering on non-_id fields for
 * non-replicated collections.
 *
 * @tags: [
 *   requires_fcv_53,
 *   assumes_against_mongod_not_mongos,
 *   assumes_no_implicit_collection_creation_after_drop,
 *   does_not_support_stepdowns,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/clustered_collections/clustered_collection_util.js");

const conn = MongoRunner.runMongod({setParameter: {supportArbitraryClusterKeyIndex: true}});

const validateCompoundSecondaryIndexes = function(db, coll, clusterKey) {
    const clusterKeyField = Object.keys(clusterKey)[0];
    coll.drop();
    assert.commandWorked(
        db.createCollection(coll.getName(), {clusteredIndex: {key: clusterKey, unique: true}}));
    // Expect it's possible to create a compound secondary index that does not include the cluster
    // key.
    assert.commandWorked(coll.createIndex({secondaryKey0: 1, secondaryKey1: 1}));
    // Expect it's possible to create a compound secondary index that prefixes the cluster key.
    assert.commandWorked(coll.createIndex({[clusterKeyField]: 1, secondaryKey1: 1}));
    // Expect it's possible to create a compound secondary index that includes the cluster key but
    // not as a prefix.
    assert.commandWorked(coll.createIndex({secondaryKey0: 1, [clusterKeyField]: 1}));
    coll.drop();
};

// Tests it is legal to call createIndex on the cluster key with or without {'clustered': true} as
// an option. Additionally, confirms it is illegal to call createIndex with the 'clustered' option
// on a pattern that is not the cluster key.
const validateCreateIndexOnClusterKey = function(db, collName, fullCreateOptions) {
    const clusterKey = fullCreateOptions.clusteredIndex.key;

    const listIndexes0 = assert.commandWorked(db[collName].runCommand("listIndexes"));
    const listIndexesClusteredIndex = listIndexes0.cursor.firstBatch[0];

    // Expect listIndexes to append the 'clustered' field to it's clusteredIndex output.
    assert.docEq(listIndexesClusteredIndex.key, clusterKey);
    assert.eq(listIndexesClusteredIndex.clustered, true);

    // no-op with the 'clustered' option.
    assert.commandWorked(
        db[collName].runCommand({createIndexes: collName, indexes: [listIndexesClusteredIndex]}));

    // no-op without the 'clustered' option.
    assert.commandWorked(db[collName].createIndex(clusterKey));

    // 'clustered' is not a valid option for an index not on the cluster key.
    assert.commandFailedWithCode(
        db[collName].createIndex({notMyIndex: 1}, {clustered: true, unique: true}), 6243700);

    assert.commandFailedWithCode(db[collName].runCommand({
        createIndexes: collName,
        indexes: [
            {key: {a: 1}, name: "a_1"},
            {key: {b: 1}, name: "b_1_clustered", clustered: true, unique: true}
        ]
    }),
                                 6243700);

    // The listIndexes output should be unchanged.
    const listIndexes1 = assert.commandWorked(db[collName].runCommand("listIndexes"));
    assert.eq(listIndexes1.cursor.firstBatch.length, 1);
    assert.docEq(listIndexes1.cursor.firstBatch[0], listIndexes0.cursor.firstBatch[0]);
};

// It is illegal to drop the clusteredIndex. Verify that the various ways of dropping the
// clusteredIndex fail accordingly.
const validateClusteredIndexUndroppable = function(db, collName, fullCreateOptions) {
    const expectedIndexName = fullCreateOptions.clusteredIndex.name;
    const expectedIndexKey = fullCreateOptions.clusteredIndex.key;

    assert.commandFailedWithCode(db[collName].dropIndex(expectedIndexKey), 5979800);

    assert.commandFailedWithCode(db[collName].dropIndex(expectedIndexName), 5979800);

    assert.commandFailedWithCode(db.runCommand({dropIndexes: collName, index: [expectedIndexName]}),
                                 5979800);
};

const validateCreatedCollection = function(db, collName, createOptions) {
    // Upon creating a collection, fields absent in the user provided create options are filled
    // in with default values. The fullCreateOptions should contain default values for the
    // fields not specified by the user.
    const fullCreateOptions = ClusteredCollectionUtil.constructFullCreateOptions(createOptions);

    ClusteredCollectionUtil.validateListCollections(db, collName, fullCreateOptions);
    ClusteredCollectionUtil.validateListIndexes(db, collName, fullCreateOptions);

    validateCreateIndexOnClusterKey(db, collName, fullCreateOptions);
    validateClusteredIndexUndroppable(db, collName, fullCreateOptions);
};

/**
 * Creates, validates, and drops a clustered collection with the provided createOptions.
 */
const runSuccessfulCreate = function(db, coll, createOptions) {
    assert.commandWorked(db.createCollection(coll.getName(), createOptions));
    validateCreatedCollection(db, coll.getName(), createOptions);
    coll.drop();
};

const validateClusteredCappedCollections = function(db, coll, clusterKey) {
    runSuccessfulCreate(
        db,
        coll,
        {clusteredIndex: {key: clusterKey, unique: true}, capped: true, expireAfterSeconds: 10});
    assert.commandFailedWithCode(
        db.createCollection(coll.getName(),
                            {clusteredIndex: {key: clusterKey, unique: true}, size: 10}),
        6049200);
    assert.commandFailedWithCode(
        db.createCollection(coll.getName(),
                            {clusteredIndex: {key: clusterKey, unique: true}, max: 10}),
        6049204);
    assert.commandFailedWithCode(
        db.createCollection(coll.getName(),
                            {clusteredIndex: {key: clusterKey, unique: true}, size: 10, max: 10}),
        6049200);
    assert.commandFailedWithCode(
        db.createCollection(
            coll.getName(),
            {clusteredIndex: {key: clusterKey, unique: true}, size: 10, expireAfterSeconds: 10}),
        6049200);
    assert.commandFailedWithCode(
        db.createCollection(
            coll.getName(),
            {clusteredIndex: {key: clusterKey, unique: true}, capped: true, size: 10}),
        6049200);
    assert.commandFailedWithCode(
        db.createCollection(
            coll.getName(),
            {clusteredIndex: {key: clusterKey, unique: true}, capped: true, max: 10}),
        6049204);
    assert.commandFailedWithCode(
        db.createCollection(
            coll.getName(),
            {clusteredIndex: {key: clusterKey, unique: true}, capped: true, size: 10, max: 10}),
        6049200);
    assert.commandFailedWithCode(
        db.createCollection(coll.getName(),
                            {clusteredIndex: {key: clusterKey, unique: true}, capped: true}),
        6049201);

    assert.commandWorked(db.createCollection(
        coll.getName(),
        {clusteredIndex: {key: clusterKey, unique: true}, capped: true, expireAfterSeconds: 10}));
    assert.commandFailedWithCode(coll.createIndex({a: 1}, {expireAfterSeconds: 10}), 6049202);
    coll.drop();
};

const replicatedDB = conn.getDB(jsTestName());
const nonReplicatedDB = conn.getDB('local');
const replicatedColl = replicatedDB.coll;
const nonReplicatedColl = nonReplicatedDB.coll;

replicatedColl.drop();
nonReplicatedColl.drop();

runSuccessfulCreate(
    nonReplicatedDB, nonReplicatedColl, {clusteredIndex: {key: {ts: 1}, unique: true}});

runSuccessfulCreate(nonReplicatedDB,
                    nonReplicatedColl,
                    {clusteredIndex: {key: {ts: 1}, unique: true}, expireAfterSeconds: 5});

runSuccessfulCreate(nonReplicatedDB,
                    nonReplicatedColl,
                    {clusteredIndex: {key: {ts: 1}, name: "index_on_ts", unique: true}});

runSuccessfulCreate(nonReplicatedDB,
                    nonReplicatedColl,
                    {clusteredIndex: {key: {ts: 1}, name: "index_on_ts", unique: true, v: 2}});

// Capped clustered collections creation.
validateClusteredCappedCollections(nonReplicatedDB, nonReplicatedColl, {ts: 1});

// Validate that it's not possible to create a clustered collection as a view.
assert.commandFailedWithCode(
    nonReplicatedDB.createCollection(
        nonReplicatedColl.getName(),
        {clusteredIndex: {key: {ts: 1}, unique: true}, viewOn: "sourceColl"}),
    6026500);

// Validate that it's not possible to create a clustered collection with {autoIndexId: false}.
assert.commandFailedWithCode(
    nonReplicatedDB.createCollection(
        nonReplicatedColl.getName(),
        {clusteredIndex: {key: {ts: 1}, unique: true}, autoIndexId: false}),
    6026501);

// 'unique' field must be present and set to true.
assert.commandFailedWithCode(
    nonReplicatedDB.createCollection(nonReplicatedColl.getName(), {clusteredIndex: {key: {ts: 1}}}),
    40414);
assert.commandFailedWithCode(
    nonReplicatedDB.createCollection(nonReplicatedColl.getName(),
                                     {clusteredIndex: {key: {ts: 1}, unique: false}}),
    5979700);

assert.commandFailedWithCode(
    replicatedDB.createCollection(replicatedColl.getName(),
                                  {clusteredIndex: {key: {randKey: 1}, unique: true}}),
    5979701);
assert.commandWorked(nonReplicatedDB.createCollection(
    nonReplicatedColl.getName(), {clusteredIndex: {key: {randKey: 1}, unique: true}}));
nonReplicatedColl.drop();
assert.commandFailedWithCode(
    replicatedDB.createCollection(replicatedColl.getName(),
                                  {clusteredIndex: {key: {ts: 1}, unique: true}}),
    5979701);

// Validate that arbitrary cluster keys must not be compounded, must not have nested fields, and
// that the key must have a value of 1.
assert.commandFailedWithCode(
    nonReplicatedDB.createCollection(nonReplicatedColl.getName(),
                                     {clusteredIndex: {key: {ts: 1, a: 1}, unique: true}}),
    6053700);
assert.commandFailedWithCode(
    nonReplicatedDB.createCollection(nonReplicatedColl.getName(),
                                     {clusteredIndex: {key: {'ts.a': 1}, unique: true}}),
    6053701);
assert.commandFailedWithCode(
    nonReplicatedDB.createCollection(nonReplicatedColl.getName(),
                                     {clusteredIndex: {key: {ts: -1}, unique: true}}),
    6053702);
assert.commandFailedWithCode(
    nonReplicatedDB.createCollection(nonReplicatedColl.getName(),
                                     {clusteredIndex: {key: {ts: {a: 1}}, unique: true}}),
    6053702);

assert.commandFailedWithCode(
    nonReplicatedDB.createCollection(nonReplicatedColl.getName(),
                                     {clusteredIndex: {key: {ts: 1}, unique: true, v: 12345}}),
    5979704);

// Invalid 'expireAfterSeconds'.
assert.commandFailedWithCode(
    nonReplicatedDB.createCollection(
        nonReplicatedColl.getName(),
        {clusteredIndex: {key: {ts: 1}, unique: true}, expireAfterSeconds: -10}),
    ErrorCodes.InvalidOptions);

// Validate that it's possible to create secondary indexes, regardless of whether they
// include the cluster key as one of the fields.
validateCompoundSecondaryIndexes(nonReplicatedDB, nonReplicatedColl, {ts: 1});

MongoRunner.stopMongod(conn);
})();

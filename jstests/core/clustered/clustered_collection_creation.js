/**
 * Tests the options used to create a clustered collection. Validates the created collection's
 * listIndexes and listCollections outputs and ensures the clusteredIndex cannot be dropped
 * regardless of the create options used.
 * Covers clustering on {_id: 1} for replicated collections, and clustering on non-_id fields for
 * non-replicated collections.
 *
 * @tags: [
 *   requires_fcv_53,
 *   does_not_support_stepdowns,
 *   # Write and read on "local" db with multi clients cannot match the expected response.
 *   multi_clients_incompatible,
 * ]
 */
import {ClusteredCollectionUtil} from "jstests/libs/clustered_collections/clustered_collection_util.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

const validateCompoundSecondaryIndexes = function (db, coll, clusterKey) {
    const clusterKeyField = Object.keys(clusterKey)[0];
    db.runCommand({drop: coll.getName()});
    assert.commandWorked(db.createCollection(coll.getName(), {clusteredIndex: {key: clusterKey, unique: true}}));
    // Expect it's possible to create a compound secondary index that does not include the cluster
    // key.
    assert.commandWorked(coll.createIndex({secondaryKey0: 1, secondaryKey1: 1}));
    // Expect it's possible to create a compound secondary index that prefixes the cluster key.
    assert.commandWorked(coll.createIndex({[clusterKeyField]: 1, secondaryKey1: 1}));
    // Expect it's possible to create a compound secondary index that includes the cluster key but
    // not as a prefix.
    assert.commandWorked(coll.createIndex({secondaryKey0: 1, [clusterKeyField]: 1}));
    db.runCommand({drop: coll.getName()});
};

const overrideIndexType = function (clusterKey, indexType) {
    for (const field of Object.keys(clusterKey)) {
        return Object.assign(Object.assign({}, clusterKey), {[field]: indexType});
    }
};

// Tests it is legal to call createIndex on the cluster key with or without {'clustered': true} as
// an option. Additionally, confirms it is illegal to call createIndex with the 'clustered' option
// on a pattern that is not the cluster key.
const validateCreateIndexOnClusterKey = function (db, collName, fullCreateOptions) {
    const clusterKey = fullCreateOptions.clusteredIndex.key;

    const listIndexes0 = assert.commandWorked(db[collName].runCommand("listIndexes"));
    const listIndexesClusteredIndex = listIndexes0.cursor.firstBatch[0];

    // Expect listIndexes to append the 'clustered' field to it's clusteredIndex output.
    assert.docEq(listIndexesClusteredIndex.key, clusterKey);
    assert.eq(listIndexesClusteredIndex.clustered, true);

    // no-op with the 'clustered' option.
    assert.commandWorked(db[collName].runCommand({createIndexes: collName, indexes: [listIndexesClusteredIndex]}));

    // no-op without the 'clustered' option.
    assert.commandWorked(db[collName].createIndex(clusterKey));

    // 'clustered' is not a valid option for an index not on the cluster key.
    assert.commandFailedWithCode(db[collName].createIndex({notMyIndex: 1}, {clustered: true, unique: true}), 6243700);

    assert.commandFailedWithCode(
        db[collName].runCommand({
            createIndexes: collName,
            indexes: [
                {key: {a: 1}, name: "a_1"},
                {key: {b: 1}, name: "b_1_clustered", clustered: true, unique: true},
            ],
        }),
        6243700,
    );

    // The listIndexes output should be unchanged.
    const listIndexes1 = assert.commandWorked(db[collName].runCommand("listIndexes"));
    assert.eq(listIndexes1.cursor.firstBatch.length, listIndexes0.cursor.firstBatch.length);
    for (let i = 0; i < listIndexes1.cursor.firstBatch.length; ++i) {
        assert.docEq(listIndexes1.cursor.firstBatch[i], listIndexes0.cursor.firstBatch[i]);
    }

    // It's possible to create 'hashed','2d','2dsphere' and 'text' indexes on the cluster key.
    assert.commandWorked(db[collName].createIndex(overrideIndexType(clusterKey, "hashed")));
    assert.commandWorked(db[collName].createIndex(overrideIndexType(clusterKey, "2d")));
    assert.commandWorked(db[collName].createIndex(overrideIndexType(clusterKey, "2dsphere")));
    assert.commandWorked(db[collName].createIndex(overrideIndexType(clusterKey, "text")));

    const finalIndexes = assert.commandWorked(db[collName].runCommand("listIndexes"));
    assert.eq(finalIndexes.cursor.firstBatch.length, 5);
};

// It is illegal to drop the clusteredIndex. Verify that the various ways of dropping the
// clusteredIndex fail accordingly.
const validateClusteredIndexUndroppable = function (db, collName, fullCreateOptions) {
    const expectedIndexName = fullCreateOptions.clusteredIndex.name;
    const expectedIndexKey = fullCreateOptions.clusteredIndex.key;

    assert.commandFailedWithCode(db[collName].dropIndex(expectedIndexKey), 5979800);

    assert.commandFailedWithCode(db[collName].dropIndex(expectedIndexName), 5979800);

    assert.commandFailedWithCode(db.runCommand({dropIndexes: collName, index: [expectedIndexName]}), 5979800);
};

const validateCreatedCollection = function (db, collName, createOptions) {
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
const runSuccessfulCreate = function (db, coll, createOptions) {
    assert.commandWorked(db.createCollection(coll.getName(), createOptions));
    validateCreatedCollection(db, coll.getName(), createOptions);
    db.runCommand({drop: coll.getName()});
};

const validateCreatedCollectionNonClustered = function (db, collName) {
    ClusteredCollectionUtil.validateListCollectionsNotClustered(db, collName);
    ClusteredCollectionUtil.validateListIndexesNonClustered(db, collName);
};

const runSuccessfulCreateNonClustered = function (db, coll, createOptions) {
    assert.commandWorked(db.createCollection(coll.getName(), createOptions));
    validateCreatedCollectionNonClustered(db, coll.getName(), createOptions);
    db.runCommand({drop: coll.getName()});
};

const validateClusteredCappedCollections = function (db, coll, clusterKey) {
    runSuccessfulCreate(db, coll, {
        clusteredIndex: {key: clusterKey, unique: true},
        capped: true,
        expireAfterSeconds: 10,
    });
    assert.commandFailedWithCode(
        db.createCollection(coll.getName(), {clusteredIndex: {key: clusterKey, unique: true}, size: 10}),
        6049200,
    );
    assert.commandFailedWithCode(
        db.createCollection(coll.getName(), {clusteredIndex: {key: clusterKey, unique: true}, max: 10}),
        6049204,
    );
    assert.commandFailedWithCode(
        db.createCollection(coll.getName(), {clusteredIndex: {key: clusterKey, unique: true}, size: 10, max: 10}),
        6049200,
    );
    assert.commandFailedWithCode(
        db.createCollection(coll.getName(), {
            clusteredIndex: {key: clusterKey, unique: true},
            size: 10,
            expireAfterSeconds: 10,
        }),
        6049200,
    );
    assert.commandFailedWithCode(
        db.createCollection(coll.getName(), {clusteredIndex: {key: clusterKey, unique: true}, capped: true, size: 10}),
        6049200,
    );
    assert.commandFailedWithCode(
        db.createCollection(coll.getName(), {clusteredIndex: {key: clusterKey, unique: true}, capped: true, max: 10}),
        6049204,
    );
    assert.commandFailedWithCode(
        db.createCollection(coll.getName(), {
            clusteredIndex: {key: clusterKey, unique: true},
            capped: true,
            size: 10,
            max: 10,
        }),
        6049200,
    );
    assert.commandFailedWithCode(
        db.createCollection(coll.getName(), {clusteredIndex: {key: clusterKey, unique: true}, capped: true}),
        6049201,
    );

    assert.commandWorked(
        db.createCollection(coll.getName(), {
            clusteredIndex: {key: clusterKey, unique: true},
            capped: true,
            expireAfterSeconds: 10,
        }),
    );
    assert.commandFailedWithCode(coll.createIndex({a: 1}, {expireAfterSeconds: 10}), 6049202);
    db.runCommand({drop: coll.getName()});
};

const replicatedDB = db.getSiblingDB(jsTestName());
const replicatedColl = replicatedDB.coll;
const isSharded = FixtureHelpers.isSharded(replicatedColl);
replicatedDB.runCommand({drop: replicatedColl.getName()});

runSuccessfulCreateNonClustered(replicatedDB, replicatedColl, {clusteredIndex: false});

runSuccessfulCreate(replicatedDB, replicatedColl, {clusteredIndex: {key: {_id: 1}, unique: true}});
runSuccessfulCreate(replicatedDB, replicatedColl, {
    clusteredIndex: {key: {_id: 1}, unique: true},
    expireAfterSeconds: 5,
});

runSuccessfulCreate(replicatedDB, replicatedColl, {clusteredIndex: {key: {_id: 1}, name: "index_on_id", unique: true}});

runSuccessfulCreate(replicatedDB, replicatedColl, {
    clusteredIndex: {key: {_id: 1}, name: "index_on_id", unique: true, v: 2},
});

// Capped clustered collections creation.
const hasTestCommandsEnabled = assert.commandWorked(
    replicatedDB.adminCommand({getParameter: 1, enableTestCommands: 1}),
).enableTestCommands;

if (hasTestCommandsEnabled && !isSharded) {
    jsTestLog("Testing clustered capped collections");
    validateClusteredCappedCollections(replicatedDB, replicatedColl, {_id: 1});
}

// Validate that the arguments aren't conflicting.
assert.commandFailedWithCode(
    replicatedDB.createCollection(replicatedColl.getName(), {clusteredIndex: false, expireAfterSeconds: 5}),
    ErrorCodes.InvalidOptions,
);

// Validate that it's not possible to create a clustered collection as a view.
assert.commandFailedWithCode(
    replicatedDB.createCollection(replicatedColl.getName(), {
        clusteredIndex: {key: {_id: 1}, unique: true},
        viewOn: "sourceColl",
    }),
    6026500,
);

// 'unique' field must be present and set to true.
assert.commandFailedWithCode(
    replicatedDB.createCollection(replicatedColl.getName(), {clusteredIndex: {key: {_id: 1}}}),
    ErrorCodes.IDLFailedToParse,
);
assert.commandFailedWithCode(
    replicatedDB.createCollection(replicatedColl.getName(), {clusteredIndex: {key: {_id: 1}, unique: false}}),
    5979700,
);

assert.commandFailedWithCode(
    replicatedDB.createCollection(replicatedColl.getName(), {clusteredIndex: {key: {randKey: 1}, unique: true}}),
    ErrorCodes.InvalidIndexSpecificationOption,
);
assert.commandFailedWithCode(
    replicatedDB.createCollection(replicatedColl.getName(), {clusteredIndex: {key: {ts: 1}, unique: true}}),
    ErrorCodes.InvalidIndexSpecificationOption,
);

// Clustered index legacy format { clusteredIndex: <bool> } is only supported on certain internal
// namespaces and for time-series collections.
assert.commandFailedWithCode(replicatedDB.createCollection(replicatedColl.getName(), {clusteredIndex: true}), 5979703);

assert.commandFailedWithCode(
    replicatedDB.createCollection(replicatedColl.getName(), {
        clusteredIndex: {key: {_id: 1}, unique: true, randField: 1},
    }),
    ErrorCodes.IDLUnknownField,
);

assert.commandFailedWithCode(
    replicatedDB.createCollection(replicatedColl.getName(), {clusteredIndex: {key: {_id: 1}, unique: true, v: 12345}}),
    5979704,
);

// Invalid 'expireAfterSeconds'.
assert.commandFailedWithCode(
    replicatedDB.createCollection(replicatedColl.getName(), {
        clusteredIndex: {key: {_id: 1}, unique: true},
        expireAfterSeconds: -10,
    }),
    ErrorCodes.InvalidOptions,
);

// Check using clustered : false, which is disallowed, fails for implicit collection creation
assert.commandFailedWithCode(
    db.runCommand({
        createIndexes: "some_collection",
        indexes: [{"key": {"somedata": 1}, "name": "s2", "clustered": false, "unique": true}],
    }),
    6492800,
);

// Validate that it's possible to create secondary indexes, regardless of whether they
// include the cluster key as one of the fields.
validateCompoundSecondaryIndexes(replicatedDB, replicatedColl, {_id: 1});

if (FixtureHelpers.isMongos(db)) {
    // Using the local database is not supported through mongos, so end the test here.
    quit();
}

const nonReplicatedDB = db.getSiblingDB("local");
const nonReplicatedColl = (function () {
    try {
        return nonReplicatedDB.coll;
    } catch (ex) {
        // Using the local database is not supported in this configuration, so end the test here.
        quit();
    }
})();
nonReplicatedDB.runCommand({drop: nonReplicatedColl.getName()});

runSuccessfulCreateNonClustered(nonReplicatedDB, nonReplicatedColl, {clusteredIndex: false});
runSuccessfulCreateNonClustered(nonReplicatedDB, nonReplicatedColl, {clusteredIndex: false});

assert.commandFailedWithCode(
    nonReplicatedDB.createCollection(nonReplicatedColl.getName(), {clusteredIndex: {key: {ts: 1}, unique: true}}),
    ErrorCodes.InvalidIndexSpecificationOption,
);
runSuccessfulCreate(nonReplicatedDB, nonReplicatedColl, {
    clusteredIndex: {key: {_id: 1}, unique: true},
    expireAfterSeconds: 5,
});
assert.commandFailedWithCode(
    nonReplicatedDB.createCollection(nonReplicatedColl.getName(), {
        clusteredIndex: {key: {ts: 1}, unique: true},
        expireAfterSeconds: 5,
    }),
    ErrorCodes.InvalidIndexSpecificationOption,
);

runSuccessfulCreate(nonReplicatedDB, nonReplicatedColl, {
    clusteredIndex: {key: {_id: 1}, name: "index_on_id", unique: true},
});

assert.commandFailedWithCode(
    nonReplicatedDB.createCollection(nonReplicatedColl.getName(), {
        clusteredIndex: {key: {ts: 1}, name: "index_on_ts", unique: true},
    }),
    ErrorCodes.InvalidIndexSpecificationOption,
);

runSuccessfulCreate(nonReplicatedDB, nonReplicatedColl, {
    clusteredIndex: {key: {_id: 1}, name: "index_on_id", unique: true, v: 2},
});
assert.commandFailedWithCode(
    nonReplicatedDB.createCollection(nonReplicatedColl.getName(), {
        clusteredIndex: {key: {ts: 1}, name: "index_on_ts", unique: true, v: 2},
    }),
    ErrorCodes.InvalidIndexSpecificationOption,
);

// Validate that it's not possible to create a clustered collection as a view.
assert.commandFailedWithCode(
    nonReplicatedDB.createCollection(nonReplicatedColl.getName(), {
        clusteredIndex: {key: {ts: 1}, unique: true},
        viewOn: "sourceColl",
    }),
    6026500,
);

// 'unique' field must be present and set to true.
assert.commandFailedWithCode(
    nonReplicatedDB.createCollection(nonReplicatedColl.getName(), {clusteredIndex: {key: {ts: 1}}}),
    ErrorCodes.IDLFailedToParse,
);
assert.commandFailedWithCode(
    nonReplicatedDB.createCollection(nonReplicatedColl.getName(), {clusteredIndex: {key: {ts: 1}, unique: false}}),
    5979700,
);

assert.commandFailedWithCode(
    nonReplicatedDB.createCollection(nonReplicatedColl.getName(), {clusteredIndex: {key: {randKey: 1}, unique: true}}),
    ErrorCodes.InvalidIndexSpecificationOption,
);
nonReplicatedDB.runCommand({drop: nonReplicatedColl.getName()});

// Clustered index legacy format { clusteredIndex: <bool> } is only supported on certain internal
// namespaces and for time-series collections. Additionally, collections that support the
// legacy format are prohibited from using the other format.
assert.commandFailedWithCode(
    nonReplicatedDB.createCollection(nonReplicatedColl.getName(), {
        clusteredIndex: {key: {ts: 1}, unique: true, randField: 1},
    }),
    ErrorCodes.IDLUnknownField,
);

/**
 * Test edge cases for the rangePreview -> range rename.
 */
import "jstests/multiVersion/libs/multi_rs.js";

import {EncryptedClient} from "jstests/fle2/libs/encrypted_client_util.js";

const CRUDOnDeprecatedCollectionCode = 8575606;
const CreateDeprecatedCollectionCode = 8575605;
const CreateCollectionDisabledFFCode = 9576801;
function testBinaryUpgradeWithRangePreviewCollection(upgradeConfig, isFeatureFlagEnabled) {
    const rst = new ReplSetTest({nodes: 2, nodeOptions: {binVersion: 'last-lts'}});
    rst.startSet();
    rst.initiate();
    let client = new EncryptedClient(rst.getPrimary(), "dbTest");
    // Old version; creating a new "rangePreview" collection succeeds.
    assert.commandWorked(client.createEncryptionCollection("coll1", {
        encryptedFields: {
            "fields": [{
                path: "field1",
                bsonType: "int",
                queries: [{
                    queryType: "rangePreview",
                    min: NumberInt(0),
                    max: NumberInt(8),
                    contention: NumberInt(0),
                    sparsity: 1
                }]
            }]
        }
    }));
    assert.commandWorked(client.createEncryptionCollection("coll2", {
        encryptedFields: {
            "fields": [{
                path: "field1",
                bsonType: "int",
                queries: [{
                    queryType: "rangePreview",
                    min: NumberInt(0),
                    max: NumberInt(8),
                    contention: NumberInt(0),
                    sparsity: 1
                }]
            }]
        }
    }));

    // The binary upgrade should always succeed and we should be able to start up with the
    // rangePreview collection with FCV last-lts.
    rst.upgradeSet(upgradeConfig);
    client = new EncryptedClient(rst.getPrimary(), "dbTest");

    // Latest binaries on last-LTS FCV shouldn't allow find/insert operations on rangePreview
    // collections.
    assert.commandFailedWithCode(
        client.getDB().erunCommand({insert: "coll1", documents: [{_id: 1, field1: NumberInt(2)}]}),
        CRUDOnDeprecatedCollectionCode);
    assert.commandFailedWithCode(client.getDB().erunCommand({find: "coll1", filter: {}}),
                                 CRUDOnDeprecatedCollectionCode);

    // However, drop should work.
    assert(client.getDB().coll1.drop());

    // FCV upgrade should not be impeded by the presence of a rangePreview collection.
    assert.commandWorked(
        rst.getPrimary().adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));

    // The replica set should be able to restart on upgraded FCV even with the existence of the
    // rangePreview collection.
    rst.stopSet(null /* signal */, true /* forRestart */);
    rst.startSet({restart: true});
    client = new EncryptedClient(rst.getPrimary(), "dbTest");

    // Find/insert operations on the rangePreview collections should continue to fail with the
    // latest FCV.
    assert.commandFailedWithCode(
        client.getDB().erunCommand({insert: "coll2", documents: [{_id: 1, field1: NumberInt(2)}]}),
        CRUDOnDeprecatedCollectionCode);
    assert.commandFailedWithCode(client.getDB().erunCommand({find: "coll2", filter: {}}),
                                 CRUDOnDeprecatedCollectionCode);

    // Drop should still succeed.
    assert(client.getDB().coll2.drop());

    // If featureFlagQERangeV2 is enabled, we should be able to recreate coll2 using range rather
    // than rangePreview and then run find/insert operations on it. We need to create a new
    // EncryptedClient so that the old schema mapping for coll2 is removed.
    client = new EncryptedClient(rst.getPrimary(), "dbTest");
    if (isFeatureFlagEnabled) {
        assert.commandWorked(client.createEncryptionCollection("coll2", {
            encryptedFields: {
                "fields": [{
                    path: "field1",
                    bsonType: "int",
                    queries: [{
                        queryType: "range",
                        min: NumberInt(0),
                        max: NumberInt(8),
                        contention: NumberInt(0),
                        sparsity: 1
                    }]
                }]
            }
        }));
        assert.commandWorked(client.getDB().erunCommand(
            {insert: "coll2", documents: [{_id: 0, field1: NumberInt(1), field2: NumberInt(2)}]}));
        client.runEncryptionOperation(() => {
            assert.eq(client.getDB().coll2.find({}, {__safeContent__: 0}).toArray(),
                      [{_id: 0, field1: NumberInt(1), field2: NumberInt(2)}]);
        });
    } else {
        // Otherwise, we should still be unable to create collections with "range" encrypted fields.
        assert.throwsWithCode(() => client.createEncryptionCollection("coll2", {
            encryptedFields: {
                "fields": [{
                    path: "field1",
                    bsonType: "int",
                    queries: [{
                        queryType: "range",
                        min: NumberInt(0),
                        max: NumberInt(8),
                        contention: NumberInt(0),
                        sparsity: 1
                    }]
                }]
            }
        }),
                              CreateCollectionDisabledFFCode);
    }

    rst.stopSet();
}

// Upgrade should succeed regardless of whether the feature flag is turned on or off;
// we simply are checking for the same behavior in both cases.
testBinaryUpgradeWithRangePreviewCollection(
    {binVersion: 'latest', setParameter: {featureFlagQERangeV2: false}},
    false /* isFeatureFlagEnabled */);
testBinaryUpgradeWithRangePreviewCollection(
    {binVersion: 'latest', setParameter: {featureFlagQERangeV2: true}},
    true /* isFeatureFlagEnabled */);

function testDowngradeWithRangeCollection(config, isFeatureFlagEnabled) {
    const rst = new ReplSetTest({nodes: 2, nodeOptions: config});
    rst.startSet();
    rst.initiate();
    let client = new EncryptedClient(rst.getPrimary(), "dbTest");
    // New version; creating a new "range" collection succeeds if featureFlagQERangeV2 is enabled.
    if (isFeatureFlagEnabled) {
        assert.commandWorked(client.createEncryptionCollection("coll", {
            encryptedFields: {
                "fields": [{
                    path: "field1",
                    bsonType: "int",
                    queries: [{
                        queryType: "range",
                        min: NumberInt(0),
                        max: NumberInt(8),
                        contention: NumberInt(0),
                        sparsity: 1
                    }]
                }]
            }
        }));

        // Finds and inserts should succeed on the "range" collection on FCV latest.
        assert.commandWorked(client.getDB().erunCommand(
            {insert: "coll", documents: [{_id: 0, field1: NumberInt(1), field2: NumberInt(2)}]}));
        client.runEncryptionOperation(() => {
            assert.eq(client.getDB().coll.find({}, {__safeContent__: 0}).toArray(),
                      [{_id: 0, field1: NumberInt(1), field2: NumberInt(2)}]);
        });

        // FCV downgrade to lastLTS should fail while the range collection exists.
        assert.commandFailedWithCode(
            rst.getPrimary().adminCommand(
                {setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}),
            ErrorCodes.CannotDowngrade);

        // Dropping the "range" collection should succeed.
        assert(client.getDB().coll.drop());
    } else {
        // If the feature flag is disabled, then it should not be possible to create a "range"
        // collection.
        assert.throwsWithCode(() => client.createEncryptionCollection("coll", {
            encryptedFields: {
                "fields": [{
                    path: "field1",
                    bsonType: "int",
                    queries: [{
                        queryType: "range",
                        min: NumberInt(0),
                        max: NumberInt(8),
                        contention: NumberInt(0),
                        sparsity: 1
                    }]
                }]
            }
        }),
                              CreateCollectionDisabledFFCode);
    }

    // FCV downgrade to lastLTS should succeed when there are no "range" collections.
    assert.commandWorked(
        rst.getPrimary().adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}));

    // Creating a "range" collection on the downgraded FCV should fail.
    assert.throwsWithCode(() => client.createEncryptionCollection("coll", {
        encryptedFields: {
            "fields": [{
                path: "field1",
                bsonType: "int",
                queries: [{
                    queryType: "range",
                    min: NumberInt(0),
                    max: NumberInt(8),
                    contention: NumberInt(0),
                    sparsity: 1
                }]
            }]
        }
    }),
                          CreateCollectionDisabledFFCode);

    // Downgrade to lastLTS binaries.
    rst.upgradeSet({binVersion: 'last-lts', setParameter: {}});

    // Creating a "range" collection on last-LTS binaries should fail.
    // We need to create a new EncryptedClient so that the old schema mapping for coll2 is removed.
    client = new EncryptedClient(rst.getPrimary(), "dbTest");
    assert.throwsWithCode(() => client.createEncryptionCollection("coll", {
        encryptedFields: {
            "fields": [{
                path: "field1",
                bsonType: "int",
                queries: [{
                    queryType: "range",
                    min: NumberInt(0),
                    max: NumberInt(8),
                    contention: NumberInt(0),
                    sparsity: 1
                }]
            }]
        }
    }),
                          ErrorCodes.BadValue);

    // We should be able to recreate "coll" with "rangePreview" instead of "range" and
    // now that we are on last-lts binaries.
    // We need to create a new EncryptedClient so that the old schema mapping for coll2 is removed.
    client = new EncryptedClient(rst.getPrimary(), "dbTest");
    assert.commandWorked(client.createEncryptionCollection("coll", {
        encryptedFields: {
            "fields": [{
                path: "field1",
                bsonType: "int",
                queries: [{
                    queryType: "rangePreview",
                    min: NumberInt(0),
                    max: NumberInt(8),
                    contention: NumberInt(0),
                    sparsity: 1
                }]
            }]
        }
    }));

    rst.stopSet();
}

testDowngradeWithRangeCollection(
    {binVersion: 'latest', setParameter: {featureFlagQERangeV2: false}},
    false /* isFeatureFlagEnabled */);
testDowngradeWithRangeCollection({binVersion: 'latest', setParameter: {featureFlagQERangeV2: true}},
                                 true /* isFeatureFlagEnabled */);

function testCreateCollection(config) {
    const rst = new ReplSetTest({nodes: 2, nodeConfig: config});
    rst.startSet();
    rst.initiate();
    const client = new EncryptedClient(rst.getPrimary(), "dbTest");
    // Creating a new rangePreview collection should fail.
    assert.throwsWithCode(() => client.createEncryptionCollection("coll", {
        encryptedFields: {
            "fields": [{
                path: "field1",
                bsonType: "int",
                queries: [{
                    queryType: "rangePreview",
                    min: NumberInt(0),
                    max: NumberInt(8),
                    contention: NumberInt(0),
                    sparsity: 1
                }]
            }]
        }
    }),
                          CreateDeprecatedCollectionCode);
    // Creating a new range collection should succeed.
    assert.commandWorked(client.createEncryptionCollection("coll", {
        encryptedFields: {
            "fields": [{
                path: "field1",
                bsonType: "int",
                queries: [{
                    queryType: "range",
                    min: NumberInt(0),
                    max: NumberInt(8),
                    contention: NumberInt(0),
                    sparsity: 1
                }]
            }]
        }
    }));

    // CRUD operations should work fine on the range collection.
    assert.commandWorked(client.getDB().erunCommand(
        {insert: "coll", documents: [{_id: 0, field1: NumberInt(1), field2: NumberInt(2)}]}));
    client.runEncryptionOperation(() => {
        assert.eq(client.getDB().coll.find({}, {__safeContent__: 0}).toArray(),
                  [{_id: 0, field1: NumberInt(1), field2: NumberInt(2)}]);
    });

    rst.stopSet();
}

testCreateCollection({setParameter: {featureFlagQERangeV2: false}});
testCreateCollection({setParameter: {featureFlagQERangeV2: true}});

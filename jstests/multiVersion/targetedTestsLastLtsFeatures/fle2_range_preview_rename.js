/**
 * Test edge cases for the rangePreview -> range rename.
 */
import "jstests/multiVersion/libs/multi_rs.js";

import {EncryptedClient} from "jstests/fle2/libs/encrypted_client_util.js";

const CRUDOnDeprecatedCollectionCode = 8575606;
const CreateDeprecatedCollectionCode = 8575605;
function testBinaryUpgradeWithRangePreviewCollection(upgradeConfig, fcvUpgradeShouldSucceed) {
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

    assert.commandWorked(
        client.getDB().runCommand({insert: "coll1", documents: [{_id: 0, field1: NumberInt(1)}]}));
    // The upgradeSet should always succeed and we should be able to start up with the rangePreview
    // collection.
    rst.upgradeSet(upgradeConfig);
    client = new EncryptedClient(rst.getPrimary(), "dbTest");

    // After upgrading, we can't do any CRUD ops on the collection
    assert.commandFailedWithCode(
        client.getDB().runCommand({insert: "coll1", documents: [{_id: 1, field1: NumberInt(2)}]}),
        CRUDOnDeprecatedCollectionCode);
    assert.commandFailedWithCode(client.getDB().runCommand({find: "coll1", filter: {}}),
                                 CRUDOnDeprecatedCollectionCode);

    // We should be able to drop the collection
    client.getDB().coll1.drop();

    const res =
        rst.getPrimary().adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true});
    if (fcvUpgradeShouldSucceed) {
        assert.commandWorked(res);
    } else {
        assert.commandFailedWithCode(res, ErrorCodes.CannotUpgrade);
        client.getDB().coll2.drop();
        // After dropping both rangePreview collections, we should be able to upgrade (once drops
        // clear).
        assert.soon(() => {
            assert.commandWorked(rst.getPrimary().adminCommand(
                {setFeatureCompatibilityVersion: latestFCV, confirm: true}));
            return true;
        });
    }
    rst.stopSet();
}

// When we set feature flag off, upgrading FCV will succeed because rangePreview is allowed when
// Range V2 is disabled
testBinaryUpgradeWithRangePreviewCollection(
    {binVersion: 'latest', setParameter: {featureFlagQERangeV2: false}}, true);
// When we set it on, upgrading will fail
testBinaryUpgradeWithRangePreviewCollection(
    {binVersion: 'latest', setParameter: {featureFlagQERangeV2: true}}, false);

function testDowngradeWithRangeCollection(config) {
    const rst = new ReplSetTest({nodes: 2, nodeOptions: config});
    rst.startSet();
    rst.initiate();
    let client = new EncryptedClient(rst.getPrimary(), "dbTest");
    // Old version; creating a new "rangePreview" collection succeeds.
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

    // Can't downgrade FCV to lastLTS with range collection, no matter if the feature flag is
    // enabled.
    assert.commandFailedWithCode(
        rst.getPrimary().adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}),
        ErrorCodes.CannotDowngrade);

    client.getDB().coll.drop();
    // After dropping the collection, we should eventually be able to downgrade
    assert.soon(() => {
        assert.commandWorked(rst.getPrimary().adminCommand(
            {setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}));
        return true;
    });

    rst.stopSet();
}

testDowngradeWithRangeCollection(
    {binVersion: 'latest', setParameter: {featureFlagQERangeV2: false}});
testDowngradeWithRangeCollection(
    {binVersion: 'latest', setParameter: {featureFlagQERangeV2: true}});

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
    assert.commandWorked(client.getDB().runCommand(
        {insert: "coll", documents: [{_id: 0, field1: NumberInt(1), field2: NumberInt(2)}]}));
    assert.eq(client.getDB().coll.find({}, {__safeContent__: 0}).toArray(),
              [{_id: 0, field1: NumberInt(1), field2: NumberInt(2)}]);

    rst.stopSet();
}

testCreateCollection({setParameter: {featureFlagQERangeV2: false}});
testCreateCollection({setParameter: {featureFlagQERangeV2: true}})

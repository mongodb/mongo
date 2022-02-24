/**
 * Tests that the cluster cannot be downgraded when there are indexes with the
 * 'prepareUnique' field present.
 *
 * TODO SERVER-63564: Remove this test once kLastLTS is 6.0.
 *
 * @tags: [requires_fcv_53]
 */
(function() {
"use strict";

const conn = MongoRunner.runMongod();
const db = conn.getDB("test");

const collModIndexUniqueEnabled = assert
                                      .commandWorked(db.getMongo().adminCommand(
                                          {getParameter: 1, featureFlagCollModIndexUnique: 1}))
                                      .featureFlagCollModIndexUnique.value;

if (!collModIndexUniqueEnabled) {
    jsTestLog('Skipping test because the collMod unique index feature flag is disabled.');
    MongoRunner.stopMongod(conn);
    return;
}

const collName = "index_prepareUnique_downgrade";
const coll = db.getCollection(collName);
assert.commandWorked(db.createCollection(coll.getName()));

function checkIndexForDowngrade(withFCV, fixIndex, isCompatible) {
    assert.commandWorked(coll.createIndex({a: 1}, {prepareUnique: true}));
    assert.commandWorked(coll.createIndex({b: 1}, {prepareUnique: true}));

    if (fixIndex) {
        // Resolves the incompatibility before the downgrade.
        assert.commandWorked(coll.dropIndex({a: 1}));
        assert.commandWorked(
            db.runCommand({collMod: collName, index: {keyPattern: {b: 1}, prepareUnique: false}}));
    } else if (!isCompatible) {
        assert.commandFailedWithCode(db.adminCommand({setFeatureCompatibilityVersion: withFCV}),
                                     ErrorCodes.CannotDowngrade);
        assert.commandWorked(coll.dropIndexes("*"));
    }

    // Downgrades to the version 'withFCV' and reset to 'latestFCV'.
    assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: withFCV}));
    assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: latestFCV}));

    assert.commandWorked(coll.dropIndexes("*"));
}

// Fails to downgrade to 5.2.
checkIndexForDowngrade(lastContinuousFCV, false, false);

// Fails to downgrade to 5.0.
checkIndexForDowngrade(lastLTSFCV, false, false);

// Successfully downgrades to 5.2 after removing the 'prepareUnique' field.
checkIndexForDowngrade(lastContinuousFCV, true, true);

// Successfully downgrades to 5.0 after removing the 'prepareUnique' field.
checkIndexForDowngrade(lastLTSFCV, true, true);

MongoRunner.stopMongod(conn);
}());
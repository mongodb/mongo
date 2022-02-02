/**
 * Tests that the cluster cannot be downgraded when there are indexes with the 'comment' field
 * present.
 *
 * TODO SERVER-63171: Update this test once kLastContinuous is 5.3.
 * TODO SERVER-63172: Remove this test once kLastLTS is 6.0.
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

const collName = "index_comment_downgrade";
const coll = db.getCollection(collName);
assert.commandWorked(db.createCollection(coll.getName()));

function checkIndexForDowngrade(withFCV, fixIndex, isCompatible) {
    assert.commandWorked(coll.createIndex({a: 1}, {comment: {"index_to_be_dropped": 1}}));
    // TODO SERVER-62971: Add the case to use collMod to remove the 'comment' field.
    // assert.commandWorked(coll.createIndex({b: 1}, {comment: {"index_to_be_collMod": 1}}));

    if (fixIndex) {
        // Resolves the incompatibility before the downgrade.
        assert.commandWorked(coll.dropIndex({a: 1}));
        // assert.commandWorked(db.runCommand({collMod: collName, index: {keyPattern: {b: 1},
        // comment: new Object()}}));
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

// Successfully downgrades to 5.2 after removing the 'comment' field.
checkIndexForDowngrade(lastContinuousFCV, true, true);

// Successfully downgrades to 5.0 after removing the 'comment' field.
checkIndexForDowngrade(lastLTSFCV, true, true);

MongoRunner.stopMongod(conn);
}());

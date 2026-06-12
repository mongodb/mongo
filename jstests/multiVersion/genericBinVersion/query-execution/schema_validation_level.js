/**
 * Test edge cases for the schema validation level 'constraint'
 */
import "jstests/multiVersion/libs/multi_rs.js";

import {ReplSetTest} from "jstests/libs/replsettest.js";

const rst = new ReplSetTest({
    nodes: 2,
    nodeOptions: {binVersion: "latest"},
});
rst.startSet();
rst.initiate();

const db = rst.getPrimary().getDB("test");

assert.commandWorked(
    db.createCollection("test", {
        validator: {a: 1},
        validationAction: "error",
        validationLevel: "constraint",
    }),
);

// Can't downgrade FCV to lastLTS when collection has constraint validation level.
assert.commandFailedWithCode(
    rst.getPrimary().adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}),
    ErrorCodes.CannotDowngrade,
);

assert.commandWorked(db.runCommand({collMod: "test", validationLevel: "strict"}));

// Setting prepareConstraintValidationLevel also blocks downgrade.
assert.commandWorked(db.runCommand({collMod: "test", prepareConstraintValidationLevel: true}));
assert.commandFailedWithCode(
    rst.getPrimary().adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}),
    ErrorCodes.CannotDowngrade,
);
assert.commandWorked(db.runCommand({collMod: "test", prepareConstraintValidationLevel: false}));

// After removing constraint validation level and prepareConstraintValidationLevel, we should
// eventually be able to downgrade.
assert.soon(() => {
    assert.commandWorked(
        rst.getPrimary().adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}),
    );
    return true;
});

rst.stopSet();

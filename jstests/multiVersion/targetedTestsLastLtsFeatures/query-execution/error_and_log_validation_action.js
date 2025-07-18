/**
 * Test edge cases for the rangePreview -> range rename.
 */
import "jstests/multiVersion/libs/multi_rs.js";

import {ReplSetTest} from "jstests/libs/replsettest.js";

// TODO SERVER-88921 Remove this test

const rst = new ReplSetTest({nodes: 2, nodeOptions: {binVersion: 'latest'}});
rst.startSet();
rst.initiate();

const db = rst.getPrimary().getDB("test");

assert.commandWorked(
    db.createCollection("test", {validator: {a: 1}, validationAction: "errorAndLog"}));

// Can't downgrade FCV to lastLTS when collection has errorAndLog validation action.
assert.commandFailedWithCode(
    rst.getPrimary().adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}),
    ErrorCodes.CannotDowngrade);

assert.commandWorked(db.runCommand({collMod: "test", validationAction: "error"}));

// After removing errorAndLog validation action, we should eventually be able to downgrade
assert.soon(() => {
    assert.commandWorked(
        rst.getPrimary().adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}));
    return true;
});

rst.stopSet();

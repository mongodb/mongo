/**
 * Tests that 'constraint' validationLevel is rejected when FCV is below the required version and
 * becomes available after upgrading FCV.
 *
 * Covers multiversion checklist item 2: verify that upgrading FCV turns the feature on.
 */
import "jstests/multiVersion/libs/multi_rs.js";

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const rst = new ReplSetTest({
    nodes: 1,
    nodeOptions: {binVersion: "latest", setParameter: {featureFlagConstraintValidationLevel: true}},
});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const adminDB = primary.getDB("admin");
const testDB = primary.getDB("constraint_validation_level_fcv_gating");
const collName = "coll";
const validator = {a: {$exists: true}};

describe("constraint validationLevel FCV gating", function () {
    before(function () {
        assert.commandWorked(
            adminDB.runCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}),
        );
        assert.commandWorked(
            testDB.createCollection(collName, {
                validator,
                validationLevel: "strict",
                validationAction: "error",
            }),
        );
    });

    after(function () {
        rst.stopSet();
    });

    it("rejects constraint at lastLTS FCV", function () {
        assert.commandWorked(
            adminDB.runCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}),
        );
        assert.commandFailedWithCode(
            testDB.createCollection("coll_constraint_lts", {
                validator,
                validationLevel: "constraint",
                validationAction: "error",
            }),
            ErrorCodes.InvalidOptions,
        );
        assert.commandFailedWithCode(
            testDB.runCommand({collMod: collName, prepareConstraintValidationLevel: true}),
            ErrorCodes.InvalidOptions,
        );
        assert.commandFailedWithCode(
            testDB.runCommand({collMod: collName, validationLevel: "constraint"}),
            ErrorCodes.InvalidOptions,
        );
    });

    // TODO SERVER-123679 update continuous behavior after last-continuous is 9.0.
    it("rejects constraint at lastContinuous FCV", function () {
        // Only meaningful when lastContinuous is a distinct version from both lastLTS and latest.
        // Must pass through latestFCV first since you cannot jump from lastLTS to lastContinuous.
        if (lastContinuousFCV === lastLTSFCV || lastContinuousFCV === latestFCV) {
            return;
        }
        assert.commandWorked(
            adminDB.runCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}),
        );
        assert.commandWorked(
            adminDB.runCommand({setFeatureCompatibilityVersion: lastContinuousFCV, confirm: true}),
        );
        assert.commandFailedWithCode(
            testDB.createCollection("coll_constraint_continuous", {
                validator,
                validationLevel: "constraint",
                validationAction: "error",
            }),
            ErrorCodes.InvalidOptions,
        );
        assert.commandFailedWithCode(
            testDB.runCommand({collMod: collName, prepareConstraintValidationLevel: true}),
            ErrorCodes.InvalidOptions,
        );
        assert.commandFailedWithCode(
            testDB.runCommand({collMod: collName, validationLevel: "constraint"}),
            ErrorCodes.InvalidOptions,
        );
    });

    it("enables constraint after upgrading to latestFCV", function () {
        assert.commandWorked(
            adminDB.runCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}),
        );
        assert.commandWorked(
            testDB.runCommand({collMod: collName, prepareConstraintValidationLevel: true}),
        );
        assert.commandWorked(testDB.runCommand({collMod: collName, validationLevel: "constraint"}));
        assert.eq(
            testDB.getCollectionInfos({name: collName})[0].options.validationLevel,
            "constraint",
        );
        assert.commandWorked(
            testDB.createCollection("coll_direct", {
                validator,
                validationLevel: "constraint",
                validationAction: "error",
            }),
        );
    });
});

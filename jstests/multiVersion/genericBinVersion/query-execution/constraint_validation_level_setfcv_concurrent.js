/**
 * Tests concurrency between this feature and the setFCV command:
 *
 *   collection at plain strict — pauses the downgrade mid-transition using the
 *   hangWhileDowngrading failpoint to verify that both constraint upgrade steps (prepareConstraint
 *   and validationLevel:constraint) are rejected by the FCV gate before the collection scan runs.
 */
import "jstests/multiVersion/libs/multi_rs.js";

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const rst = new ReplSetTest({
    nodes: 1,
    nodeOptions: {binVersion: "latest"},
});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const adminDB = primary.getDB("admin");
const testDB = primary.getDB("constraint_validation_level_setfcv_concurrent");
const validator = {a: {$exists: true}};
const collName = "coll";

assert.commandWorked(
    adminDB.runCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}),
);
assert.commandWorked(testDB.createCollection(collName, {validator, validationLevel: "strict"}));

// Pause the downgrade after the FCV document has moved to kDowngrading but before the collection
// scan, then verify both constraint upgrade steps are blocked by the FCV gate.
// After releasing the pause the downgrade must complete successfully.

const hangFP = configureFailPoint(primary, "hangWhileDowngrading");
const downgradeThread = new Thread(
    (host, lastLTSFCV) => {
        const conn = new Mongo(host);
        assert.commandWorked(
            conn.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}),
        );
    },
    primary.host,
    lastLTSFCV,
);
downgradeThread.start();
hangFP.wait();

// FCV is now kDowngrading. Both steps of the constraint upgrade path must be blocked by the
// FCV gate before the collection scan runs, closing any window where a collection could be
// moved toward 'constraint' and then missed by the downgrade scan.
assert.commandFailedWithCode(
    testDB.runCommand({collMod: collName, prepareConstraintValidationLevel: true}),
    ErrorCodes.InvalidOptions,
    "Expected prepareConstraintValidationLevel to be rejected while FCV is kDowngrading",
);
assert.commandFailedWithCode(
    testDB.runCommand({collMod: collName, validationLevel: "constraint"}),
    ErrorCodes.InvalidOptions,
    "Expected collMod to constraint to be rejected while FCV is kDowngrading",
);

hangFP.off();
downgradeThread.join();

assert.eq(
    assert.commandWorked(adminDB.runCommand({getParameter: 1, featureCompatibilityVersion: 1}))
        .featureCompatibilityVersion.version,
    lastLTSFCV,
    "FCV must be at lastLTSFCV after successful downgrade",
);
assert.eq(
    testDB.getCollectionInfos({name: collName})[0].options.validationLevel,
    "strict",
    "Collection must remain at strict after the blocked collMod attempts",
);

rst.stopSet();

/**
 * Regression test for SERVER-100675.
 *
 * Creating a system.buckets collection without timeseries options can interleave with FCV upgrade,
 * such that create on the primary checks the DisallowBucketCollectionWithoutTimeseriesOptions
 * feature flag on the old FCV, but the fully upgraded FCV document commits in the oplog before the
 * create operation. Thus, the secondary will already be in the new FCV when applying the create op.
 *
 * Before of this, the secondary must not check the DisallowBucketCollectionWithoutTimeseriesOptions
 * feature flag again during oplog application (as described in SERVER-91221), as otherwise it will
 * reject the create operation, and invariant due to being unable to do oplog application.
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {
    assertCommandFailedWithCodeInParallelShell,
    assertCommandWorkedInParallelShell,
} from "jstests/libs/parallel_shell_helpers.js";

const rst = new ReplSetTest({nodes: 2});
rst.startSet();
rst.initiate();
const primary = rst.getPrimary();
const db = primary.getDB(jsTestName());

assert(FeatureFlagUtil.isEnabled(db, "DisallowBucketCollectionWithoutTimeseriesOptions"));
assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}));
assert(!FeatureFlagUtil.isEnabled(db, "DisallowBucketCollectionWithoutTimeseriesOptions"));

// Start upgrading the FCV, but hang before the fully upgraded FCV document is committed
const fpSetFCV = configureFailPoint(primary, 'hangWhileUpgrading');
const awaitSetFCV = assertCommandWorkedInParallelShell(
    primary, primary.getDB("admin"), {setFeatureCompatibilityVersion: latestFCV, confirm: true});
fpSetFCV.wait();

// Start creating a system.bucket collection without timeseries options.
// DisallowBucketCollectionWithoutTimeseriesOptions is still disabled now, so creation is allowed.
const fpCreate = configureFailPoint(primary, 'hangAfterTimeseriesBucketsWithoutOptionsCheck');
const awaitCreate =
    assertCommandWorkedInParallelShell(primary, db, {create: "system.buckets.crasher"});
fpCreate.wait();

// Release the FCV upgrade so that nodes transition to the fully upgraded FCV
fpSetFCV.off();
awaitSetFCV();

// Release the create operation. The secondary will apply the create op while in the upgraded FCV.
fpCreate.off();
awaitCreate();

rst.stopSet();

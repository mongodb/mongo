import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";

/**
 * Asserts that for a given log, the oplog_cap_maintainer_thread has
 * not been started and oplog sampling has not occurred.
 */
export function verifyOplogCapMaintainerThreadNotStarted(log) {
    const threadRegex = new RegExp('"id":5295000');
    const oplogTruncateMarkersRegex = new RegExp('"id":22382');

    assert(!threadRegex.test(log));
    assert(!oplogTruncateMarkersRegex.test(log));
}

/**
 * Returns true if running in disaggregated storage mode
 */
export function isDisagg(primary) {
    const disaggRes = assert.commandWorkedOrFailedWithCode(
        primary.adminCommand({getParameter: 1, disaggregatedStorageEnabled: 1}),
        ErrorCodes.InvalidOptions, // returned if an older version doesn't have the param
    );
    return disaggRes.ok && disaggRes.disaggregatedStorageEnabled;
}

/**
 * Quits the test if running in disaggregated storage mode without
 * featureFlagSizeBasedOplogTruncationForDisagg enabled. Calls teardown() first if provided.
 *
 * Returns true if running in disaggregated storage mode
 *
 * TODO(SERVER-125068) delete this function once the feature flag is removed
 */
export function skipTestIfSizeBasedOplogTruncationDisabled(primary, teardown) {
    const isDsc = isDisagg(primary);
    if (
        isDsc &&
        !FeatureFlagUtil.isPresentAndEnabled(
            primary.getDB("admin"),
            "SizeBasedOplogTruncationForDisagg",
        )
    ) {
        jsTest.log.info(
            "Skipping test because featureFlagSizeBasedOplogTruncationForDisagg is not enabled",
        );
        if (teardown) {
            teardown();
        }
        quit();
    }
    return isDsc;
}

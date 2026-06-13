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
 * Quits the test if running in disaggregated storage mode without
 * featureFlagSizeBasedOplogTruncationForDisagg enabled. Calls teardown() first if provided.
 */
export function skipTestIfSizeBasedOplogTruncationDisabled(primary, teardown) {
    const disaggRes = assert.commandWorkedOrFailedWithCode(
        primary.adminCommand({getParameter: 1, disaggregatedStorageEnabled: 1}),
        ErrorCodes.InvalidOptions, // returned if an older version doesn't have the param
    );
    const isDisagg = disaggRes.ok && disaggRes.disaggregatedStorageEnabled;
    if (
        isDisagg &&
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
}

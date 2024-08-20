/*
 * Tests that pauseMigrationsDuringMultiUpdates cluster parameter requires the feature flag in order
 * to be enabled.
 */

import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({shards: 1});
(() => {
    // TODO SERVER-82386: Delete this test as part of removing feature flag.
    const featureStatus = FeatureFlagUtil.getStatus(st.configRS.getPrimary(),
                                                    "PauseMigrationsDuringMultiUpdatesAvailable");
    switch (featureStatus) {
        case FeatureFlagUtil.FlagStatus.kNotFound:
            jsTestLog("Skipping test because feature flag is not found.");
            return;
        case FeatureFlagUtil.FlagStatus.kEnabled:
            jsTestLog("Skipping test because feature flag is enabled.");
            return;
        case FeatureFlagUtil.FlagStatus.kDisabled:
            // Continue running the test. We are only interested in the case where the feature flag
            // exists (so that the cluster parameter also exists) but is disabled (so that the
            // cluster parameter is not allowed to be enabled).
    }

    assert.commandFailedWithCode(
        st.s.adminCommand(
            {setClusterParameter: {pauseMigrationsDuringMultiUpdates: {enabled: true}}}),
        ErrorCodes.IllegalOperation);
})();
st.stop();

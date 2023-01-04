/**
 * Utilities for testing config server catalog shard behaviors.
 */
var CatalogShardUtil = (function() {
    load("jstests/libs/feature_flag_util.js");

    function isEnabledIgnoringFCV(st) {
        return FeatureFlagUtil.isEnabled(
            st.configRS.getPrimary(), "CatalogShard", undefined /* user */, true /* ignoreFCV */);
    }

    return {
        isEnabledIgnoringFCV,
    };
})();

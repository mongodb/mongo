/**
 * Utilities for testing config server catalog shard behaviors.
 */
var CatalogShardUtil = (function() {
    load("jstests/libs/feature_flag_util.js");

    function isEnabledIgnoringFCV(st) {
        return FeatureFlagUtil.isEnabled(
            st.configRS.getPrimary(), "CatalogShard", undefined /* user */, true /* ignoreFCV */);
    }

    function transitionToDedicatedConfigServer(st, timeout) {
        if (timeout == undefined) {
            timeout = 10 * 60 * 1000;  // 10 minutes
        }

        assert.soon(function() {
            const res = st.s.adminCommand({transitionToDedicatedConfigServer: 1});
            if (!res.ok && res.code === ErrorCodes.ShardNotFound) {
                // If the config server primary steps down right after removing the config.shards
                // doc for the shard but before responding with "state": "completed", the mongos
                // would retry the _configsvrTransitionToDedicatedConfigServer command against the
                // new config server primary, which would not find the removed shard in its
                // ShardRegistry if it has done a ShardRegistry reload after the config.shards doc
                // for the shard was removed. This would cause the command to fail with
                // ShardNotFound.
                return true;
            }
            assert.commandWorked(res);
            return res.state == 'completed';
        }, "failed to transition to dedicated config server within " + timeout + "ms", timeout);
    }

    return {
        isEnabledIgnoringFCV,
        transitionToDedicatedConfigServer,
    };
})();

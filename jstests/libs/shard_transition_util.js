/**
 * Utilities for testing shard and config server transition behaviors
 */
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";

export const ShardTransitionUtil = (function() {
    function isConfigServerTransitionEnabledIgnoringFCV(st) {
        return FeatureFlagUtil.isEnabled(st.configRS.getPrimary(),
                                         "TransitionToCatalogShard",
                                         undefined /* user */,
                                         true /* ignoreFCV */);
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

    function waitForRangeDeletions(conn) {
        assert.soon(() => {
            const rangeDeletions = conn.getCollection("config.rangeDeletions").find().toArray();
            if (rangeDeletions.length) {
                jsTest.log.info("Waiting for range deletions to complete", {rangeDeletions});
                sleep(100);
                return false;
            }
            return true;
        });
    }

    function retryOnShardTransitionErrors(func) {
        while (true) {
            try {
                func();
                return;
            } catch (e) {
                if (e.code === ErrorCodes.ShardNotFound) {
                    jsTest.log.info("Ignoring error with transitioning shard, retrying",
                                    {error: e});
                    continue;
                }

                throw e;
            }
        }
    }

    return {
        isConfigServerTransitionEnabledIgnoringFCV,
        transitionToDedicatedConfigServer,
        waitForRangeDeletions,
        retryOnShardTransitionErrors,
    };
})();

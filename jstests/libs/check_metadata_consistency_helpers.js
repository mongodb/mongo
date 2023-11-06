import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";

export var MetadataConsistencyChecker = (function() {
    const run = (mongos) => {
        const adminDB = mongos.getDB('admin');

        // TODO (SERVER-70396): Remove once 7.0 becomes last LTS.
        try {
            if (!FeatureFlagUtil.isEnabled(adminDB, 'CheckMetadataConsistency')) {
                jsTest.log('Skipped metadata consistency check: feature disabled');
                return;
            }
        } catch (err) {
            jsTest.log(`Skipped metadata consistency check: ${err}`);
            return;
        }

        // The isTransientError() function is responsible for setting an error as transient and
        // abort the metadata consistency check to be retried in the future.
        const isTransientError = function(e) {
            if (ErrorCodes.isRetriableError(e.code) || ErrorCodes.isInterruption(e.code) ||
                ErrorCodes.isNetworkTimeoutError(e.code)) {
                return true;
            }

            // TODO SERVER-78117: Remove once checkMetadataConsistency command is robust to
            // ShardNotFound
            if (e.code === ErrorCodes.ShardNotFound) {
                // Metadata consistency check can fail with ShardNotFound if the router's
                // ShardRegistry reloads after choosing which shards to target and a chosen
                // shard is no longer in the cluster.
                return true;
            }

            if (e.code === ErrorCodes.FailedToSatisfyReadPreference) {
                // Metadata consistency check can fail with FailedToSatisfyReadPreference error
                // response when the primary of the shard is permanently down.
                return true;
            }

            return false;
        };

        const checkMetadataConsistency = function() {
            jsTest.log('Started metadata consistency check');

            let checkOptions = {};
            // TODO SERVER-75675 unconditionally perform index consistency checks and
            // remove the skip flag from all tests
            if (!jsTest.options().skipCheckingIndexesConsistentAcrossCluster) {
                checkOptions['checkIndexes'] = true;
            } else {
                print("Skipping index consistency check across the cluster");
            }

            const inconsistencies = adminDB.checkMetadataConsistency(checkOptions).toArray();
            assert.eq(0,
                      inconsistencies.length,
                      `Found metadata inconsistencies: ${tojson(inconsistencies)}`);

            jsTest.log('Completed metadata consistency check');
        };

        try {
            checkMetadataConsistency();
        } catch (e) {
            if (isTransientError(e)) {
                jsTest.log(`Aborted metadata consistency check due to retriable error: ${e}`);
            } else {
                throw e;
            }
        }
    };

    return {
        run: run,
    };
})();

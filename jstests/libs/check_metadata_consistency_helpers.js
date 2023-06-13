'use strict';

load('jstests/libs/feature_flag_util.js');  // For FeatureFlagUtil.

var MetadataConsistencyChecker = (function() {
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
            // Metadata consistency check can fail with ShardNotFound if the router's ShardRegistry
            // reloads after choosing which shards to target and a chosen shard is no longer in the
            // cluster. This error should be transient, so it can be retried on.
            if (ErrorCodes.isRetriableError(e.code) || ErrorCodes.isInterruption(e.code) ||
                e.code === ErrorCodes.ShardNotFound) {
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

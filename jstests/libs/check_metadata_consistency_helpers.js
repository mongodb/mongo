
export var MetadataConsistencyChecker = (function() {
    const run = (mongos) => {
        const adminDB = mongos.getDB('admin');

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

            // TODO (SERVER-83881): Remove once checkMetadataConsistency can authenticate before
            // running.
            if (e.code === ErrorCodes.Unauthorized) {
                // Metadata consistency check fails with Unauthorized if the test enables the
                // authentication as the command run on a dedicated non-authenticated shell.
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
            } else if (e.code === ErrorCodes.LockBusy) {
                const buildInfo = adminDB.getServerBuildInfo();
                const slowBuild = buildInfo.isAddressSanitizerActive() ||
                    buildInfo.isThreadSanitizerActive() || buildInfo.isDebug();
                if (slowBuild) {
                    jsTest.log(
                        `Ignoring LockBusy error on checkMetadataConsistency because we are running with very slow build (e.g. ASAN enabled)`);
                } else {
                    throw e;
                }
            } else {
                // For all the other errors re-throw the exception
                throw e;
            }
        }
    };

    return {
        run: run,
    };
})();

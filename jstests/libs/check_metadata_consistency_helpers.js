
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
            if (TestData.transitioningConfigShard && inconsistencies.length === 1 &&
                inconsistencies[0].type === "MissingLocalCollection" &&
                inconsistencies[0].shard === "config") {
                // There is currently a known bug that can lead to this inconsistency on the config
                // server. Ignore it temporarily to minimize evergreen redness while we work on the
                // fix.
                //
                // TODO SERVER-56879: Stop ignoring this inconsistency.
                jsTest.log('Temporarily ignoring known metadata inconsistency: ' +
                           tojson(inconsistencies));
            } else {
                assert.eq(0,
                          inconsistencies.length,
                          `Found metadata inconsistencies: ${tojson(inconsistencies)}`);
            }

            jsTest.log('Completed metadata consistency check');
        };

        try {
            checkMetadataConsistency();
        } catch (e) {
            if (isTransientError(e)) {
                jsTest.log(`Aborted metadata consistency check due to retriable error: ${e}`);
            } else if (e.code === ErrorCodes.LockBusy) {
                const slowBuild = _isAddressSanitizerActive() || _isThreadSanitizerActive();
                if (slowBuild) {
                    jsTest.log(
                        `Ignoring LockBusy error on checkMetadataConsistency because we are running with very slow build (e.g. ASAN enabled)`);
                } else if (TestData.transitioningConfigShard) {
                    // TODO SERVER-89841: The config shard transition suite puts pressure on DDL
                    // locks and can lead the checker to fail with LockBusy.
                    jsTest.log(
                        `Temporarily ignoring LockBusy error on checkMetadataConsistency because we are running with config transitions`);
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

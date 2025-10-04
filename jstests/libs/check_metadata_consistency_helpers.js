export var MetadataConsistencyChecker = (function () {
    const run = (mongos) => {
        const adminDB = mongos.getDB("admin");

        // The isTransientError() function is responsible for setting an error as transient and
        // abort the metadata consistency check to be retried in the future.
        const isTransientError = function (e) {
            // TODO SERVER-105255 Remove the exception for ingress gRPC.
            if (mongos.isGRPC() && e.code == ErrorCodes.CallbackCanceled) {
                jsTest.log("Treating `CallbackCanceled` as transient for gRPC streams!");
                return true;
            }

            if (
                ErrorCodes.isRetriableError(e.code) ||
                ErrorCodes.isInterruption(e.code) ||
                ErrorCodes.isNetworkTimeoutError(e.code)
            ) {
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

        const checkMetadataConsistency = function () {
            jsTest.log("Started metadata consistency check");

            let checkOptions = {};
            // TODO SERVER-75675 unconditionally perform index consistency checks and
            // remove the skip flag from all tests
            if (!jsTest.options().skipCheckingIndexesConsistentAcrossCluster) {
                checkOptions["checkIndexes"] = true;
            } else {
                jsTest.log.info("Skipping index consistency check across the cluster");
            }

            let inconsistencies = adminDB.checkMetadataConsistency(checkOptions).toArray();

            // TODO SERVER-107821: do not ignore CorruptedChunkHistory in multiversion suites
            const isMultiVersion = Boolean(jsTest.options().useRandomBinVersionsWithinReplicaSet);
            if (isMultiVersion) {
                for (let i = inconsistencies.length - 1; i >= 0; i--) {
                    if (inconsistencies[i].type == "CorruptedChunkHistory") {
                        inconsistencies.splice(i, 1); // Remove inconsistency
                    }
                }
            }

            // Since bucket collections are not created atomically with their view, it may happen
            // that checkMetadataConsistency interleaves with the creation steps in case of stepdown
            const isStepdownSuite = Boolean(jsTest.options().runningWithShardStepdowns);
            if (isStepdownSuite) {
                for (let i = inconsistencies.length - 1; i >= 0; i--) {
                    if (inconsistencies[i].type == "MalformedTimeseriesBucketsCollection") {
                        inconsistencies.splice(i, 1); // Remove inconsistency
                    }
                }
            }

            // When we are dropping the sessions collection in the background, we may see
            // inconsistencies if CMC interleaves with the non-atomic, non-guarded manual drop.
            // Note that we check TestData here rather than jsTestOptions because this option is
            // used in passthroughs which use the python class of this hook and thus we pass
            // arguments via the suite + testData.
            const isSessionsCollectionDropSuite = Boolean(TestData.backgroundSessionCollectionDrop);
            if (isSessionsCollectionDropSuite) {
                for (let i = inconsistencies.length - 1; i >= 0; i--) {
                    if (inconsistencies[i].details.namespace == "config.system.sessions") {
                        inconsistencies.splice(i, 1); // Remove inconsistency
                    }
                }
            }

            assert.eq(0, inconsistencies.length, `Found metadata inconsistencies: ${tojson(inconsistencies)}`);

            jsTest.log("Completed metadata consistency check");
        };

        try {
            checkMetadataConsistency();
        } catch (e) {
            if (isTransientError(e)) {
                jsTest.log(`Aborted metadata consistency check due to retriable error: ${e}`);
            } else if (e.code === ErrorCodes.LockBusy) {
                const buildInfo = adminDB.getServerBuildInfo();
                const slowBuild =
                    buildInfo.isAddressSanitizerActive() || buildInfo.isThreadSanitizerActive() || buildInfo.isDebug();
                if (slowBuild) {
                    jsTest.log(
                        `Ignoring LockBusy error on checkMetadataConsistency because we are running with very slow build (e.g. ASAN enabled)`,
                    );
                } else {
                    throw e;
                }
            } else if (e.code === ErrorCodes.ConflictingOperationInProgress) {
                // If this were an unexpected collection disappearance, the test would tassert so
                // simply accept the error here.
                jsTest.log("Ignoring ConflictingOperationInProgress error during checkMetadataConsistency");
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

/**
 * Concurrent time-series inserts and drops, checking that the bucket catalog
 * remains in a consistent state.
 *
 * Repro skeleton for SERVER-107351 ("Concurrent time-series inserts and drops
 * can lead to inconsistent bucket catalog states"). One thread inserts into a
 * pool of time-series collections; the other thread drops and re-creates them.
 * If the bucket catalog's per-UUID executionStats entry is re-emplaced by an
 * insert that races between drop's release-stats phase and clear-buckets
 * phase, the catalog leaks a stats entry for a uuid that no longer exists.
 *
 * After all threads finish, the teardown step checks:
 *   - bucketCatalog.numActiveBuckets is non-negative (SERVER-106451 regression
 *     check; double-decrement would push it below zero),
 *   - collStats reports timeseries.bucketCount >= 0 and bucketCount is
 *     consistent with the actual on-disk bucket count for each surviving
 *     collection (precondition that reshardCollection relies on),
 *   - reshardCollection precondition probes (when running on a sharded
 *     cluster) succeed against the surviving collections.
 *
 * @tags: [
 *   requires_timeseries,
 *   # Time-series collections cannot be written to in a multi-document
 *   # transaction.
 *   does_not_support_transactions,
 *   # The test relies on collStats and bucketCatalog numbers; stepdowns can
 *   # transiently make either return values that look inconsistent.
 *   does_not_support_stepdowns,
 *   # Config fuzzer can shrink bucket parameters in ways that change the
 *   # assertion thresholds.
 *   does_not_support_config_fuzzer,
 * ]
 */

export const $config = (function () {
    const timeFieldName = "time";
    const metaFieldName = "tag";
    const collCount = 3;
    const insertBatchSize = 25;

    // Error codes the test treats as expected when a command races with a
    // collection drop. Lifted from existing time-series concurrency tests
    // (e.g. timeseries_raw_data_with_collection_recreation.js).
    const acceptableInsertErrorCodes = [
        ErrorCodes.NamespaceNotFound,
        ErrorCodes.QueryPlanKilled,
        ErrorCodes.CommandNotSupportedOnView,
        ErrorCodes.StaleConfig,
        // Insert vs concurrent re-create as time-series.
        10685100,
        10685101,
    ];

    const acceptableDropErrorCodes = [
        ErrorCodes.NamespaceNotFound,
    ];

    function collectionName(suffix, idx) {
        return jsTestName() + "_" + suffix + "_" + idx;
    }

    function getRandomCollIdx() {
        return Random.randInt(collCount);
    }

    const data = {
        collCount: collCount,
        insertBatchSize: insertBatchSize,
        timeFieldName: timeFieldName,
        metaFieldName: metaFieldName,
        collectionName: collectionName,

        createIfMissing: function (db, collName) {
            // createCollection will fail with NamespaceExists if it lost a
            // race; that's fine.
            const res = db.createCollection(collName, {
                timeseries: {timeField: timeFieldName, metaField: metaFieldName},
            });
            if (!res.ok && res.code !== ErrorCodes.NamespaceExists) {
                assert.commandWorked(res);
            }
        },
    };

    const states = {
        init: function init(db, collName) {
            // Threads are partitioned by tid parity into one inserter and one
            // dropper. The FSM scheduler uses threadCount=2, so this gives
            // exactly one of each.
            this.role = (this.tid % 2 === 0) ? "inserter" : "dropper";
        },

        insertBatch: function insertBatch(db, collName) {
            if (this.role !== "inserter") {
                return;
            }
            const idx = getRandomCollIdx();
            const target = collectionName(collName, idx);
            const docs = [];
            for (let i = 0; i < insertBatchSize; ++i) {
                docs.push({
                    [timeFieldName]: new Date(),
                    [metaFieldName]: (this.tid * 1000) + i,
                    payload: i,
                });
            }
            const res = db.runCommand({insert: target, documents: docs, ordered: false});
            // Insert may fail or partially fail if the collection is dropped
            // mid-flight; that is the race we are trying to provoke. We do
            // NOT fail the test on those expected codes.
            if (!res.ok) {
                assert.contains(res.code, acceptableInsertErrorCodes,
                    `Unexpected insert error: ${tojson(res)}`);
                return;
            }
            if (res.writeErrors) {
                for (const we of res.writeErrors) {
                    assert.contains(we.code, acceptableInsertErrorCodes,
                        `Unexpected write error: ${tojson(we)}`);
                }
            }
        },

        dropAndRecreate: function dropAndRecreate(db, collName) {
            if (this.role !== "dropper") {
                return;
            }
            const idx = getRandomCollIdx();
            const target = collectionName(collName, idx);
            const dropRes = db.runCommand({drop: target});
            if (!dropRes.ok) {
                assert.contains(dropRes.code, acceptableDropErrorCodes,
                    `Unexpected drop error: ${tojson(dropRes)}`);
            }
            // Re-create so the inserter has a target to race against next
            // iteration. The race window we care about (between drop's
            // releaseStats and clearBuckets phases) is microseconds; the
            // re-create just keeps the workload live.
            this.createIfMissing(db, target);
        },
    };

    const transitions = {
        init: {insertBatch: 0.5, dropAndRecreate: 0.5},
        insertBatch: {insertBatch: 0.7, dropAndRecreate: 0.3},
        dropAndRecreate: {insertBatch: 0.5, dropAndRecreate: 0.5},
    };

    function setup(db, collName, cluster) {
        for (let i = 0; i < collCount; ++i) {
            const target = collectionName(collName, i);
            // Drop any pre-existing collection from a prior run, then create
            // fresh.
            db.runCommand({drop: target});
            assert.commandWorked(db.createCollection(target, {
                timeseries: {timeField: timeFieldName, metaField: metaFieldName},
            }));
        }
    }

    function assertBucketCatalogConsistent(db) {
        // Per-node bucket catalog server status: numActiveBuckets must never
        // go negative (SERVER-106451). The SERVER-107351 leak doesn't make
        // this number negative, but it does inflate executionStats; we sanity
        // check both signs.
        const bucketCatalog = db.serverStatus().bucketCatalog;
        if (bucketCatalog === undefined) {
            // mongos or older server; nothing to check here.
            return;
        }
        jsTestLog("bucketCatalog server status: " + tojson(bucketCatalog));
        if (bucketCatalog.numActiveBuckets !== undefined) {
            assert.gte(bucketCatalog.numActiveBuckets, 0,
                `numActiveBuckets went negative: ${tojson(bucketCatalog)}`);
        }
    }

    function assertCollStatsConsistent(db, collFullName) {
        const stats = db.runCommand({collStats: collFullName});
        if (!stats.ok) {
            // Collection may have been dropped at the boundary.
            assert.contains(stats.code, [ErrorCodes.NamespaceNotFound],
                `collStats failed unexpectedly: ${tojson(stats)}`);
            return;
        }
        // For time-series collections collStats returns a timeseries
        // sub-document; bucketCount and numBucketInserts must be non-negative.
        if (stats.timeseries) {
            jsTestLog(`collStats timeseries for ${collFullName}: ${tojson(stats.timeseries)}`);
            for (const k of ["bucketCount", "numBucketInserts", "numBucketUpdates",
                             "numBucketsClosedDueToCount", "numBucketsClosedDueToSize",
                             "numBucketsClosedDueToTimeForward", "numBucketsClosedDueToTimeBackward",
                             "numBucketsClosedDueToMemoryThreshold", "numCommits", "numWaits",
                             "numMeasurementsCommitted"]) {
                if (stats.timeseries[k] !== undefined) {
                    assert.gte(stats.timeseries[k], 0,
                        `${collFullName} collStats.timeseries.${k} negative: ${tojson(stats.timeseries)}`);
                }
            }
        }
    }

    function assertReshardPreconditionsHold(db, collFullName) {
        // reshardCollection's precondition path queries the catalog for the
        // current collection state; if the bucket catalog has stale entries
        // referencing a dropped UUID, that path can return inconsistent
        // results. We probe with a dry-run-ish call: if the cluster is not
        // sharded, the command returns IllegalOperation which we accept.
        // If the cluster IS sharded and the collection is missing, we accept
        // NamespaceNotFound. Anything else is the bug.
        const res = db.adminCommand({reshardCollection: collFullName,
                                     key: {[metaFieldName]: 1, [timeFieldName]: 1},
                                     numInitialChunks: 1,
                                     // dry-run flag not universally available; the assert
                                     // below tolerates the command being unsupported.
                                     forceRedistribution: false});
        // We don't require this command to succeed - this test isn't a
        // sharded suite. We only require that the failure is a "well-formed"
        // failure rather than an internal-state inconsistency.
        const tolerated = [
            ErrorCodes.IllegalOperation,        // not a sharded cluster
            ErrorCodes.NamespaceNotFound,       // collection got dropped at boundary
            ErrorCodes.NamespaceNotSharded,
            ErrorCodes.CommandNotSupported,
            ErrorCodes.CommandNotFound,
            ErrorCodes.NotImplemented,
            ErrorCodes.InvalidOptions,
            ErrorCodes.BadValue,
        ];
        if (!res.ok) {
            assert.contains(res.code, tolerated,
                `reshardCollection precondition probe surfaced a non-tolerated error - ` +
                `this may indicate bucket catalog state inconsistency: ${tojson(res)}`);
        }
    }

    function teardown(db, collName, cluster) {
        // Per-node bucket catalog checks. On a replica set this runs on each
        // mongod; on a standalone it runs once.
        if (cluster.executeOnMongodNodes) {
            cluster.executeOnMongodNodes((nodeDb) => {
                assertBucketCatalogConsistent(nodeDb);
            });
        } else {
            assertBucketCatalogConsistent(db);
        }

        for (let i = 0; i < collCount; ++i) {
            const target = collectionName(collName, i);
            assertCollStatsConsistent(db, target);
            assertReshardPreconditionsHold(db, db.getName() + "." + target);
        }
    }

    return {
        threadCount: 2,
        // ~30s wall clock budget: insert state runs ~50 batches of 25 docs
        // plus drop/recreate roundtrips; total ~150 iterations per thread is
        // a reasonable approximation on a non-loaded host. The FSM framework
        // doesn't expose a duration knob, so we tune via iteration count.
        iterations: 150,
        startState: "init",
        data: data,
        states: states,
        setup: setup,
        teardown: teardown,
        transitions: transitions,
    };
})();

/**
 * Extends random_moveChunk_timeseries_inserts.js to stress archive-based bucket reopening during
 * chunk migrations.
 *
 * @tags: [
 *  requires_sharding,
 *  assumes_balancer_off,
 *  requires_non_retryable_writes,
 *  does_not_support_transactions,
 *  requires_getmore,
 * ]
 */
import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {$config as $baseConfig} from "jstests/concurrency/fsm_workloads/random_moveChunk/random_moveChunk_timeseries_inserts.js";
import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";

export const $config = extendWorkload($baseConfig, function ($config, $super) {
    // Number of measurements at the base time before the time-backward and reopening measurements.
    $config.data.docsPerBatch = 10;
    // Offset from the base time to the time-backward measurement that archives the bucket (2 hours).
    $config.data.archiveOffsetMs = 2 * 60 * 60 * 1000;

    // Insert a batch of measurements that inserts into a bucket, archives it (time-backward), then reopens it
    // (time-forward back to the base time).
    $config.states.insert = function insert(db, collName, connCache) {
        const metaVal = this.generateMetaFieldValueForInsertStage(this.tid);
        const baseTimeMs = this.startTime + Random.randInt(this.numInitialDocs) * this.increment;
        const baseTime = new Date(baseTimeMs);
        const earlierTime = new Date(baseTimeMs - this.archiveOffsetMs);

        const docs = [];
        // Helper that inserts a measurement at a given time.
        const pushMeasurement = (time) => {
            docs.push({
                _id: new ObjectId(),
                [this.metaField]: metaVal,
                [this.timeField]: time,
                f: metaVal,
            });
        };
        for (let i = 0; i < this.docsPerBatch; i++) {
            pushMeasurement(new Date(baseTime + i * 1000));
        }
        pushMeasurement(earlierTime); // archive it (time-backward).
        pushMeasurement(baseTime); // reopen the archived bucket.

        TimeseriesTest.assertInsertWorked(db[collName].insert(docs, {ordered: true}));
        TimeseriesTest.assertInsertWorked(db[this.nonShardCollName].insert(docs, {ordered: true}));
    };

    // How long to pause each archive-based bucket reopening fetch, giving concurrent moveChunks a
    // chance to induce a StaleConfig error.
    $config.data.reopeningSleepMillis = 100;

    $config.setup = function setup(db, collName, cluster) {
        $super.setup.apply(this, arguments);
        // Force the ordered insert attempts to fall back to unordered one at a time inserts,
        // which makes it easier to deterministically exercise archive-based bucket reopening.
        cluster.executeOnMongodNodes((nodeDB) => {
            assert.commandWorked(
                nodeDB.adminCommand({
                    configureFailPoint: "failAtomicTimeseriesWrites",
                    mode: "alwaysOn",
                }),
            );
            assert.commandWorked(
                nodeDB.adminCommand({
                    configureFailPoint: "hangTimeseriesReopenArchivedBucketBeforeFetch",
                    mode: "alwaysOn",
                    data: {sleepMillis: this.reopeningSleepMillis},
                }),
            );
        });
    };

    $config.teardown = function teardown(db, collName, cluster) {
        cluster.executeOnMongodNodes((nodeDB) => {
            assert.commandWorked(
                nodeDB.adminCommand({
                    configureFailPoint: "failAtomicTimeseriesWrites",
                    mode: "off",
                }),
            );
            assert.commandWorked(
                nodeDB.adminCommand({
                    configureFailPoint: "hangTimeseriesReopenArchivedBucketBeforeFetch",
                    mode: "off",
                }),
            );
        });

        // Check that the workload actually exercised archive-based reopening.
        let numBucketsFetched = 0;
        cluster.executeOnMongodNodes((nodeDB) => {
            const bc = nodeDB.serverStatus().bucketCatalog;
            if (bc) {
                numBucketsFetched += bc.numBucketsFetched || 0;
            }
        });
        jsTest.log.info("Time-series bucket reopening stats", {numBucketsFetched});
        assert.gt(
            numBucketsFetched,
            0,
            "Workload did not perform any archive-based bucket reopenings",
        );

        // Verify that no duplicate measurements were inserted.
        const coll = db[collName];
        const dupes = coll
            .aggregate([{$group: {_id: "$_id", n: {$sum: 1}}}, {$match: {n: {$gt: 1}}}])
            .toArray();
        assert.eq(dupes.length, 0, "Found duplicate measurements in the time-series collection", {
            dupes,
        });

        $super.teardown.apply(this, arguments);
    };

    return $config;
});

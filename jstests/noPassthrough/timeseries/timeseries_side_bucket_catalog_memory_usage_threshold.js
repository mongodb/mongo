/**
 * Tests that idle buckets are removed when the side bucket catalog's memory threshold is reached.
 *
 * @tags: [
 *  requires_replication,
 *  featureFlagTimeseriesUpdatesSupport,
 * ]
 */
const timeFieldName = 'time';
const metaFieldName = 'metafield';
const valueFieldName = 'value';

const runSideBucketTest = (setAtRuntime) => {
    const rst = new ReplSetTest({nodes: 1});
    // We set the idle bucket expiry memory usage threshold to be much higher than the side
    // bucket catalog memory usage threshold (100MB vs 1MB) to ensure any idle buckets that get
    // expired are expired as a result of hitting the latter rather than the former. We have
    // separate cases to test setting it at startup and at runtime.
    if (setAtRuntime) {
        rst.startSet({
            setParameter: {
                timeseriesIdleBucketExpiryMemoryUsageThreshold: 104857600,
            }
        });
        rst.initiate();
        rst.getPrimary().getDB(jsTestName()).adminCommand({
            setParameter: 1,
            timeseriesSideBucketCatalogMemoryUsageThreshold: 1048576,
        });
    } else {
        rst.startSet({
            setParameter: {
                timeseriesIdleBucketExpiryMemoryUsageThreshold: 104857600,
                timeseriesSideBucketCatalogMemoryUsageThreshold: 1048576,
            }
        });
        rst.initiate();
    }

    const db = rst.getPrimary().getDB(jsTestName());
    assert.commandWorked(db.dropDatabase());
    const coll = db.timeseries_idle_buckets;

    assert.commandWorked(db.createCollection(
        coll.getName(), {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}));
    let stats = assert.commandWorked(coll.stats());
    assert.eq(stats.timeseries.numBucketsArchivedDueToMemoryThreshold, 0);

    const numDocs = 100;
    const metaValue = 'a';
    for (let i = 0; i < numDocs; i++) {
        assert.commandWorked(coll.insert([{
            [timeFieldName]: ISODate(),
            [metaFieldName]: {[i.toString()]: metaValue},
            [valueFieldName]: 'a',
        }]));
        assert.commandWorked(coll.insert([{
            [timeFieldName]: ISODate(),
            [metaFieldName]: {[i.toString()]: metaValue},
            [valueFieldName]: 'a',
        }]));
        assert.commandWorked(coll.insert([{
            [timeFieldName]: ISODate(),
            [metaFieldName]: {[i.toString()]: metaValue},
            [valueFieldName]: 'a',
        }]));
    }

    // Go through the existing documents and perform updates large enough to trigger the
    // sidebucketcatalog memoryusage threshold.
    assert.commandWorked(coll.updateMany({}, {$set: {[valueFieldName]: 'a'.repeat(1024 * 1024)}}));

    // Check that some buckets were archived due to memory pressure.
    stats = assert.commandWorked(coll.stats());
    assert.gt(stats.timeseries.numBucketsArchivedDueToMemoryThreshold,
              0,
              "Did not find an archived bucket");

    // It is possible that archiving buckets alone was not enough to get the side bucket catalog
    // below the memory usage threshold. In this case, archived buckets will then also be closed.
    // We should guarantee that no bucket was closed that wasn't archived.
    assert.lte(stats.timeseries.numBucketsClosedDueToMemoryThreshold,
               stats.timeseries.numBucketsArchivedDueToMemoryThreshold);

    rst.stopSet();
};

// Run the test while setting the side bucket catalog memory usage threshold on startup.
runSideBucketTest(false);

// Run the test while setting the side bucket catalog memory usage threshold at runtime.
runSideBucketTest(true);

/**
 * Tests for settings of the fallback to the rowstore scanning when CSI cannot be used to
 * reconstruct the result of a query.
 *
 * @tags: [
 *   # column store indexes are still under a feature flag and require full sbe
 *   featureFlagColumnstoreIndexes,
 *   featureFlagSbeFull,
 * ]
 */

(function() {
'use strict';

load("jstests/libs/columnstore_util.js");  // For setUpServerForColumnStoreIndexTest.

const mongod = MongoRunner.runMongod({});
const db = mongod.getDB("test");

if (!setUpServerForColumnStoreIndexTest(db)) {
    MongoRunner.stopMongod(mongod);
    return;
}

const coll = db.columnstore_index_rowstore_settings;

const defaultMin = assert
                       .commandWorked(db.adminCommand(
                           {getParameter: 1, internalQueryColumnRowstoreScanMinBatchSize: 1}))
                       .internalQueryColumnRowstoreScanMinBatchSize;
const defaultMax = assert
                       .commandWorked(db.adminCommand(
                           {getParameter: 1, internalQueryColumnRowstoreScanMaxBatchSize: 1}))
                       .internalQueryColumnRowstoreScanMaxBatchSize;

function setBatchSizeMinAndMax({min: min, max: max}) {
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryColumnRowstoreScanMinBatchSize: min}));

    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryColumnRowstoreScanMaxBatchSize: max}));
}

function getRowstoreStats(explainExec) {
    return {
        fetches:
            parseInt(JSON.stringify(explainExec).split('"numRowStoreFetches":')[1].split(",")[0]),
        scans: parseInt(JSON.stringify(explainExec).split('"numRowStoreScans":')[1].split(",")[0])
    };
}

(function testSuppressScan() {
    setBatchSizeMinAndMax({min: 0, max: defaultMax});

    const docs = [
        {obj: {x: 40}},
        {obj: {x: 41}},
        {obj: {x: 42}},
        {obj: 43},
        {obj: {x: 44}},
    ];

    coll.drop();
    coll.insert(docs);
    assert.commandWorked(coll.createIndex({"$**": "columnstore"}));

    const explain = coll.explain("executionStats").find({}, {obj: 1}).finish();
    const stats = getRowstoreStats(explain);

    assert.eq(0, stats.scans, "Scanning of the row store should be suppressed. " + tojson(explain));
    assert.eq(docs.length - 1,
              stats.fetches,
              "All records with sub-object should be fetched and only those. " + tojson(explain));
})();

(function testScanToEof() {
    setBatchSizeMinAndMax({min: 5, max: defaultMax});

    const docs = [
        {obj: 40},
        {obj: {x: 41}},  // triggers fetch and switching into the scan mode for the remaining docs
        {obj: 42},
        {obj: 43},
        {obj: 44},
    ];

    coll.drop();
    coll.insert(docs);
    assert.commandWorked(coll.createIndex({"$**": "columnstore"}));

    const explain = coll.explain("executionStats").find({}, {obj: 1}).finish();
    const stats = getRowstoreStats(explain);

    assert.eq(1,
              stats.fetches,
              "The only 'bad' document should trigger a single fetch. " + tojson(explain));
    assert.eq(
        3, stats.scans, "All documents after the 'bad' one should be scanned. " + tojson(explain));
})();

(function testAllReadsMustUseRowstore() {
    setBatchSizeMinAndMax({min: 2, max: defaultMax});

    const count = 1000;
    coll.drop();
    for (let i = 0; i < count; i++) {
        coll.insert({obj: {x: i}});
    }
    assert.commandWorked(coll.createIndex({"$**": "columnstore"}));

    const explain = coll.explain("executionStats").find({}, {obj: 1}).finish();
    const stats = getRowstoreStats(explain);

    assert.eq(count,
              stats.fetches + stats.scans,
              "All docs should need reads from rowstore. " + tojson(explain));
    assert.lt(
        stats.fetches,
        20,
        "Number of fetches should be small due to exponential increase of the scanned batch size. " +
            tojson(explain));
})();

(function testRecoverAfterRowstoreScan() {
    setBatchSizeMinAndMax({min: 3, max: defaultMax});

    const docs = [
        {obj: 40},
        {obj: {x: 41}},  // triggers fetch and switching into the scan mode
        {obj: {x: 42}},  // it's a 'bad' doc but it will be scanned, not fetched
        {obj: 43},
        {obj: 44},  // triggers exit from the scan mode
        {obj: 45},
        {obj: {x: 46}},  // triggers fetch and switching into the scan mode
        {obj: 47},
        {obj: {x: 48}},  // it's a 'bad' doc but it will be scanned, not fetched
        {obj: 49},       // triggers exit from the scan mode
        {obj: 50},
    ];

    coll.drop();
    coll.insert(docs);
    assert.commandWorked(coll.createIndex({"$**": "columnstore"}));

    const explain = coll.explain("executionStats").find({}, {obj: 1}).finish();
    const stats = getRowstoreStats(explain);

    assert.eq(2, stats.fetches, "Expected number of fetches. " + tojson(explain));
    assert.eq(6, stats.scans, "Expected number of scans. " + tojson(explain));
})();

(function testScanBatchSizeResetAfterGoodRead() {
    setBatchSizeMinAndMax({min: 1, max: defaultMax});

    const docs = [
        {obj: 40},
        {obj: {x: 41}},  // triggers fetch and switching into the scan mode
        {obj: 42},       // scanned
        {obj: {x: 43}},  // 'bad' again -> fetch and grow batch size
        {obj: 44},
        {obj: 45},
        {obj: 46},
        {obj: 47},
        {obj: 48},
        {obj: 49},       // by now should have exited from the scan mode even with a bigger batch
        {obj: {x: 50}},  // triggers fetch and switching into the scan mode with the min batch size
        {obj: 51},       // scanned
        {obj: {x: 52}},  // checked and must trigger fetch (if the batch size isn't reset it would
                         // be scanned)
    ];

    coll.drop();
    coll.insert(docs);
    assert.commandWorked(coll.createIndex({"$**": "columnstore"}));

    const explain = coll.explain("executionStats").find({}, {obj: 1}).finish();
    const stats = getRowstoreStats(explain);

    assert.eq(4, stats.fetches, "Expected number of fetches. " + tojson(explain));
    assert.gte(stats.scans, 4, "Expected number of scans. " + tojson(explain));
})();

(function testNeverScanIfHavePerpathFilters() {
    setBatchSizeMinAndMax({min: defaultMin, max: defaultMax});

    const docs = [
        {a: 5, obj: 40},
        {a: 5, obj: {x: 41}},
        {a: 5, obj: {x: 42}},
        {a: 5, obj: {x: 43}},
        {a: 5, obj: 44},
    ];

    coll.drop();
    coll.insert(docs);
    assert.commandWorked(coll.createIndex({"$**": "columnstore"}));

    const explain = coll.explain("executionStats").find({a: 5}, {obj: 1}).finish();
    const stats = getRowstoreStats(explain);

    assert.eq(3, stats.fetches, "Expected number of fetches. " + tojson(explain));
    assert.eq(0, stats.scans, "Expected number of scans. " + tojson(explain));
})();

(function testMaxSizeOfBatch() {
    setBatchSizeMinAndMax({min: 1, max: 9});

    const count = 1000;
    coll.drop();
    for (let i = 0; i < count; i++) {
        coll.insert({obj: {x: i}});
    }
    assert.commandWorked(coll.createIndex({"$**": "columnstore"}));

    const explain = coll.explain("executionStats").find({}, {obj: 1}).finish();
    const stats = getRowstoreStats(explain);

    // All records have to be read from the row store. By default, we'd keep growing the batch size
    // until it gets pretty large and, essentially, we settle on just scanning the row store with
    // infrequent checks against the index. The checks come paired with fetches. So when the max is
    // set low, we should see many fetches.
    assert.eq(count,
              stats.fetches + stats.scans,
              "All docs have to be read from the rowstore one way or another. " + tojson(explain));
    assert.gt(stats.fetches, count / 10, "Expected number of fetches. " + tojson(explain));
})();

(function testMaxIsLowerThanMin() {
    setBatchSizeMinAndMax({min: 99, max: 3});

    const count = 1000;
    coll.drop();
    for (let i = 0; i < count; i++) {
        coll.insert({obj: {x: i}});
    }
    assert.commandWorked(coll.createIndex({"$**": "columnstore"}));

    const explain = coll.explain("executionStats").find({}, {obj: 1}).finish();
    const stats = getRowstoreStats(explain);

    // All records have to be read from the row store. When max is set below min, the batch size
    // will be fixed at min.
    assert.eq(count,
              stats.fetches + stats.scans,
              "All docs have to be read from the rowstore one way or another. " + tojson(explain));
    assert.eq(stats.fetches, count / 100, "Expected number of fetches. " + tojson(explain));
})();

MongoRunner.stopMongod(mongod);
}());

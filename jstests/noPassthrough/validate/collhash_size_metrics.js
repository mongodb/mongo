/**
 * Verifies the size-metrics log line emitted during collHash validation. It is enabled only on the
 * startup '--validate' path (fleet validation), never by the 'validate' command. Checks that:
 *   - the 'validate' command never emits the line,
 *   - a collHash startup '--validate' emits one self-consistent line per b-tree (collection record
 *     store and each index), and
 *   - a non-collHash startup '--validate' emits nothing.
 *
 * @tags: [
 *   requires_persistence,
 *   requires_wiredtiger,
 * ]
 */

const kSizeMetricsLogId = 12951900;

// The 'attr' object of every size-metrics log line currently in the global log. Each carries the
// metrics as structured fields (leafPages, keyCount, leafPageSizeHistogram, ...).
function getSizeMetricAttrs(conn) {
    const globalLog = assert.commandWorked(conn.adminCommand({getLog: "global"})).log;
    const attrs = [];
    for (const line of globalLog) {
        let entry;
        try {
            entry = JSON.parse(line);
        } catch (e) {
            continue;
        }
        if (entry.id === kSizeMetricsLogId) {
            attrs.push(entry.attr);
        }
    }
    return attrs;
}

// The 'attr' object of every size-metrics log line from a modal '--validate' process's raw output
// (it exits, so getLog is unavailable).
function getSizeMetricAttrsFromRawLog() {
    return rawMongoProgramOutput("(" + kSizeMetricsLogId + ")")
        .split("\n")
        .filter((line) => line.trim() !== "")
        .map((line) => {
            try {
                return JSON.parse(line.split("|").slice(1).join("|"));
            } catch (e) {
                return null;
            }
        })
        .filter((entry) => entry && entry.id === kSizeMetricsLogId)
        .map((entry) => entry.attr);
}

// Histogram ceiling in bytes. Finite buckets use exclusive maxBytes; the last bucket is
// pages at or above that ceiling (gteBytes).
function histogramCeiling(hist) {
    assert.gte(
        hist.length,
        2,
        "histogram must have a finite bucket and an open-ended bucket",
        hist,
    );
    const last = hist[hist.length - 1];
    const lastFinite = hist[hist.length - 2];
    assert.eq(undefined, last.maxBytes, "open-ended bucket must not use maxBytes", hist);
    assert.eq(
        lastFinite.maxBytes,
        last.gteBytes,
        "gteBytes must equal the last finite maxBytes",
        hist,
    );
    return last.gteBytes;
}

let conn = MongoRunner.runMongod();
const dbName = jsTestName();
const collName = jsTestName();

// Enough sizeable documents to span many leaf pages and force an internal page.
const kNumDocs = 5000;
const padding = "x".repeat(120);
{
    const coll = conn.getDB(dbName).getCollection(collName);
    let bulk = coll.initializeUnorderedBulkOp();
    for (let i = 0; i < kNumDocs; i++) {
        bulk.insert({_id: i, a: i, pad: padding});
    }
    assert.commandWorked(bulk.execute());
}

// Size metrics are measured from on-disk page images, so restart to flush the dataset and start
// with an empty cache; the walk then reads (and accounts) every page from disk. Mirrors production,
// where collHash validation runs against a checkpointed, on-disk data set.
MongoRunner.stopMongod(conn);
conn = MongoRunner.runMongod({restart: true, cleanData: false, dbpath: conn.dbpath});

const db = conn.getDB(dbName);
const coll = db.getCollection(collName);

// The 'validate' command must never emit a size-metrics line, regardless of options.
assert.eq(kNumDocs, coll.find().itcount());
assert.commandWorked(coll.validate());
assert.commandWorked(coll.validate({collHash: true}));
assert.eq(0, getSizeMetricAttrs(conn).length, "the validate command must never emit size metrics");

// Everything below exercises the startup '--validate' path (the only place size metrics are
// enabled). Stop the server to release the dbpath; each run starts a modal mongod that validates
// and exits.
const dbpath = conn.dbpath;
const port = conn.port;
MongoRunner.stopMongod(conn);

// Runs 'mongod --validate' scoped to our collection with the given inner options, asserts a clean
// exit, and returns the emitted size-metrics log 'attr' objects.
function runModalValidate(innerOptions) {
    clearRawMongoProgramOutput();
    const exitCode = runMongoProgram(
        "mongod",
        "--validate",
        "--port",
        port,
        "--dbpath",
        dbpath,
        "--setParameter",
        `validateDbName=${dbName}`,
        "--setParameter",
        `validateCollectionName=${collName}`,
        "--setParameter",
        `collectionValidateOptions={options: ${innerOptions}}`,
    );
    assert.eq(
        MongoRunner.EXIT_CLEAN,
        exitCode,
        `modal '--validate' with options ${innerOptions} did not exit cleanly`,
    );
    return getSizeMetricAttrsFromRawLog();
}

// A collHash startup validation emits one size-metrics line per b-tree.
const metrics = runModalValidate("{collHash: true}");
jsTest.log.info("Collected startup size-metrics attrs", {metrics});
assert.gte(metrics.length, 2, "expected a line for the collection and at least one index");

// Every URI is an on-disk file URI and every histogram sums to the reported leaf-page count.
for (const m of metrics) {
    assert(m.uri.startsWith("file:") && m.uri.endsWith(".wt"), "URI is not an on-disk file URI", m);
    const hist = m.leafPageSizeHistogram;
    const lastFiniteMax = histogramCeiling(hist);
    const firstMax = hist[0].maxBytes;
    assert.eq(
        firstMax * (hist.length - 1),
        lastFiniteMax,
        "finite histogram buckets are not equal-width",
        m,
    );
    const histSum = hist.reduce((a, b) => a + b.count, 0);
    assert.eq(histSum, m.leafPages, "histogram buckets do not sum to the leaf page count", m);
    assert.gte(m.leafPages, 1, "expected at least one leaf page", m);
    // Clean shutdown before validation means every page has an on-disk image.
    assert.eq(0, m.pagesWithoutImage, "walk skipped pages that had no on-disk image", m);
}

// The collection record store (identified by its file URI): key count equals the document count,
// spanning multiple leaf pages plus an internal page.
const collMetrics = metrics.filter((m) => m.uri.includes("collection-"));
assert.eq(1, collMetrics.length, "expected exactly one collection line", {metrics});
const cm = collMetrics[0];
assert.eq(kNumDocs, cm.keyCount, "collection key count does not match record count", cm);
assert.eq(cm.keyCount, cm.valueCount, "collection key and value counts differ", cm);
assert.gt(cm.leafPages, 1, "expected multiple leaf pages", cm);
assert.gte(cm.internalPages, 1, "expected at least one internal page", cm);
assert.gt(cm.leafBytes, 0, "expected non-zero leaf bytes", cm);
assert.gt(cm.valueBytes, 0, "expected non-zero value bytes", cm);
assert.eq(0, cm.overflowPages, "did not expect overflow pages for small documents", cm);
const collCeiling = histogramCeiling(cm.leafPageSizeHistogram);
// 32 KiB is on-disk maxleafpage; 128 KiB is the pre-compression budget once WiredTiger publishes
// hist_ceiling. Accept either so this test holds across that import.
assert(
    collCeiling === 32 * 1024 || collCeiling === 128 * 1024,
    "collection histogram ceiling is neither 32 KiB nor 128 KiB",
    {cm, collCeiling},
);

// At least one index (the _id index) must have produced its own line.
const indexMetrics = metrics.filter((m) => m.uri.includes("index-"));
assert.gte(indexMetrics.length, 1, "expected at least one index line", {metrics});
for (const im of indexMetrics) {
    assert.eq(kNumDocs, im.keyCount, "index key count does not match record count", im);
    assert.eq(
        16 * 1024,
        histogramCeiling(im.leafPageSizeHistogram),
        "index histogram ceiling is not 16 KiB",
        im,
    );
}

// Size metrics are coupled to collHash: a non-collHash startup validation must not emit the line.
assert.eq(
    0,
    runModalValidate("{collHash: false}").length,
    "size metrics emitted for a non-collHash startup validation",
);

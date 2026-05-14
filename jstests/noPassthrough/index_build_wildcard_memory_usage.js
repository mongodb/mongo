/**
 * Builds a wildcard index with some unique multikey paths and ensures that we do not blow up
 * the memory and cause excessive spilling.
 */
const maxMemUsageMB = 50;
const replSet = new ReplSetTest({
    nodes: 1,
    nodeOptions: {
        setParameter: {
            maxIndexBuildMemoryUsageMegabytes: maxMemUsageMB,
        },
    },
});

replSet.startSet();
replSet.initiate();

const primary = replSet.getPrimary();
const testDB = primary.getDB("test");
const coll = testDB[jsTestName()];

const docs = maxMemUsageMB;
// A base field name that is roughly 1MB in size.
const baseName = "a".repeat(1024 * 1024 - 48);
const batchSize = 10;
let d = 0;
while (d < docs) {
    const bulk = coll.initializeUnorderedBulkOp();
    for (let i = 0; i < batchSize; i++) {
        // Insert documents with an array name that results in unique multikey paths.
        // Every document creates two keys: ~1MB user key and ~1MB metadata key.
        bulk.insert({[`${baseName}_${d + i}`]: [1]});
    }
    d += batchSize;
    assert.commandWorked(bulk.execute());
}

// Inserting ~100MB of data into the index should have caused about 2 spills.
// However, if not properly spilled, the multikey metadata keys would take up about 50MB of memory,
// leaving no memory to sort the following small keys.
let objs = [];
for (let i = 0; i < 1000; i++) {
    objs.push({_id: i, arr: [i]});
}
assert.commandWorked(coll.insertMany(objs));

// While we build the index, we must never exceed our memory limit.
const awaitShell = startParallelShell(() => {
    while (!db.getSiblingDB("test").signal.findOne()) {
        const stats = assert.commandWorked(db.serverStatus()).indexBulkBuilder;
        print("mem usage: " + stats.memUsage + ", spilled ranges: " + stats.spilledRanges);
        // The index build memory usage is actually a high-water mark. Include a buffer into our
        // assertion to account of any extra memory usage before spilling.
        const memBuffer = 1 * 1024 * 1024;
        const maxMemUsage = 50 * 1024 * 1024;
        assert.lte(stats.memUsage, maxMemUsage + memBuffer);
        // Estimating 2 spills for the large documents and 1 final spill.
        assert.between(0, stats.spilledRanges, 4);
        sleep(500);
    }
}, primary.port);

assert.commandWorked(coll.createIndex({"$**": 1}));
assert.commandWorked(testDB.signal.insert({done: true}));

awaitShell();

const stats = assert.commandWorked(primary.getDB("admin").serverStatus()).indexBulkBuilder;
print("Final index build stats: " + tojson(stats));
// After the index build completion the indexBulkBuilder.memUsage should reset back to zero
const memUsage = stats.memUsage;
assert.eq(memUsage, 0);
assert.between(1, stats.spilledRanges, 4);

replSet.stopSet();

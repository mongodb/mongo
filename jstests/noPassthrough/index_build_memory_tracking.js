/**
 * Builds an index with many large keys and ensures that we stay below the memory limit.
 */
const maxMemUsageMB = 50;
const replSet = new ReplSetTest({
    nodes: 1,
    nodeOptions: {
        setParameter: {
            maxIndexBuildMemoryUsageMegabytes: maxMemUsageMB,
        }
    },

});

replSet.startSet();
replSet.initiate();

const primary = replSet.getPrimary();
const testDB = primary.getDB('test');
const coll = testDB.index_build_wildcard;

const docs = 50;
const bigArr = new Array(100000).fill('x'.repeat(10));
const batchSize = 10;
const padding = 'x'.repeat(1000);
let d = 0;
while (d < docs) {
    const bulk = coll.initializeUnorderedBulkOp();
    for (let i = 0; i < batchSize; i++) {
        bulk.insert({a: (d + i) + padding, arr: bigArr});
    }
    d += batchSize;
    assert.commandWorked(bulk.execute());
}

// While we build the index, we must never exceed our memory limit.
const awaitShell = startParallelShell(() => {
    while (!db.getSiblingDB('test').signal.findOne()) {
        const memUsage = assert.commandWorked(db.serverStatus()).indexBulkBuilder.memUsage;
        print("mem usage: " + memUsage);
        // The index build memory usage is actually a high-water mark. Include a buffer into our
        // assertion to account of any extra memory usage before spilling.
        const memBuffer = 1 * 1024 * 1024;
        const maxMemUsage = 50 * 1024 * 1024;
        assert.lte(memUsage, maxMemUsage + memBuffer);
        sleep(500);
    }
}, primary.port);

// Build a wildcard index that generates many large keys, forcing the index build to spill
// multiple times.
assert.commandWorked(coll.createIndex({"$**": 1}));
assert.commandWorked(testDB.signal.insert({done: true}));

awaitShell();

replSet.stopSet();

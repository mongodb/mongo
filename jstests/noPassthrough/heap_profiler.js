// Exercises the basic heap profiler path.

function testHeapProfiler(db) {
    const adminDb = db.getDB('admin');

    const ss = adminDb.serverStatus();

    assert(ss.hasOwnProperty("heapProfile"));
    jsTestLog(ss.heapProfile);

    assert(ss.heapProfile.stats.bytesAllocated > 0);
    assert(ss.heapProfile.hasOwnProperty("stacks") &&
           Object.keys(ss.heapProfile.stacks).length > 0);

    MongoRunner.stopMongod(db);
}

let db;
try {
    db = MongoRunner.runMongod({setParameter: "heapProfilingEnabled=true"});
} catch (err) {
    jsTestLog("Heap profiler is not available on this platform.");
    quit();
}

testHeapProfiler(db);

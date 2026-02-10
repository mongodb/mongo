// Exercises the basic heap profiler path.

const minProfilingRate = 10000;

function testHeapProfilerOnStartup(opts) {
    let db;
    try {
        db = MongoRunner.runMongod(opts);
    } catch (err) {
        jsTestLog("Heap profiler is not available on this platform.");
        quit();
    }

    const adminDb = db.getDB("admin");

    jsTestLog("Heap profile stats are present when sample interval bytes are > 0 at startup");
    const ss = adminDb.serverStatus();
    assert(ss.hasOwnProperty("heapProfile"));
    jsTestLog(ss.heapProfile);

    assert(ss.heapProfile.stats.bytesAllocated > 0);
    assert(ss.heapProfile.hasOwnProperty("stacks") && Object.keys(ss.heapProfile.stacks).length > 0);

    MongoRunner.stopMongod(db);
}

function testHeapProfilerOnRuntime() {
    const db = MongoRunner.runMongod();
    const adminDb = db.getDB("admin");

    jsTestLog("Heap profile stats are present when sample interval bytes > 0");
    adminDb.runCommand({setParameter: 1, heapProfilingSampleIntervalBytes: minProfilingRate});
    assert.soon(() => {
        const ss = adminDb.serverStatus();

        return (
            ss.hasOwnProperty("heapProfile") &&
            ss.heapProfile.stats.bytesAllocated > 0 &&
            ss.heapProfile.hasOwnProperty("stacks") &&
            Object.keys(ss.heapProfile.stacks).length > 0
        );
    }, "Heap profile serverStatus section is not present");

    jsTestLog("Heap profile stats are excluded when sample interval bytes = 0");
    adminDb.runCommand({setParameter: 1, heapProfilingSampleIntervalBytes: 0});
    assert.soon(() => {
        const ss = adminDb.serverStatus();

        return !ss.hasOwnProperty("heapProfile");
    }, "Heap profile serverStatus section was found when it should be disabled");

    MongoRunner.stopMongod(db);
}

function testHeapProfilerRespectsMemoryLimit() {
    let db;
    try {
        db = MongoRunner.runMongod({
            setParameter: {heapProfilingSampleIntervalBytes: minProfilingRate, heapProfilingMaxObjects: 2},
        });
    } catch (err) {
        jsTestLog("Heap profiler is not available on this platform.");
        quit();
    }

    const adminDb = db.getDB("admin");

    jsTestLog("Heap profiler auto-disables itself after hitting the max object limit.");
    assert.soon(() => {
        const res = adminDb.runCommand({getParameter: 1, "heapProfilingSampleIntervalBytes": 1});

        return res["ok"] === 1 && res["heapProfilingSampleIntervalBytes"].toNumber() === 0;
    }, "Heap profile was not disabled after hitting maximum objects limit.");

    MongoRunner.stopMongod(db);
}

function testHeapProfilingMaxObjectsRuntimeUpdate() {
    let db;
    try {
        db = MongoRunner.runMongod({
            setParameter: {heapProfilingSampleIntervalBytes: minProfilingRate},
        });
    } catch (err) {
        jsTestLog("Heap profiler is not available on this platform.");
        quit();
    }

    const adminDb = db.getDB("admin");

    jsTestLog("Test runtime update of heapProfilingMaxObjects");

    // Get initial value
    let res = adminDb.runCommand({getParameter: 1, heapProfilingMaxObjects: 1});
    assert.commandWorked(res);
    const initialValue = res.heapProfilingMaxObjects;
    jsTestLog("Initial heapProfilingMaxObjects: " + initialValue);

    // Set a new value at runtime
    const newValue = 100000;
    assert.commandWorked(adminDb.runCommand({setParameter: 1, heapProfilingMaxObjects: newValue}));

    // Verify the value was updated
    res = adminDb.runCommand({getParameter: 1, heapProfilingMaxObjects: 1});
    assert.commandWorked(res);
    assert.eq(res.heapProfilingMaxObjects, newValue, "heapProfilingMaxObjects was not updated to " + newValue);

    // Set back to 0 (use heuristic)
    assert.commandWorked(adminDb.runCommand({setParameter: 1, heapProfilingMaxObjects: 0}));
    res = adminDb.runCommand({getParameter: 1, heapProfilingMaxObjects: 1});
    assert.commandWorked(res);
    assert.eq(res.heapProfilingMaxObjects, 0);

    // Verify heap profiler still works after the update
    const ss = adminDb.serverStatus();
    assert(ss.hasOwnProperty("heapProfile"), "heapProfile section should exist");

    MongoRunner.stopMongod(db);
}

const conn = MongoRunner.runMongod();
const adminDb = conn.getDB("admin");
const buildInfo = adminDb.runCommand("buildInfo");
jsTestLog(buildInfo);
MongoRunner.stopMongod(conn);

assert(buildInfo.hasOwnProperty("allocator"));

if (buildInfo.allocator === "tcmalloc-google") {
    jsTestLog("Testing tcmalloc-google heap profiler");
    testHeapProfilerOnStartup({setParameter: {heapProfilingSampleIntervalBytes: minProfilingRate}});
    testHeapProfilerOnRuntime();
    testHeapProfilerRespectsMemoryLimit();
    testHeapProfilingMaxObjectsRuntimeUpdate();
} else if (buildInfo.allocator === "tcmalloc-gperf") {
    jsTestLog("Testing tcmalloc-gperf heap profiler");
    testHeapProfilerOnStartup({
        setParameter: {heapProfilingEnabled: 1, heapProfilingSampleIntervalBytes: minProfilingRate},
    });
}

/**
 * Tracks ephemeralForTest's memory usage metrics on a consistent workload.
 */
(function() {
"use strict";

const storageEngine = jsTest.options().storageEngine;
if (storageEngine != "ephemeralForTest") {
    return;
}

let testColl1 = db.jstests_ephemeralForTest_metrics1;
let testColl2 = db.jstests_ephemeralForTest_metrics2;
let testColl3 = db.jstests_ephemeralForTest_metrics3;

const s1 = startParallelShell(() => {
    testColl1 = db.jstests_ephemeralForTest_metrics1;
    for (let i = 0; i < 50000; ++i) {
        assert.writeOK(testColl1.save({x: Math.floor(Math.random() * 1024 * 1024), y: "y"}));
    }
    assert.commandWorked(testColl1.createIndex({x: 1, y: 1}));
    for (let i = 0; i < 50000; ++i) {
        assert.writeOK(testColl1.save({x: Math.floor(Math.random() * 1024 * 1024), y: "y"}));
    }
});

const s2 = startParallelShell(function() {
    testColl2 = db.jstests_ephemeralForTest_metrics2;
    for (let i = 0; i < 50000; ++i) {
        assert.writeOK(testColl2.save({x: "x", y: Math.floor(Math.random() * 1024 * 1024)}));
    }

    for (let i = 0; i < 100; ++i) {
        assert.commandWorked(
            testColl2.updateOne({x: "x"}, {$set: {x: Math.floor(Math.random() * 1024 * 1024)}}));
    }
});

const s3 = startParallelShell(function() {
    testColl3 = db.jstests_ephemeralForTest_metrics3;
    for (let i = 0; i < 50000; ++i) {
        assert.writeOK(testColl3.save({x: i, y: Math.floor(Math.random() * 1024 * 1024)}));
    }
});

s1();
s2();
s3();

let serverStatus = db.serverStatus().ephemeralForTest;
print("Total Memory Usage: " + serverStatus.totalMemoryUsage + " Bytes.");
print("Total Number of Nodes: " + serverStatus.totalNodes + ".");
print("Average Number of Children: " + serverStatus.averageChildren + ".");

for (let i = 0; i < 50000; ++i) {
    assert.commandWorked(testColl3.deleteOne({x: i}));
}

serverStatus = db.serverStatus().ephemeralForTest;
print("After Deletion:");
print("Total Memory Usage: " + serverStatus.totalMemoryUsage + " Bytes.");
print("Total Number of Nodes: " + serverStatus.totalNodes + ".");
print("Average Number of Children: " + serverStatus.averageChildren + ".");
})();
/**
 * Tests that percentage-based memory limits for the mongod are set correctly.
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {IndexBuildTest} from "jstests/noPassthrough/libs/index_builds/index_build.js";

const replSet = new ReplSetTest({
    nodes: 1,
});
replSet.startSet();
replSet.initiate();

const minIndexBuildMemoryLimitBytes = 50 * 1024 * 1024;

const primary = replSet.getPrimary();
const testDB = primary.getDB('test');
const coll = testDB[jsTestName()];

const availableMemoryMB = assert.commandWorked(testDB.hostInfo()).system.memSizeMB;
const availableMemoryBytes = availableMemoryMB * 1024 * 1024;
jsTestLog("System Memory: [" + availableMemoryMB.toString() + "] MB");
jsTestLog("System Memory: [" + availableMemoryBytes.toString() + "] Bytes");

const docs = 50;
const batchSize = 10;
let d = 0;
while (d < docs) {
    const bulk = coll.initializeUnorderedBulkOp();
    for (let i = 0; i < batchSize; i++) {
        let val = d + i;
        bulk.insert({a: val, b: val, c: val, d: val, e: val, f: val, g: val});
    }
    d += batchSize;
    assert.commandWorked(bulk.execute());
}

function roughlyEqual(expected, actual, digits) {
    let expectedNormalized = Math.trunc(expected / Math.pow(10, digits));
    let actualNormalized = Math.trunc(actual / Math.pow(10, digits));
    jsTestLog("expected: [" + expectedNormalized.toString() + "] == [" +
              actualNormalized.toString() + "]");
    return expectedNormalized == actualNormalized;
}

function assertLogIdExists(logId) {
    assert.soon(() => rawMongoProgramOutput(':' + logId + ','),
                "Expected index build log message not found.",
                10 * 1000);
}

function runTest(
    indexSpec, indexBuildMemoryLimit, expectedComputedLimit, expectedLogId, isSuccess) {
    clearRawMongoProgramOutput();

    if (indexBuildMemoryLimit != "default") {
        if (!isSuccess) {
            assert.commandFailedWithCode(
                primary.adminCommand(
                    {setParameter: 1, maxIndexBuildMemoryUsageMegabytes: indexBuildMemoryLimit}),
                ErrorCodes.BadValue);
            return;
        } else {
            assert.commandWorked(primary.adminCommand(
                {setParameter: 1, maxIndexBuildMemoryUsageMegabytes: indexBuildMemoryLimit}));
        }
    }

    const awaitIndexBuild = IndexBuildTest.startIndexBuild(primary, coll.getFullName(), indexSpec);
    awaitIndexBuild();
    IndexBuildTest.waitForIndexBuildToStop(testDB);

    assertLogIdExists(expectedLogId);
    let indexBuildMemoryLogLines = rawMongoProgramOutput('INDEX.*:' + expectedLogId + ',');
    let matchedLine = indexBuildMemoryLogLines.match('limitBytes":[0-9]+');
    assert.neq(null, matchedLine);
    jsTestLog("matchedLimit: " + matchedLine);
    let computedLimit = parseInt(matchedLine.toString().split(":")[1]);
    jsTestLog("computedLimit: " + computedLimit);
    jsTestLog("expectedComputedLimit: " + expectedComputedLimit);

    assert(roughlyEqual(expectedComputedLimit, computedLimit, 6));
}

function runPercentLimitTest(indexSpec,
                             indexBuildMemoryPercentLimit,
                             expectedComputedLimit,
                             expectedLogId,
                             isSuccess = true) {
    jsTestLog("runPercentLimitTest(" + tojson(indexSpec) + ", " +
              indexBuildMemoryPercentLimit.toString() + ", " + expectedComputedLimit.toString() +
              ", " + expectedLogId.toString() + ", isSuccess=" + isSuccess + ")");
    runTest(
        indexSpec, indexBuildMemoryPercentLimit, expectedComputedLimit, expectedLogId, isSuccess);
}

function runByteLimitTest(indexSpec,
                          indexBuildMemoryByteLimitMB,
                          expectedComputedLimitBytes,
                          expectedLogId,
                          isSuccess = true) {
    jsTestLog("runByteLimitTest(" + tojson(indexSpec) + ", " +
              indexBuildMemoryByteLimitMB.toString() + ", " +
              expectedComputedLimitBytes.toString() + ", " + expectedLogId.toString() +
              ", isSuccess=" + isSuccess + ")");
    runTest(indexSpec,
            indexBuildMemoryByteLimitMB,
            expectedComputedLimitBytes,
            expectedLogId,
            isSuccess);
}

//
// Check Default
//
runByteLimitTest({a: 1}, "default", 200 * 1024 * 1024, "10448900");

//
// Percentage-based tests.
//

// Beneath lower bound
if (availableMemoryMB <= 50 * 1024) {
    runPercentLimitTest({b: 1}, 0.001, minIndexBuildMemoryLimitBytes, "10448901");
    assertLogIdExists("10448902");
} else {
    runPercentLimitTest({b: 1}, (1 / 1000), (availableMemoryBytes / 1000), "10448900");
}

// Within bounds
if (availableMemoryMB >= 500) {
    runPercentLimitTest({c: 1}, 0.1, (0.1 * availableMemoryBytes), "10448901");
} else {
    runPercentLimitTest({c: 1}, 0.1, minIndexBuildMemoryLimitBytes, "10448902");
}

// Above upper bound
runPercentLimitTest({d: 1}, 0.99, 0.8 * availableMemoryBytes, "10448901", false);

//
// Bytes-based tests.
//

// Beneath lower bound
runByteLimitTest({e: 1}, 30, minIndexBuildMemoryLimitBytes, "10448902", false);
runByteLimitTest({e: 1}, 0, 1024, "10448900", false);
runByteLimitTest({e: 1}, -5, 1024, "10448900", false);

// Within bounds
runByteLimitTest({f: 1}, 1024, 1024 * 1024 * 1024, "10448900");

// Bad Values
runByteLimitTest({g: 1}, NumberDecimal(NaN), 1024, "10448900", false);
runByteLimitTest({g: 1}, "NaN", 1024, "10448900", false);

// Above available memory (?)
// runByteLimitTest({c: 1}, 2 * availableMemoryMB, , "10448900");

replSet.stopSet();

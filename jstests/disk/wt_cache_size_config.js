/**
 * Verifying the various ways to set the WiredTiger cache size. The cache size can be set in GB
 * or as a percentage of the physical RAM available.
 *
 * @tags: [requires_wiredtiger]
 *
 */

import {isLinux} from "jstests/libs/os_helpers.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

if (!isLinux()) {
    quit();
}
const bytesPerKB = 1024;
const bytesPerMB = bytesPerKB * 1024;
const bytesPerGB = bytesPerMB * 1024;
const totalCacheKB = Number(
    cat("/proc/meminfo")
        .split("\n")
        .filter((str) => str.includes("MemTotal"))[0]
        .split(/\s+/)[1],
);
jsTestLog("Total process RAM available: " + totalCacheKB);

// Mirrors processinfo_linux.cpp: getMemorySizeLimit(). If a cgroup memory limit is set and
// is smaller than the system total, mongod will use that as its base for WiredTiger cache sizing.
function getMemorySizeLimitKB() {
    for (const cgroupFile of [
        "/sys/fs/cgroup/memory.max", // cgroups v2
        "/sys/fs/cgroup/memory/memory.limit_in_bytes", // cgroups v1
    ]) {
        let content;
        try {
            content = cat(cgroupFile).trim();
        } catch (e) {
            continue;
        }
        if (!content || content === "max") {
            break;
        }
        const limitBytes = Number(content);
        if (!isNaN(limitBytes)) {
            return Math.min(totalCacheKB, limitBytes / bytesPerKB);
        }
    }
    return totalCacheKB;
}
const effectiveCacheKB = getMemorySizeLimitKB();
jsTestLog("Effective process RAM (after cgroup limits): " + effectiveCacheKB);

function runTest(expectedCacheSizeBytes, serverConfig, fuzzyGBCheck = false) {
    let rst = new ReplSetTest({nodes: 1});
    rst.startSet(serverConfig);
    rst.initiate();
    let primary = rst.getPrimary();
    let actualCacheSizeBytes = assert.commandWorked(primary.adminCommand({serverStatus: 1})).wiredTiger.cache[
        "maximum bytes configured"
    ];

    // Verify storage engine cache size in effect
    if (fuzzyGBCheck) {
        assert.eq(Math.round(expectedCacheSizeBytes / bytesPerGB), Math.round(actualCacheSizeBytes / bytesPerGB));
    } else {
        assert.eq(expectedCacheSizeBytes, actualCacheSizeBytes);
    }

    rst.stopSet();
}

TestData.storageEngineCacheSizePct = "";
TestData.storageEngineCacheSizeGB = "";

//
// Byte-based tests.
//
TestData.storageEngineCacheSizeGB = "0.4";
runTest(0.4 * bytesPerGB, {}, true);
TestData.storageEngineCacheSizeGB = "";

runTest(0.3 * bytesPerGB, {wiredTigerCacheSizeGB: "0.3"}, true);

// Test upper limit
runTest(10 * 1000 * 1000 * bytesPerMB, {wiredTigerCacheSizeGB: "10000"}, false);

// Test default value
runTest(Math.max((effectiveCacheKB / bytesPerKB - 1024) * 0.5, 256.0) * bytesPerMB, {}, true);

//
// Percentage-based tests.
//
TestData.storageEngineCacheSizePct = "0.01";
runTest(0.01 * effectiveCacheKB * bytesPerKB, {}, true);
TestData.storageEngineCacheSizePct = "";

runTest(0.03 * effectiveCacheKB * bytesPerKB, {wiredTigerCacheSizePct: "0.03"}, true);

// Test lower limit
runTest(256 * bytesPerMB, {wiredTigerCacheSizePct: "0.00000001"}, false);

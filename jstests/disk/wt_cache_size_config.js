/**
 * Verifying the various ways to set the WiredTiger cache size. The cache size can be set in GB
 * or as a percentage of the physical RAM available.
 *
 * @tags: [requires_wiredtiger]
 *
 */

import {
    isLinux,
} from "jstests/libs/os_helpers.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

if (!isLinux()) {
    quit();
}
const bytesPerKB = 1024;
const bytesPerMB = bytesPerKB * 1024;
const bytesPerGB = bytesPerMB * 1024;
const totalCacheKB = Number(
    cat("/proc/meminfo").split("\n").filter((str) => str.includes("MemTotal"))[0].split(/\s+/)[1]);
jsTestLog("Total process RAM available: " + totalCacheKB);

function runTest(expectedCacheSizeBytes, serverConfig, fuzzyGBCheck = false) {
    let rst = new ReplSetTest({nodes: 1});
    rst.startSet(serverConfig);
    rst.initiate();
    let primary = rst.getPrimary();
    let actualCacheSizeBytes = assert.commandWorked(primary.adminCommand({serverStatus: 1}))
                                   .wiredTiger.cache["maximum bytes configured"];

    // Verify storage engine cache size in effect
    if (fuzzyGBCheck) {
        assert.eq(Math.round(expectedCacheSizeBytes / bytesPerGB),
                  Math.round(actualCacheSizeBytes / bytesPerGB));
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
runTest(Math.max(((totalCacheKB / bytesPerKB) - 1024) * 0.5, 256.0) * bytesPerMB, {}, true);

//
// Percentage-based tests.
//
TestData.storageEngineCacheSizePct = "0.01";
runTest(0.01 * totalCacheKB * bytesPerKB, {}, true);
TestData.storageEngineCacheSizePct = "";

runTest(0.03 * totalCacheKB * bytesPerKB, {wiredTigerCacheSizePct: "0.03"}, true);

// Test lower limit
runTest(256 * bytesPerMB, {wiredTigerCacheSizePct: "0.00000001"}, false);
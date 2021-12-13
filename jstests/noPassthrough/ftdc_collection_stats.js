// Validate FTDC can gather collStats for collections

// Scenarios that are tested
// -------------------------
// Bad namespace on startup
// Valid collections
// Bad namespace at runtime
// Missing collection

load('jstests/libs/ftdc.js');

(function() {
'use strict';

// Validate we fail at startup on bad input
let startFailed = MongoRunner.runMongod(
    {waitForConnect: false, setParameter: "diagnosticDataCollectionStatsNamespaces=local"});
waitFailedToStart(startFailed.pid, 2);

let m = MongoRunner.runMongod(
    {setParameter: "diagnosticDataCollectionStatsNamespaces=local.startup_log"});
let adminDb = m.getDB('admin');

assert.eq(getParameter(adminDb, "diagnosticDataCollectionStatsNamespaces"), ["local.startup_log"]);

// Validate that collection stats are collected
let doc = verifyGetDiagnosticData(adminDb);
assert.eq(doc.collectionStats["local.startup_log"].ns, "local.startup_log");

// Validate that incorrect changes have no effect
assert.commandFailed(setParameter(adminDb, {"diagnosticDataCollectionStatsNamespaces": ["local"]}));

assert.eq(getParameter(adminDb, "diagnosticDataCollectionStatsNamespaces"), ["local.startup_log"]);

// Validate that collection stats are collected for runtime collections
assert.commandWorked(setParameter(
    adminDb,
    {"diagnosticDataCollectionStatsNamespaces": ["admin.system.version", "admin.does_not_exist"]}));
assert.soon(() => {
    let result = assert.commandWorked(adminDb.runCommand("getDiagnosticData"));
    jsTestLog("Collected: " + tojson(result));
    let collectionStats = result.data.collectionStats;
    return collectionStats.hasOwnProperty("admin.system.version") &&
        collectionStats["admin.system.version"].ns == "admin.system.version" &&
        collectionStats["admin.does_not_exist"].ns == "admin.does_not_exist" != undefined;
});

// Validate that when it is disabled, we stop collecting
assert.commandWorked(setParameter(adminDb, {"diagnosticDataCollectionStatsNamespaces": []}));

assert.eq(getParameter(adminDb, "diagnosticDataCollectionStatsNamespaces"), []);

assert.soon(() => {
    let result = assert.commandWorked(adminDb.runCommand("getDiagnosticData"));
    jsTestLog("Collected: " + tojson(result));
    return !result.data.hasOwnProperty("collectionStats");
});

MongoRunner.stopMongod(m);
})();

// Validate FTDC can gather collStats for collections

// Scenarios that are tested
// -------------------------
// Bad namespace on startup
// Valid collections
// Bad namespace at runtime
// Missing collection

import {getParameter, setParameter, verifyGetDiagnosticData, waitFailedToStart} from "jstests/libs/ftdc.js";

// Validate we fail at startup on bad input
let startFailed = MongoRunner.runMongod({
    waitForConnect: false,
    setParameter: "diagnosticDataCollectionStatsNamespaces=local",
});
waitFailedToStart(startFailed.pid, 2);

let m = MongoRunner.runMongod({setParameter: "diagnosticDataCollectionStatsNamespaces=local.startup_log"});
let adminDb = m.getDB("admin");

assert.eq(getParameter(adminDb, "diagnosticDataCollectionStatsNamespaces"), ["local.startup_log"]);

// Validate that collection stats are collected
let doc;
assert.soon(() => {
    doc = verifyGetDiagnosticData(adminDb);
    return (
        doc.collectionStats.hasOwnProperty("local.startup_log") &&
        doc.collectionStats["local.startup_log"].ns == "local.startup_log"
    );
});

// Validate that incorrect changes have no effect
assert.commandFailed(setParameter(adminDb, {"diagnosticDataCollectionStatsNamespaces": ["local"]}));

assert.eq(getParameter(adminDb, "diagnosticDataCollectionStatsNamespaces"), ["local.startup_log"]);

// Validate that collection stats are collected for runtime collections and that we do not crash
// for a non-existent collection
assert.commandWorked(
    setParameter(adminDb, {
        "diagnosticDataCollectionStatsNamespaces": ["admin.system.version", "admin.does_not_exist"],
    }),
);
assert.soon(() => {
    let result = assert.commandWorked(adminDb.runCommand("getDiagnosticData"));
    jsTestLog("Collected: " + tojson(result));
    let collectionStats = result.data.collectionStats;
    return (
        collectionStats.hasOwnProperty("admin.system.version") &&
        collectionStats["admin.system.version"].ns == "admin.system.version"
    );
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

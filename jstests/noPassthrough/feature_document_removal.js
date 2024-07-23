/**
 * Test logic that removes the obsolete feature document when an FCV upgrade is requested.
 *
 * @tags: [
 *   requires_replication,
 * ]
 */

const dbName = "test";

const rst = new ReplSetTest({nodes: 1});

/*
 * Restarts the primary and requests a FCV downgrade followed by a subsequent upgrade to test
 * feature document removal.
 */
function restartAndRequestUpgrade() {
    rst.restart(primary);
    primary = rst.getPrimary();
    db = primary.getDB(dbName);

    let adminDB = primary.getDB("admin");
    adminDB.setLogLevel(1);

    jsTestLog("Request FCV downgrade");
    assert.commandWorked(
        db.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}));

    jsTestLog("Request FCV upgrade");
    assert.commandWorked(
        db.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));
}

rst.startSet();
rst.initiate();

let primary = rst.getPrimary();
let db = primary.getDB(dbName);
const conn = db.getMongo();

// Enable failpoint for creation of mock feature document.
assert.commandWorked(db.adminCommand({configureFailPoint: "createFeatureDoc", mode: "alwaysOn"}));

assert.commandWorked(db.createCollection(jsTestName()));

// Disable the failpoint to avoid adding multiple feature documents.
assert.commandWorked(db.adminCommand({configureFailPoint: "createFeatureDoc", mode: "off"}));

restartAndRequestUpgrade();
assert(checkLog.checkContainsOnceJson(conn, 8583700));

// Redo the process to ensure the feature document was properly removed the
// first time. The feature document should no longer exist now and the removal code should not run.
restartAndRequestUpgrade();
assert(checkLog.checkContainsOnceJson(conn, 8583700));

rst.stopSet();
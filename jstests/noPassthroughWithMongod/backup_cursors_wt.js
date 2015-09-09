/**
 * Test beginBackup and endBackup functionality under WT
 * - Skip for all storage engines other than WiredTiger
 * - Run the fsyncLock command
 * - Confirm that backup cursors are created
 * - run the fsyncUnlock command
 * - Confirm that backup cursors are released
 */
(function() {
    "use strict";
    var testDB = db.getSisterDB('beginBackupTestDB');
    testDB.dropDatabase();

    // Tests the db.fsyncLock/fsyncUnlock features
    var storageEngine = db.serverStatus().storageEngine.name;

    if (storageEngine !== "wiredTiger") {
        jsTestLog("Skipping test for " + storageEngine + " as it doesn't have a backup cursor");
        return;
    }

    var beforeSS = db.serverStatus();
    var res = db.fsyncLock();
    assert(res.ok, "fsyncLock command failed");

    // Under WiredTiger we will open a backup session and cursor. Confirm that these are opened.
    var afterSS = db.serverStatus().wiredTiger.session
    beforeSS = beforeSS.wiredTiger.session
    assert.gt(afterSS["open session count"], beforeSS["open session count"],
        "WiredTiger did not open a backup session as expected");
    assert.gt(afterSS["open cursor count"], beforeSS["open cursor count"],
        "WiredTiger did not open a backup cursor as expected");

    var res = db.fsyncUnlock();
    var finalSS = db.serverStatus().wiredTiger.session;
    assert.eq(beforeSS["open session count"], finalSS["open session count"],
        "WiredTiger did not close its backup session as expected");
    assert.eq(beforeSS["open cursor count"], finalSS["open cursor count"],
        "WiredTiger did not close its backup cursor as expected after ");
}());

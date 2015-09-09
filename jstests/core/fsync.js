/**
 * Test fsyncLock functionality
 * - Skip for all storage engines which don't support fsync
 * - Run the fsyncLock command, confirm we lock correctly with currentOp
 * - Confirm that backup cursors are created and released in WT
 * - Confirm that we cannot insert during fsyncLock
 * - Confirm that writes can progress after fsyncUnlock
 * - Confirm that the command can be run repeatedly without breaking things
 * - Confirm that the pseudo commands and eval can perform fsyncLock/Unlock
 */
(function() {
    "use strict";

    // Start with a clean DB
    var fsyncLockDB = db.getSisterDB('fsyncLockTestDB');
    fsyncLockDB.dropDatabase();

    // Tests the db.fsyncLock/fsyncUnlock features
    var storageEngine = db.serverStatus().storageEngine.name;

    // As of SERVER-18899 fsyncLock/fsyncUnlock will error when called on a storage engine
    // that does not support the begin/end backup commands. The array below contains a 
    // list of engines which do support this option and should be ammended as needed. 
    var supportsFsync = db.fsyncLock();

    if ( supportsFsync.ok != 1) {
        jsTestLog("Skipping test for " + storageEngine + " as it does not support fsync");
        return;
    } else {
        db.fsyncUnlock();
    }

    var resFail = fsyncLockDB.runCommand({fsync:1, lock:1});

    // Start with a clean DB
    var fsyncLockDB = db.getSisterDB('fsyncLockTestDB');
    fsyncLockDB.dropDatabase();
 
    // Test it doesn't work unless invoked against the admin DB
    var resFail = fsyncLockDB.runCommand({fsync:1, lock:1});
    assert(!resFail.ok, "fsyncLock command succeeded against DB other than admin.");

    // Uses admin automatically and locks the server for writes
    var fsyncLockRes = db.fsyncLock();
    assert(fsyncLockRes.ok, "fsyncLock command failed against admin DB");
    assert(db.currentOp().fsyncLock, "Value in db.currentOp incorrect for fsyncLocked server");

    // Make sure writes are blocked. Spawn a write operation in a separate shell and make sure it
    // is blocked. There is really now way to do that currently, so just check that the write didn't
    // go through.
    var writeOpHandle = startParallelShell("db.getSisterDB('fsyncLockTestDB').coll.insert({x:1});");
    sleep(1000);

    // Make sure reads can still run even though there is a pending write and also that the write
    // didn't get through
    assert.eq(0, fsyncLockDB.coll.count({}));

    // Unlock and make sure the insert succeeded
    var fsyncUnlockRes = db.fsyncUnlock();
    assert(fsyncUnlockRes.ok, "fsyncUnlock command failed");
    assert(db.currentOp().fsyncLock == null, "fsyncUnlock is not null in db.currentOp");

    // Make sure the db is unlocked and the initial write made it through.
    writeOpHandle();
    fsyncLockDB.coll.insert({x:2});

    assert.eq(2, fsyncLockDB.coll.count({}));

    // Issue the fsyncLock and fsyncUnlock a second time, to ensure that we can 
    // run this command repeatedly with no problems. Additionally check that the WT
    // backup session and cursor are closed when we unlock.
    var beforeSS = db.serverStatus();
    var fsyncLockRes = db.fsyncLock();
    assert(fsyncLockRes.ok, "Second execution of fsyncLock command failed");

    // Under WiredTiger we will open a backup session and cursor. Confirm that these are opened.
    if (storageEngine == "wiredTiger") {
        var afterSS = db.serverStatus().wiredTiger.session
        beforeSS = beforeSS.wiredTiger.session
        assert.gt(afterSS["open session count"], beforeSS["open session count"],
            "WiredTiger did not open a backup session as expected");
        assert.gt(afterSS["open cursor count"], beforeSS["open cursor count"],
            "WiredTiger did not open a backup cursor as expected");
    }

    var fsyncUnlockRes = db.fsyncUnlock();
    assert(fsyncUnlockRes.ok, "Second execution of fsyncUnlock command failed");

    if (storageEngine == "wiredTiger") {
        var finalSS = db.serverStatus().wiredTiger.session;
        assert.eq(beforeSS["open session count"], finalSS["open session count"],
            "WiredTiger did not close its backup session as expected");
        assert.eq(beforeSS["open cursor count"], finalSS["open cursor count"],
            "WiredTiger did not close its backup cursor as expected after ");
    }

    // Ensure eval is not allowed to invoke fsyncLock
    assert(!db.eval('db.fsyncLock()').ok, "eval('db.fsyncLock()') should fail.");

    // Check that the fsyncUnlock pseudo-command (a lookup on cmd.$sys.unlock)
    // still has the same effect as a legitimate 'fsyncUnlock' command
    // TODO: remove this in in the release following MongoDB 3.2 when pseudo-commands
    // are removed
    var fsyncCommandRes = db.fsyncLock();
    assert(fsyncLockRes.ok, "fsyncLock command failed against admin DB");
    assert(db.currentOp().fsyncLock, "Value in db.currentOp incorrect for fsyncLocked server");
    var fsyncPseudoCommandRes = db.getSiblingDB("admin").$cmd.sys.unlock.findOne();
    assert(fsyncPseudoCommandRes.ok, "fsyncUnlock pseudo-command failed");
    assert(db.currentOp().fsyncLock == null, "fsyncUnlock is not null in db.currentOp");
}());

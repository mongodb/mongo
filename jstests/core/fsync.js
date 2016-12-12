/**
 * Test fsyncLock functionality
 * - Skip for all storage engines which don't support fsync
 * - Run the fsyncLock command, confirm we lock correctly with currentOp
 * - Confirm that we cannot insert during fsyncLock
 * - Confirm that writes can progress after fsyncUnlock
 * - Confirm that the command can be run repeatedly without breaking things
 * - Confirm that the pseudo commands and eval can perform fsyncLock/Unlock
 */
(function() {
    "use strict";

    // Start with a clean DB.
    var fsyncLockDB = db.getSisterDB('fsyncLockTestDB');
    fsyncLockDB.dropDatabase();

    // Tests the db.fsyncLock/fsyncUnlock features.
    var storageEngine = db.serverStatus().storageEngine.name;

    // As of SERVER-18899 fsyncLock/fsyncUnlock will error when called on a storage engine
    // that does not support the begin/end backup commands.
    var supportsFsync = db.fsyncLock();

    if (!supportsFsync.ok) {
        assert.commandFailedWithCode(supportsFsync, ErrorCodes.CommandNotSupported);
        jsTestLog("Skipping test for " + storageEngine + " as it does not support fsync");
        return;
    }
    db.fsyncUnlock();

    var resFail = fsyncLockDB.runCommand({fsync: 1, lock: 1});

    // Start with a clean DB
    var fsyncLockDB = db.getSisterDB('fsyncLockTestDB');
    fsyncLockDB.dropDatabase();

    // Test that a single, regular write works as expected.
    assert.writeOK(fsyncLockDB.coll.insert({x: 1}));

    // Test that fsyncLock doesn't work unless invoked against the admin DB.
    var resFail = fsyncLockDB.runCommand({fsync: 1, lock: 1});
    assert(!resFail.ok, "fsyncLock command succeeded against DB other than admin.");

    // Uses admin automatically and locks the server for writes.
    var fsyncLockRes = db.fsyncLock();
    assert(fsyncLockRes.ok, "fsyncLock command failed against admin DB");
    assert(db.currentOp().fsyncLock, "Value in db.currentOp incorrect for fsyncLocked server");

    // Make sure writes are blocked. Spawn a write operation in a separate shell and make sure it
    // is blocked. There is really no way to do that currently, so just check that the write didn't
    // go through.
    var writeOpHandle = startParallelShell("db.getSisterDB('fsyncLockTestDB').coll.insert({x:1});");
    sleep(3000);

    // Make sure reads can still run even though there is a pending write and also that the write
    // didn't get through.
    assert.eq(1, fsyncLockDB.coll.find({}).itcount());

    // Unlock and make sure the insert succeeded.
    var fsyncUnlockRes = db.fsyncUnlock();
    assert(fsyncUnlockRes.ok, "fsyncUnlock command failed");
    assert(db.currentOp().fsyncLock == null, "fsyncUnlock is not null in db.currentOp");

    // Make sure the db is unlocked and the initial write made it through.
    writeOpHandle();
    assert.writeOK(fsyncLockDB.coll.insert({x: 2}));

    assert.eq(3, fsyncLockDB.coll.count({}));

    // Issue the fsyncLock and fsyncUnlock a second time, to ensure that we can
    // run this command repeatedly with no problems.
    var fsyncLockRes = db.fsyncLock();
    assert(fsyncLockRes.ok, "Second execution of fsyncLock command failed");

    var fsyncUnlockRes = db.fsyncUnlock();
    assert(fsyncUnlockRes.ok, "Second execution of fsyncUnlock command failed");

    // Ensure eval is not allowed to invoke fsyncLock
    assert(!db.eval('db.fsyncLock()').ok, "eval('db.fsyncLock()') should fail.");

    // Check that the fsyncUnlock pseudo-command (a lookup on cmd.$sys.unlock)
    // still has the same effect as a legitimate 'fsyncUnlock' command.
    // TODO: remove this in in the release following MongoDB 3.2 when pseudo-commands
    // are removed.
    var fsyncCommandRes = db.fsyncLock();
    assert(fsyncLockRes.ok, "fsyncLock command failed against admin DB");
    assert(db.currentOp().fsyncLock, "Value in db.currentOp incorrect for fsyncLocked server");
    var fsyncPseudoCommandRes = db.getSiblingDB("admin").$cmd.sys.unlock.findOne();
    assert(fsyncPseudoCommandRes.ok, "fsyncUnlock pseudo-command failed");
    assert(db.currentOp().fsyncLock == null, "fsyncUnlock is not null in db.currentOp");

    // Make sure that insert attempts made during multiple fsyncLock requests will not execute until
    // all locks have been released.
    fsyncLockRes = db.fsyncLock();
    assert.commandWorked(fsyncLockRes);
    assert(fsyncLockRes.lockCount == 1, tojson(fsyncLockRes));
    let currentOp = db.currentOp();
    assert.commandWorked(currentOp);
    assert(currentOp.fsyncLock, "Value in db.currentOp incorrect for fsyncLocked server");

    let shellHandle1 =
        startParallelShell("db.getSisterDB('fsyncLockTestDB').multipleLock.insert({x:1});");

    fsyncLockRes = db.fsyncLock();
    assert.commandWorked(fsyncLockRes);
    assert(fsyncLockRes.lockCount == 2, tojson(fsyncLockRes));
    currentOp = db.currentOp();
    assert.commandWorked(currentOp);
    assert(currentOp.fsyncLock, "Value in db.currentOp incorrect for fsyncLocked server");

    let shellHandle2 =
        startParallelShell("db.getSisterDB('fsyncLockTestDB').multipleLock.insert({x:1});");
    sleep(3000);
    assert.eq(0, fsyncLockDB.multipleLock.find({}).itcount());

    fsyncUnlockRes = db.fsyncUnlock();
    assert.commandWorked(fsyncUnlockRes);
    assert(fsyncUnlockRes.lockCount == 1, tojson(fsyncLockRes));
    sleep(3000);
    assert.eq(0, fsyncLockDB.multipleLock.find({}).itcount());

    fsyncUnlockRes = db.fsyncUnlock();
    assert.commandWorked(fsyncUnlockRes);
    assert(fsyncUnlockRes.lockCount == 0, tojson(fsyncLockRes));
    shellHandle1();
    shellHandle2();
    assert.eq(2, fsyncLockDB.multipleLock.find({}).itcount());
}());

/**
 * Test fsyncLock functionality
 * - Skip for all storage engines which don't support fsync
 * - Run the fsyncLock command, confirm we lock correctly with currentOp
 * - Confirm that we cannot insert during fsyncLock
 * - Confirm that writes can progress after fsyncUnlock
 * - Confirm that the command can be run repeatedly without breaking things
 *
 * @tags: [
 *   # The test runs commands that are not allowed with security token: fsync, fsyncUnlock.
 *   not_allowed_with_signed_security_token,
 *   requires_fastcount,
 *   requires_fsync,
 *   uses_parallel_shell,
 * ]
 */
function waitUntilOpCountIs(opFilter, num) {
    assert.soon(() => {
        let ops = db
            .getSiblingDB("admin")
            .aggregate([{$currentOp: {allUsers: true, idleConnections: true}}, {$match: opFilter}])
            .toArray();
        if (ops.length != num) {
            jsTest.log("Num opeartions: " + ops.length + ", expected: " + num);
            jsTest.log(ops);
            return false;
        }
        return true;
    });
}

// Start with a clean DB.
var fsyncLockDB = db.getSiblingDB("fsyncLockTestDB");
fsyncLockDB.dropDatabase();

// Tests the db.fsyncLock/fsyncUnlock features.
let storageEngine = db.serverStatus().storageEngine.name;

// As of SERVER-18899 fsyncLock/fsyncUnlock will error when called on a storage engine
// that does not support the begin/end backup commands.
let supportsFsync = db.fsyncLock();

if (!supportsFsync.ok) {
    assert.commandFailedWithCode(supportsFsync, ErrorCodes.CommandNotSupported);
    jsTestLog("Skipping test for " + storageEngine + " as it does not support fsync");
    quit();
}
db.fsyncUnlock();

var resFail = fsyncLockDB.runCommand({fsync: 1, lock: 1});

// Start with a clean DB
var fsyncLockDB = db.getSiblingDB("fsyncLockTestDB");
fsyncLockDB.dropDatabase();

// Test that a single, regular write works as expected.
assert.commandWorked(fsyncLockDB.coll.insert({x: 1}));

// Test that fsyncLock doesn't work unless invoked against the admin DB.
var resFail = fsyncLockDB.runCommand({fsync: 1, lock: 1});
assert(!resFail.ok, "fsyncLock command succeeded against DB other than admin.");

// Ensure that fsync (and fsyncLock) are strict, see HELP-58426
assert.commandFailed(db.adminCommand({fsync: 1, unlock: true}));
assert.commandFailed(db.adminCommand({fsync: 1, unlock: false}));
assert.commandFailed(db.adminCommand({fsync: 1, lock: "not_valid_boolean"}));

// Uses admin automatically and locks the server for writes.
var fsyncLockRes = db.fsyncLock();
assert(fsyncLockRes.ok, "fsyncLock command failed against admin DB");
assert(
    db.getSiblingDB("admin").runCommand({currentOp: 1}).fsyncLock,
    "Value in currentOp result incorrect for fsyncLocked server",
);

// Make sure writes are blocked. Spawn a write operation in a separate shell and make sure it
// is blocked. There is really no way to do that currently, so just check that the write didn't
// go through.
let writeOpHandle = startParallelShell("db.getSiblingDB('fsyncLockTestDB').coll.insert({x:1});");
waitUntilOpCountIs({op: "insert", ns: "fsyncLockTestDB.coll", waitingForLock: true}, 1);

// Make sure reads can still run even though there is a pending write and also that the write
// didn't get through.
assert.eq(1, fsyncLockDB.coll.find({}).itcount());

// Unlock and make sure the insert succeeded.
var fsyncUnlockRes = db.fsyncUnlock();
assert(fsyncUnlockRes.ok, "fsyncUnlock command failed");
assert(
    db.getSiblingDB("admin").runCommand({currentOp: 1}).fsyncLock == null,
    "fsyncUnlock is not null in currentOp result",
);

// Make sure the db is unlocked and the initial write made it through.
writeOpHandle();
assert.commandWorked(fsyncLockDB.coll.insert({x: 2}));

assert.eq(3, fsyncLockDB.coll.count({}));

// Issue the fsyncLock and fsyncUnlock a second time, to ensure that we can
// run this command repeatedly with no problems.
var fsyncLockRes = db.fsyncLock();
assert(fsyncLockRes.ok, "Second execution of fsyncLock command failed");

var fsyncUnlockRes = db.fsyncUnlock();
assert(fsyncUnlockRes.ok, "Second execution of fsyncUnlock command failed");

// Make sure that insert attempts made during multiple fsyncLock requests will not execute until
// all locks have been released.
fsyncLockRes = db.fsyncLock();
assert.commandWorked(fsyncLockRes);
assert(fsyncLockRes.lockCount == 1, tojson(fsyncLockRes));
let currentOp = db.getSiblingDB("admin").runCommand({currentOp: 1});
assert.commandWorked(currentOp);
assert(currentOp.fsyncLock, "Value in currentOp result incorrect for fsyncLocked server");

let shellHandle1 = startParallelShell("db.getSiblingDB('fsyncLockTestDB').multipleLock.insert({x:1});");

fsyncLockRes = db.fsyncLock();
assert.commandWorked(fsyncLockRes);
assert(fsyncLockRes.lockCount == 2, tojson(fsyncLockRes));
currentOp = db.getSiblingDB("admin").runCommand({currentOp: 1});
assert.commandWorked(currentOp);
assert(currentOp.fsyncLock, "Value in currentOp result incorrect for fsyncLocked server");

let shellHandle2 = startParallelShell("db.getSiblingDB('fsyncLockTestDB').multipleLock.insert({x:1});");
waitUntilOpCountIs({op: "insert", ns: "fsyncLockTestDB.multipleLock", waitingForLock: true}, 2);

assert.eq(0, fsyncLockDB.multipleLock.find({}).itcount());

fsyncUnlockRes = db.fsyncUnlock();
assert.commandWorked(fsyncUnlockRes);
assert(fsyncUnlockRes.lockCount == 1, tojson(fsyncLockRes));
sleep(1000);
assert.eq(0, fsyncLockDB.multipleLock.find({}).itcount());

fsyncUnlockRes = db.fsyncUnlock();
assert.commandWorked(fsyncUnlockRes);
assert(fsyncUnlockRes.lockCount == 0, tojson(fsyncLockRes));
shellHandle1();
shellHandle2();
assert.eq(2, fsyncLockDB.multipleLock.find({}).itcount());

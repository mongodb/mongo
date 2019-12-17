/*
 * This test checks that we can recover from an operation that has taken out a write lock for a long
 * time (potentially deadlocking the server).
 *
 * This comes out of a number of BFs related to jstests/core/mr_killop.js in the parallel suite (see
 * BF-6259 for the full write-up).
 *
 * @tags: [requires_replication]
 */
(function() {
'use strict';

load("jstests/libs/wait_for_command.js");

TestData.enableTestCommands = true;

// Start a mongod with the user cache size set to zero, so we know that users who have logged out
// always get fetched cleanly from disk.
const rst = new ReplSetTest({
    nodes: 3,
    nodeOptions: {auth: "", setParameter: "authorizationManagerCacheSize=0"},
    keyFile: "jstests/libs/key1"
});

rst.startSet();
rst.initiate();

const mongod = rst.getPrimary();
const admin = mongod.getDB("admin");

admin.createUser({user: "admin", pwd: "admin", roles: ["root"]});
admin.auth("admin", "admin");

// Mark the "admin2" user as pinned in memory, we'll use this later on to recover from the deadlock
assert.commandWorked(admin.runCommand({
    setParameter: 1,
    logLevel: 2,
    authorizationManagerPinnedUsers: [
        {user: "admin2", db: "admin"},
    ],
}));

admin.createUser({user: "admin2", pwd: "admin", roles: ["root"]});

let secondConn = new Mongo(mongod.host);
let secondAdmin = secondConn.getDB("admin");
secondAdmin.auth("admin2", "admin");

// Invalidate the user cache, but ensure that eventually admin2 will show up there because it's
// pinned
assert.commandWorked(admin.runCommand({invalidateUserCache: 1}));
assert.soon(function() {
    let cacheContents = admin.aggregate([{$listCachedAndActiveUsers: {}}]).toArray();
    print("User cache after initialization: ", tojson(cacheContents));

    const admin2Doc = sortDoc({"username": "admin2", "db": "admin", "active": true});
    return cacheContents.some((doc) => friendlyEqual(admin2Doc, sortDoc(doc)));
});

// The deadlock happens in two phases. First we run a command that acquires a read lock and holds it
// forever
let readLockShell = startParallelShell(function() {
    assert.eq(db.getSiblingDB("admin").auth("admin", "admin"), 1);
    assert.commandFailed(db.adminCommand(
        {sleep: 1, secs: 500, lock: "r", lockTarget: "admin", $comment: "Read lock sleep"}));
}, mongod.port);

// Wait for that command to appear in currentOp
const readID = waitForCommand(
    "readlock",
    op => (op["ns"] == "admin.$cmd" && op["command"]["$comment"] == "Read lock sleep"),
    admin);

// Then we run a command that tries to acquire a write lock, which will wait forever because we're
// already holding a read lock, but will also prevent any new read locks from being taken
let writeLockShell = startParallelShell(function() {
    assert.eq(db.getSiblingDB("admin").auth("admin", "admin"), 1);
    assert.commandFailed(db.adminCommand(
        {sleep: 1, secs: 500, lock: "w", lockTarget: "admin", $comment: "Write lock sleep"}));
}, mongod.port);

// Wait for that command to appear in currentOp
const writeID = waitForCommand(
    "writeLock",
    op => (op["ns"] == "admin.$cmd" && op["command"]["$comment"] == "Write lock sleep"),
    admin);

print("killing ops and moving on!");

// If "admin2" wasn't pinned in memory, then these would hang.
assert.commandWorked(secondAdmin.currentOp());
assert.commandWorked(secondAdmin.killOp(readID));
assert.commandWorked(secondAdmin.killOp(writeID));

readLockShell();
writeLockShell();

admin.logout();
secondAdmin.logout();

rst.stopSet();
})();

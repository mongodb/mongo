/*
 * This test checks that we can recover from an operation that has taken out a write lock
 * for a long time (potentially deadlocking the server). This comes out of a number of BF's
 * related to jstests/core/mr_killop.js in the parallel suite (see BF-6259 for the full
 * write-up).
 *
 * @tags: [requires_replication]
 *
 */
(function() {
'use strict';
jsTest.setOption("enableTestCommands", true);
// Start a mongod with the user cache size set to zero, so we know that users who have
// logged out always get fetched cleanly from disk.
const rs = new ReplSetTest({
    nodes: 3,
    nodeOptions: {auth: "", setParameter: "authorizationManagerCacheSize=0"},
    keyFile: "jstests/libs/key1"
});

rs.startSet();
rs.initiate();
const mongod = rs.getPrimary();
const admin = mongod.getDB("admin");

admin.createUser({user: "admin", pwd: "admin", roles: ["root"]});
admin.auth("admin", "admin");

// Mark the "admin2" user as pinned in memory, we'll use this later on to recover from
// the deadlock
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

// Invalidate the user cache so we know only "admin" is in there
assert.commandWorked(admin.runCommand({invalidateUserCache: 1}));
assert.soon(function() {
    let cacheContents = admin.aggregate([{$listCachedAndActiveUsers: {}}]).toArray();
    print("User cache after initialization: ", tojson(cacheContents));

    const admin2Doc = sortDoc({"username": "admin2", "db": "admin", "active": true});
    return cacheContents.some((doc) => friendlyEqual(admin2Doc, sortDoc(doc)));
});

const waitForCommand = function(waitingFor, opFilter) {
    let opId = -1;
    assert.soon(function() {
        print(`Checking for ${waitingFor}`);
        const curopRes = admin.currentOp();
        assert.commandWorked(curopRes);
        const foundOp = curopRes["inprog"].filter(opFilter);

        if (foundOp.length == 1) {
            opId = foundOp[0]["opid"];
        }
        return (foundOp.length == 1);
    });
    return opId;
};

// The deadlock happens in two phases. First we run a command that acquires a read lock and
// holds it for forever.
let readLockShell = startParallelShell(function() {
    assert.eq(db.getSiblingDB("admin").auth("admin", "admin"), 1);
    assert.commandFailed(db.adminCommand(
        {sleep: 1, secs: 500, lock: "r", lockTarget: "admin", $comment: "Read lock sleep"}));
}, mongod.port);

// Wait for that command to appear in currentOp
const readID = waitForCommand(
    "readlock", op => (op["ns"] == "admin.$cmd" && op["command"]["$comment"] == "Read lock sleep"));

// Then we run a command that tries to acquire a write lock, which will wait for forever
// because we're already holding a read lock, but will also prevent any new read locks from
// being taken.
let writeLockShell = startParallelShell(function() {
    assert.eq(db.getSiblingDB("admin").auth("admin", "admin"), 1);
    assert.commandFailed(db.adminCommand(
        {sleep: 1, secs: 500, lock: "w", lockTarget: "admin", $comment: "Write lock sleep"}));
}, mongod.port);

// Wait for that to appear in currentOp
const writeID = waitForCommand(
    "writeLock",
    op => (op["ns"] == "admin.$cmd" && op["command"]["$comment"] == "Write lock sleep"));

print("killing ops and moving on!");

// If "admin2" wasn't pinned in memory, then these would hang.
assert.commandWorked(secondAdmin.currentOp());
assert.commandWorked(secondAdmin.killOp(readID));
assert.commandWorked(secondAdmin.killOp(writeID));

readLockShell();
writeLockShell();

admin.logout();
secondAdmin.logout();
rs.stopSet();
})();

// This checks that removing a user document actually unpins a user. This is a round-about way
// of making sure that updates to the authz manager by the opObserver correctly invalidates the
// cache and that pinned users don't stick around after they're removed.
(function() {
'use strict';
jsTest.setOption("enableTestCommands", true);
// Start a mongod with the user cache size set to zero, so we know that users who have
// logged out always get fetched cleanly from disk.
const mongod = MongoRunner.runMongod({auth: "", setParameter: "authorizationManagerCacheSize=0"});
let admin = mongod.getDB("admin");

admin.createUser({user: "admin", pwd: "admin", roles: ["root"]});
admin.auth("admin", "admin");

// Mark the "admin2" user as pinned in memory
assert.commandWorked(admin.runCommand({
    setParameter: 1,
    logLevel: 2,
    authorizationManagerPinnedUsers: [
        {user: "admin2", db: "admin"},
    ],
}));

admin.createUser({user: "admin2", pwd: "admin", roles: ["root"]});

// Invalidate the user cache so we know only "admin" is in there
assert.commandWorked(admin.runCommand({invalidateUserCache: 1}));
print("User cache after initialization: ",
      tojson(admin.aggregate([{$listCachedAndActiveUsers: {}}]).toArray()));

assert.commandWorked(admin.getCollection("system.users").remove({user: "admin2"}));

print("User cache after removing user doc: ",
      tojson(admin.aggregate([{$listCachedAndActiveUsers: {}}]).toArray()));

assert.eq(admin.auth("admin2", "admin"), 0);
MongoRunner.stopMongod(mongod);
})();

// This checks that clearing the pinned user list actually unpins a user.
(function() {
'use strict';
jsTest.setOption("enableTestCommands", true);
// Start a mongod with the user cache size set to zero, so we know that users who have
// logged out always get fetched cleanly from disk.
const mongod = MongoRunner.runMongod({auth: "", setParameter: "authorizationManagerCacheSize=0"});
let admin = mongod.getDB("admin");

admin.createUser({user: "admin", pwd: "admin", roles: ["root"]});
admin.auth("admin", "admin");

// Mark the "admin2" user as pinned in memory
assert.commandWorked(admin.runCommand({
    setParameter: 1,
    logLevel: 2,
    authorizationManagerPinnedUsers: [
        {user: "admin2", db: "admin"},
    ],
}));

admin.createUser({user: "admin2", pwd: "admin", roles: ["root"]});
assert.soon(function() {
    let cacheContents = admin.aggregate([{$listCachedAndActiveUsers: {}}]).toArray();
    print("User cache after initialization: ", tojson(cacheContents));

    const admin2Doc = sortDoc({"username": "admin2", "db": "admin", "active": true});
    return cacheContents.some((doc) => friendlyEqual(admin2Doc, sortDoc(doc)));
});

// Clear the pinned users list
assert.commandWorked(admin.runCommand({setParameter: 1, authorizationManagerPinnedUsers: []}));

// Check that admin2 gets removed from the cache
assert.commandWorked(admin.runCommand({invalidateUserCache: 1}));
assert.soon(function() {
    let cacheContents = admin.aggregate([{$listCachedAndActiveUsers: {}}]).toArray();
    print("User cache after initialization: ", tojson(cacheContents));

    const admin2Doc = sortDoc({"username": "admin2", "db": "admin", "active": true});
    return !cacheContents.some((doc) => friendlyEqual(admin2Doc, sortDoc(doc)));
});

MongoRunner.stopMongod(mongod);
})();

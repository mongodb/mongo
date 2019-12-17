/**
 * This test checks that removing a user document actually unpins a user. This is a round-about way
 * of making sure that updates to the authz manager by the opObserver correctly invalidates the
 * cache and that pinned users don't stick around after they're removed.
 */
(function() {
'use strict';

TestData.enableTestCommands = true;

// Start a mongod with the user cache size set to zero, so we know that users who have logged out
// always get fetched cleanly from disk.
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

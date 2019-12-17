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

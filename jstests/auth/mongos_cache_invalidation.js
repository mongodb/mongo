/**
 * This tests that updates to user and role definitions made on one mongos propagate properly
 * to other mongoses.
 */

var authzErrorCode = 13;
var hasAuthzError = function(result) {
    assert(result.hasWriteError());
    assert.eq(authzErrorCode, result.getWriteError().code);
};

var st = new ShardingTest({
    shards: 2,
    config: 3,
    mongos: [
        {},
        {setParameter: "userCacheInvalidationIntervalSecs=5"},
        {setParameter: "userCacheInvalidationIntervalSecs=600"}
    ],
    keyFile: 'jstests/libs/key1'
});

st.s1.getDB('admin').createUser({user: 'root', pwd: 'pwd', roles: ['root']});
st.s1.getDB('admin').auth('root', 'pwd');

var res = st.s1.getDB('admin').runCommand({setParameter: 1, userCacheInvalidationIntervalSecs: 0});
assert.commandFailed(res, "Setting the invalidation interval to an disallowed value should fail");

res = st.s1.getDB('admin').runCommand({setParameter: 1, userCacheInvalidationIntervalSecs: 100000});
assert.commandFailed(res, "Setting the invalidation interval to an disallowed value should fail");

res = st.s1.getDB('admin').runCommand({getParameter: 1, userCacheInvalidationIntervalSecs: 1});

assert.eq(5, res.userCacheInvalidationIntervalSecs);
assert.writeOK(st.s1.getDB('test').foo.insert({a: 1}));  // initial data
assert.writeOK(st.s1.getDB('test').bar.insert({a: 1}));  // initial data
st.s1.getDB('admin').createUser({user: 'admin', pwd: 'pwd', roles: ['userAdminAnyDatabase']});
st.s1.getDB('admin').logout();

st.s0.getDB('admin').auth('admin', 'pwd');
st.s0.getDB('admin').createRole({
    role: 'myRole',
    roles: [],
    privileges: [{resource: {cluster: true}, actions: ['invalidateUserCache', 'setParameter']}]
});
st.s0.getDB('test').createUser({
    user: 'spencer',
    pwd: 'pwd',
    roles: ['read', {role: 'myRole', db: 'admin'}, {role: 'userAdminAnyDatabase', db: 'admin'}]
});
st.s0.getDB('admin').logout();

var db1 = st.s0.getDB('test');
db1.auth('spencer', 'pwd');
var db2 = st.s1.getDB('test');
db2.auth('spencer', 'pwd');
var db3 = st.s2.getDB('test');
db3.auth('spencer', 'pwd');

/**
 * At this point we have 3 handles to the "test" database, each of which are on connections to
 * different mongoses.  "db1", "db2", and "db3" are all auth'd as spencer@test and will be used
 * to verify that user and role data changes get propaged to their mongoses.
 * "db2" is connected to a mongos with a 5 second user cache invalidation interval,
 * while "db3" is connected to a mongos with a 10 minute cache invalidation interval.
 */

(function testChangingInvalidationInterval() {
    jsTestLog("Test that changing the invalidation interval takes effect immediately");

    assert.commandFailedWithCode(db3.bar.runCommand("drop"), authzErrorCode);
    assert.eq(1, db3.bar.findOne().a);

    db1.getSiblingDB('admin').grantPrivilegesToRole(
        "myRole", [{resource: {db: 'test', collection: ''}, actions: ['dropCollection']}]);

    // At first db3 should still think we're unauthorized because it hasn't invalidated it's cache.
    assert.commandFailedWithCode(db3.bar.runCommand('drop'), authzErrorCode);
    // Changing the value of the invalidation interval should make it invalidate its cache quickly.
    assert.commandWorked(db3.adminCommand({setParameter: 1, userCacheInvalidationIntervalSecs: 1}));
    sleep(2000);
    assert.commandWorked(db3.bar.runCommand('drop'));
    assert.eq(0, db3.bar.count());

    // Set the invalidation interval back for the rest of the tests
    db3.adminCommand({setParameter: 1, userCacheInvalidationIntervalSecs: 600});
})();

(function testGrantingPrivileges() {
    jsTestLog("Testing propagation of granting privileges");

    hasAuthzError(db1.foo.update({}, {$inc: {a: 1}}));
    hasAuthzError(db2.foo.update({}, {$inc: {a: 1}}));
    hasAuthzError(db3.foo.update({}, {$inc: {a: 1}}));

    assert.eq(1, db1.foo.findOne().a);
    assert.eq(1, db2.foo.findOne().a);
    assert.eq(1, db3.foo.findOne().a);

    db1.getSiblingDB('admin').grantPrivilegesToRole(
        "myRole", [{resource: {db: 'test', collection: ''}, actions: ['update']}]);

    // s0/db1 should update its cache instantly
    assert.writeOK(db1.foo.update({}, {$inc: {a: 1}}));
    assert.eq(2, db1.foo.findOne().a);

    // s1/db2 should update its cache in 10 seconds.
    assert.soon(function() {
        var res = db2.foo.update({}, {$inc: {a: 1}});
        if (res.hasWriteError()) {
            return false;
        }
        return db2.foo.findOne().a == 3;
    }, "Mongos did not update its user cache after 10 seconds", 10 * 1000);

    // We manually invalidate the cache on s2/db3.
    db3.adminCommand("invalidateUserCache");
    assert.writeOK(db3.foo.update({}, {$inc: {a: 1}}));
    assert.eq(4, db3.foo.findOne().a);

})();

(function testRevokingPrivileges() {
    jsTestLog("Testing propagation of revoking privileges");

    db1.getSiblingDB('admin').revokePrivilegesFromRole(
        "myRole", [{resource: {db: 'test', collection: ''}, actions: ['update']}]);

    // s0/db1 should update its cache instantly
    hasAuthzError(db1.foo.update({}, {$inc: {a: 1}}));

    jsTest.log("Beginning wait for s1/db2 cache update.");
    // s1/db2 should update its cache in 10 seconds.
    assert.soon(function() {
        var res = db2.foo.update({}, {$inc: {a: 1}});
        return res.hasWriteError() && res.getWriteError().code == authzErrorCode;
    }, "Mongos did not update its user cache after 10 seconds", 10 * 1000);

    // We manually invalidate the cache on s1/db3.
    db3.adminCommand("invalidateUserCache");
    hasAuthzError(db3.foo.update({}, {$inc: {a: 1}}));
})();

(function testModifyingUser() {
    jsTestLog("Testing propagation modifications to a user, rather than to a role");

    hasAuthzError(db1.foo.update({}, {$inc: {a: 1}}));
    hasAuthzError(db2.foo.update({}, {$inc: {a: 1}}));
    hasAuthzError(db3.foo.update({}, {$inc: {a: 1}}));

    db1.getSiblingDB('test').grantRolesToUser("spencer", ['readWrite']);

    // s0/db1 should update its cache instantly
    assert.writeOK(db1.foo.update({}, {$inc: {a: 1}}));

    // s1/db2 should update its cache in 5 seconds.
    assert.soon(
        function() {
            return !db2.foo.update({}, {$inc: {a: 1}}).hasWriteError();
        },
        "Mongos did not update its user cache after 5 seconds",
        6 * 1000);  // Give an extra 1 second to avoid races

    // We manually invalidate the cache on s1/db3.
    db3.adminCommand("invalidateUserCache");
    assert.writeOK(db3.foo.update({}, {$inc: {a: 1}}));
})();

(function testConcurrentUserModification() {
    jsTestLog("Testing having 2 mongoses modify the same user at the same time");  // SERVER-13850

    assert.writeOK(db1.foo.update({}, {$inc: {a: 1}}));
    assert.writeOK(db3.foo.update({}, {$inc: {a: 1}}));

    db1.getSiblingDB('test').revokeRolesFromUser("spencer", ['readWrite']);

    // At this point db3 still thinks "spencer" has readWrite.  Use it to add a different role
    // and make sure it doesn't add back readWrite
    hasAuthzError(db1.foo.update({}, {$inc: {a: 1}}));
    assert.writeOK(db3.foo.update({}, {$inc: {a: 1}}));

    db3.getSiblingDB('test').grantRolesToUser("spencer", ['dbAdmin']);

    hasAuthzError(db1.foo.update({}, {$inc: {a: 1}}));
    // modifying "spencer" should force db3 to update its cache entry for "spencer"
    hasAuthzError(db3.foo.update({}, {$inc: {a: 1}}));

    // Make sure nothing changes from invalidating the cache
    db1.adminCommand('invalidateUserCache');
    db3.adminCommand('invalidateUserCache');
    hasAuthzError(db1.foo.update({}, {$inc: {a: 1}}));
    hasAuthzError(db3.foo.update({}, {$inc: {a: 1}}));
})();

(function testDroppingUser() {
    jsTestLog("Testing propagation of dropping users");

    assert.commandWorked(db1.foo.runCommand("collStats"));
    assert.commandWorked(db2.foo.runCommand("collStats"));
    assert.commandWorked(db3.foo.runCommand("collStats"));

    db1.dropUser('spencer');

    // s0/db1 should update its cache instantly
    assert.commandFailedWithCode(db1.foo.runCommand("collStats"), authzErrorCode);

    // s1/db2 should update its cache in 5 seconds.
    assert.soon(
        function() {
            return db2.foo.runCommand("collStats").code == authzErrorCode;
        },
        "Mongos did not update its user cache after 5 seconds",
        6 * 1000);  // Give an extra 1 second to avoid races

    // We manually invalidate the cache on s2/db3.
    db3.adminCommand("invalidateUserCache");
    assert.commandFailedWithCode(db3.foo.runCommand("collStats"), authzErrorCode);

})();

st.stop();

print("SUCCESS Completed mongos_cache_invalidation.js");

/**
 * This tests that updates to user and role definitions made on one mongos propagate properly
 * to other mongoses.
 * @tags: [requires_sharding]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";

let authzErrorCode = 13;
let hasAuthzError = function (result) {
    assert(result instanceof WriteCommandError);
    assert.eq(authzErrorCode, result.code);
};

let st = new ShardingTest({
    shards: 2,
    config: 3,
    mongos: [
        {},
        {setParameter: {userCacheInvalidationIntervalSecs: 5}},
        {setParameter: {userCacheInvalidationIntervalSecs: 600}},
    ],
    keyFile: "jstests/libs/key1",
});

st.s1.getDB("admin").createUser({user: "root", pwd: "pwd", roles: ["__system"]});
st.s1.getDB("admin").auth("root", "pwd");

let res = st.s1.getDB("admin").runCommand({setParameter: 1, userCacheInvalidationIntervalSecs: 0});
assert.commandFailed(res, "Setting the invalidation interval to an disallowed value should fail");

res = st.s1.getDB("admin").runCommand({setParameter: 1, userCacheInvalidationIntervalSecs: 100000});
assert.commandFailed(res, "Setting the invalidation interval to an disallowed value should fail");

res = st.s1.getDB("admin").runCommand({getParameter: 1, userCacheInvalidationIntervalSecs: 1});

assert.eq(5, res.userCacheInvalidationIntervalSecs);
assert.commandWorked(st.s1.getDB("test").foo.insert({a: 1})); // initial data
assert.commandWorked(st.s1.getDB("test").bar.insert({a: 1})); // initial data
st.s1.getDB("admin").createUser({user: "admin", pwd: "pwd", roles: ["userAdminAnyDatabase"]});
st.s1.getDB("admin").logout();

st.s0.getDB("admin").auth("admin", "pwd");
st.s0.getDB("admin").createRole({
    role: "myRole",
    roles: [],
    privileges: [
        {
            resource: {cluster: true},
            actions: ["invalidateUserCache", "getParameter", "setParameter", "getLog", "dbStats"],
        },
    ],
});
st.s0.getDB("test").createUser({
    user: "spencer",
    pwd: "pwd",
    roles: ["read", {role: "myRole", db: "admin"}, {role: "userAdminAnyDatabase", db: "admin"}],
});
st.s0.getDB("admin").logout();

const db1 = st.s0.getDB("test");
assert(db1.auth("spencer", "pwd"));
const db2 = st.s1.getDB("test");
assert(db2.auth("spencer", "pwd"));
const db3 = st.s2.getDB("test");
assert(db3.auth("spencer", "pwd"));

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

    db1.getSiblingDB("admin").grantPrivilegesToRole("myRole", [
        {resource: {db: "test", collection: ""}, actions: ["dropCollection"]},
    ]);

    // At first db3 should still think we're unauthorized because it hasn't invalidated it's cache.
    assert.commandFailedWithCode(db3.bar.runCommand("drop"), authzErrorCode);
    // Changing the value of the invalidation interval should make it invalidate its cache quickly.
    assert.commandWorked(db3.adminCommand({setParameter: 1, userCacheInvalidationIntervalSecs: 1}));
    sleep(2000);
    assert.commandWorked(db3.bar.runCommand("drop"));
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

    db1.getSiblingDB("admin").grantPrivilegesToRole("myRole", [
        {resource: {db: "test", collection: ""}, actions: ["update"]},
    ]);

    // s0/db1 should update its cache instantly
    assert.commandWorked(db1.foo.update({}, {$inc: {a: 1}}));
    assert.eq(2, db1.foo.findOne().a);

    // s1/db2 should update its cache in 10 seconds.
    sleep(10000);
    assert.soon(
        function () {
            let res = db2.foo.update({}, {$inc: {a: 1}});
            if (res instanceof WriteCommandError) {
                return false;
            }
            return db2.foo.findOne().a == 3;
        },
        "Mongos did not update its user cache after 10 seconds",
        5 * 1000 /* Additional 5 seconds of buffer in case of slow update */,
    );

    // We manually invalidate the cache on s2/db3.
    db3.adminCommand("invalidateUserCache");
    assert.commandWorked(db3.foo.update({}, {$inc: {a: 1}}));
    assert.eq(4, db3.foo.findOne().a);
})();

(function testRevokingPrivileges() {
    jsTestLog("Testing propagation of revoking privileges");

    db1.getSiblingDB("admin").revokePrivilegesFromRole("myRole", [
        {resource: {db: "test", collection: ""}, actions: ["update"]},
    ]);

    // s0/db1 should update its cache instantly
    hasAuthzError(db1.foo.update({}, {$inc: {a: 1}}));

    jsTest.log("Beginning wait for s1/db2 cache update.");
    // s1/db2 should update its cache in 10 seconds.
    sleep(10000);
    assert.soon(
        function () {
            let res = db2.foo.update({}, {$inc: {a: 1}});
            return res instanceof WriteCommandError && res.code == authzErrorCode;
        },
        "Mongos did not update its user cache after 10 seconds",
        5 * 1000 /* Additional 5 seconds of buffer in case of slow update */,
    );

    // We manually invalidate the cache on s1/db3.
    db3.adminCommand("invalidateUserCache");
    hasAuthzError(db3.foo.update({}, {$inc: {a: 1}}));
})();

/**
 * Test we do not flush the cache and generate a new userCacheGeneration if there are no user
 * changes.
 */
(function testKeepingCacheIfNoChange() {
    jsTestLog("Testing cache generation stays the same and the user cache is not flushed if there are no changes");

    const cacheIntervalBeforeTest = assert.commandWorked(
        db1.adminCommand({getParameter: 1, userCacheInvalidationIntervalSecs: 1}),
    ).userCacheInvalidationIntervalSecs;

    // Set the invalidation interval for 5 seconds
    assert.commandWorked(db1.adminCommand({setParameter: 1, userCacheInvalidationIntervalSecs: 5}));

    // Get current cache generation
    const cfg1 = st.configRS.getPrimary().getDB("admin");
    assert(cfg1.auth("root", "pwd"));
    const genBeforeChange = assert.commandWorked(cfg1.runCommand({_getUserCacheGeneration: 1}));

    // Update user role so cacheGeneration gets updated and grab current time
    let currentTime = Date.now();
    db1.getSiblingDB("admin").revokePrivilegesFromRole("myRole", [
        {resource: {db: "test", collection: ""}, actions: ["dbStats"]},
    ]);

    // Look for cache generation change that happened after currentTime
    let genAfterChange;
    assert.soon(() => {
        genAfterChange = assert.commandWorked(cfg1.runCommand({_getUserCacheGeneration: 1}));
        return genBeforeChange.cacheGeneration != genAfterChange.cacheGeneration;
    });

    // Set userCacheInvalidationIntervalSecs to 1 second and check current cache generation does not
    // change after no user changes
    currentTime = Date.now();
    assert.commandWorked(db1.adminCommand({setParameter: 1, userCacheInvalidationIntervalSecs: 1}));

    // Sleep 3 seconds to give enough time for the userCacheInvalidator job to run
    sleep(3000);

    const genAfterNoChange = assert.commandWorked(cfg1.runCommand({_getUserCacheGeneration: 1}));
    assert.eq(
        genAfterChange.cacheGeneration,
        genAfterNoChange.cacheGeneration,
        "User cache generation changed after no user change.",
    );

    cfg1.logout();

    // Set userCacheInvalidationInterval back to value before the test
    assert.commandWorked(
        db1.adminCommand({setParameter: 1, userCacheInvalidationIntervalSecs: cacheIntervalBeforeTest}),
    );
})();

(function testModifyingUser() {
    jsTestLog("Testing propagation modifications to a user, rather than to a role");

    hasAuthzError(db1.foo.update({}, {$inc: {a: 1}}));
    hasAuthzError(db2.foo.update({}, {$inc: {a: 1}}));
    hasAuthzError(db3.foo.update({}, {$inc: {a: 1}}));

    db1.getSiblingDB("test").grantRolesToUser("spencer", ["readWrite"]);

    // s0/db1 should update its cache instantly
    assert.commandWorked(db1.foo.update({}, {$inc: {a: 1}}));

    // s1/db2 should update its cache in 10 seconds.
    sleep(10000);
    assert.soon(
        function () {
            return !(db2.foo.update({}, {$inc: {a: 1}}) instanceof WriteCommandError);
        },
        "Mongos did not update its user cache after 10 seconds",
        5 * 1000 /* Additional 5 seconds of buffer in case of slow update */,
    );

    // We manually invalidate the cache on s1/db3.
    db3.adminCommand("invalidateUserCache");
    assert.commandWorked(db3.foo.update({}, {$inc: {a: 1}}));
})();

(function testConcurrentUserModification() {
    jsTestLog("Testing having 2 mongoses modify the same user at the same time"); // SERVER-13850

    assert.commandWorked(db1.foo.update({}, {$inc: {a: 1}}));
    assert.commandWorked(db3.foo.update({}, {$inc: {a: 1}}));

    db1.getSiblingDB("test").revokeRolesFromUser("spencer", ["readWrite"]);

    // At this point db3 still thinks "spencer" has readWrite.  Use it to add a different role
    // and make sure it doesn't add back readWrite
    hasAuthzError(db1.foo.update({}, {$inc: {a: 1}}));
    assert.commandWorked(db3.foo.update({}, {$inc: {a: 1}}));

    db3.getSiblingDB("test").grantRolesToUser("spencer", ["dbAdmin"]);

    hasAuthzError(db1.foo.update({}, {$inc: {a: 1}}));
    // modifying "spencer" should force db3 to update its cache entry for "spencer"
    hasAuthzError(db3.foo.update({}, {$inc: {a: 1}}));

    // Make sure nothing changes from invalidating the cache
    db1.adminCommand("invalidateUserCache");
    db3.adminCommand("invalidateUserCache");
    hasAuthzError(db1.foo.update({}, {$inc: {a: 1}}));
    hasAuthzError(db3.foo.update({}, {$inc: {a: 1}}));
})();

(function testDroppingUser() {
    jsTestLog("Testing propagation of dropping users");

    assert.commandWorked(db1.foo.runCommand("collStats"));
    assert.commandWorked(db2.foo.runCommand("collStats"));
    assert.commandWorked(db3.foo.runCommand("collStats"));

    db1.dropUser("spencer");

    // s0/db1 should update its cache instantly
    assert.commandFailedWithCode(db1.foo.runCommand("collStats"), authzErrorCode);

    // s1/db2 should update its cache in 10 seconds.
    sleep(10000);
    assert.soon(
        function () {
            return db2.foo.runCommand("collStats").code == authzErrorCode;
        },
        "Mongos did not update its user cache after 10 seconds",
        5 * 1000 /* Additional 5 seconds of buffer in case of slow update */,
    );

    // We manually invalidate the cache on s2/db3.
    db3.adminCommand("invalidateUserCache");
    assert.commandFailedWithCode(db3.foo.runCommand("collStats"), authzErrorCode);
})();

(function testStaticCacheGeneration() {
    jsTestLog("Testing that cache generations stay static across config server authentication");
    const cfg1 = st.configRS.getPrimary().getDB("admin");
    assert(cfg1.auth("root", "pwd"));

    // Create a previously unauthenticated user which is not in the authorization cached
    assert.commandWorked(cfg1.runCommand({createUser: "previouslyUncached", pwd: "pwd", roles: []}));

    const oldRes = assert.commandWorked(cfg1.runCommand({_getUserCacheGeneration: 1}));

    // Authenticate as the uncached user
    cfg1.logout();
    assert(cfg1.auth("previouslyUncached", "pwd"));
    cfg1.logout();
    assert(cfg1.auth("root", "pwd"));

    const newRes = assert.commandWorked(cfg1.runCommand({_getUserCacheGeneration: 1}));
    assert.eq(
        oldRes.cacheGeneration,
        newRes.cacheGeneration,
        "User cache generation supriously incremented on config servers",
    );

    // Put connection to config server back into default state before shutdown
    cfg1.logout();
})();

st.stop();

print("SUCCESS Completed mongos_cache_invalidation.js");

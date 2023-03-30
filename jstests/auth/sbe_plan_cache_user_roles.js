/**
 * Test $$USER_ROLES works correctly with the SBE plan cache. The same query should return the
 * updated user role info when a different user logs in.
 * @tags: [
 *   featureFlagUserRoles,
 *   # Multiple servers can mess up the plan cache list.
 *   assumes_standalone_mongod,
 * ]
 */
(function() {
"use strict";
load("jstests/libs/sbe_util.js");  // For checkSBEEnabled.

const mongod = MongoRunner.runMongod();
const dbName = "test";
const db = mongod.getDB(dbName);

const sbeEnabled = checkSBEEnabled(db);

// Create two users, each with different roles.
assert.commandWorked(
    db.runCommand({createUser: "user1", pwd: "pwd", roles: [{role: "read", db: dbName}]}));
assert.commandWorked(
    db.runCommand({createUser: "user2", pwd: "pwd", roles: [{role: "readWrite", db: dbName}]}));

const coll = db.sbe_plan_cache_user_roles;
coll.drop();

const verifyPlanCache = function(role) {
    if (sbeEnabled) {
        const caches = coll.getPlanCache().list();
        assert.eq(1, caches.length, caches);
        assert.eq(caches[0].cachedPlan.stages.includes(role), false, caches);
    }
};

assert.commandWorked(coll.insert({_id: 1}));

// While logged in as user1, we should see user1's roles.
db.auth("user1", "pwd");
let results = coll.find({}, {roles: "$$USER_ROLES"}).toArray();
assert.eq(results.length, 1);
assert.eq(results[0].roles, [{_id: "test.read", role: "read", db: "test"}]);
// It can take two executions of a query for a plan to get cached.
coll.find({}, {roles: "$$USER_ROLES"}).toArray();
verifyPlanCache("test.read");
db.logout();

// While logged in as user2, we should see user2's roles.
db.auth("user2", "pwd");
results = coll.find({}, {roles: "$$USER_ROLES"}).toArray();
assert.eq(results.length, 1);
assert.eq(results[0].roles, [{_id: "test.readWrite", role: "readWrite", db: "test"}]);
verifyPlanCache("test.readWrite");
db.logout();

MongoRunner.stopMongod(mongod);
})();

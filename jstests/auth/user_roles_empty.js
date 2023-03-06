// Tests that $$USER_ROLES works as expected in a find command when the array returned by
// $$USER_ROLES is empty and when mongod was started with auth disabled.
// @tags: [featureFlagUserRoles, requires_fcv_70]

(function() {
"use strict";

const mongod = MongoRunner.runMongod();
const dbName = "test";
const db = mongod.getDB(dbName);

// We need to create a collection for the following findOne() to run upon and we need to insert
// a document in that collection so that the findOne() actually has a document to project the
// $$USER_ROLES onto.
assert.commandWorked(db.coll.insert({a: 1, allowedRoles: "all"}));

// When no user is authenticated, $$USER_ROLES evaluates to an empty array. Note that we are
// "authorized" to run this find command because we did not initialize the mongod with any "auth"
// specification.
let result = db.coll.findOne({}, {myRoles: "$$USER_ROLES"});
assert.eq([], result.myRoles);

// Create and authenticate a user that does not have any roles. In this case, $$USER_ROLES will
// also evaluate to an empty array. Because we did not initialize the mongod with any "auth"
// specification, we are "authorized" as this user to execute the following find command even though
// the user does not have read privileges.
assert.commandWorked(db.runCommand({createUser: "user", pwd: "pwd", roles: []}));
db.auth("user", "pwd");

// TODO SERVER-74264: Change the projected field name here to 'myRoles' (also in the assert).
result = db.coll.findOne({}, {myRoles2: "$$USER_ROLES"});
assert.eq([], result.myRoles2);

db.logout();

// Create and authenticate a user that does have roles. Here, we want to test that $$USER_ROLES
// provides the correct value in the case where mongod was started with auth disbaled.
assert.commandWorked(
    db.runCommand({createUser: "user2", pwd: "pwd", roles: [{role: "read", db: dbName}]}));
db.auth("user2", "pwd");

// TODO SERVER-74264: Change the projected field name here to 'myRoles' (also in the assert).
result = db.coll.findOne({}, {myRoles3: "$$USER_ROLES"});
assert.eq([{_id: dbName + ".read", role: "read", db: dbName}], result.myRoles3);

MongoRunner.stopMongod(mongod);
}());

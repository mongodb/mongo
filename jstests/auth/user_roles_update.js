// Tests that $$USER_ROLES is not able to be accessed within an update command.
// @tags: [featureFlagUserRoles, requires_fcv_70]

(function() {
"use strict";

const dbName = "test";
const collName = "coll";
const varNotAvailableErr = 51144;

const mongod = MongoRunner.runMongod({auth: ""});

// Create a user on the admin database.
let admin = mongod.getDB("admin");
assert.commandWorked(admin.runCommand({createUser: "admin", pwd: "admin", roles: ["root"]}));
admin.auth("admin", "admin");

const db = mongod.getDB(dbName);
let coll = db.getCollection(collName);
assert.commandWorked(coll.insert({a: 1}));

// Check that $$USER_ROLES is not available within an update command.
assert.commandFailedWithCode(coll.update({$expr: {$in: ["root", '$$USER_ROLES.role']}}, {a: 2}),
                             varNotAvailableErr);

db.logout();
MongoRunner.stopMongod(mongod);
}());

// Tests that $$USER_ROLES is not available when the server parameter is set to false.
// @tags: [featureFlagUserRoles]

(function() {
"use strict";

const dbName = "test";
const collName = "coll";
const varNotAvailableErr = 51144;

function runTest(conn, disableAtRunTime) {
    // Create a user on the admin database with the root role so that we can create users with other
    // roles to other databases.
    let admin = conn.getDB("admin");
    assert.commandWorked(admin.runCommand({createUser: "admin", pwd: "admin", roles: ["root"]}));
    admin.auth("admin", "admin");

    if (disableAtRunTime) {
        // Disable the $$USER_ROLES server parameter. This requires the admin user to be
        // authenticated.
        assert.commandWorked(admin.runCommand({setParameter: 1, enableAccessToUserRoles: false}));
    }

    const db = conn.getDB(dbName);

    // Create a user.
    assert.commandWorked(db.runCommand({
        createUser: "user",
        pwd: "pwd",
        roles: [{role: "read", db: dbName}],
    }));

    // Create a view.
    let pipeline = [{
        $set:
            {"a": {$cond: {if: {$in: ["read", '$$USER_ROLES.role']}, then: "$a", else: "$$REMOVE"}}}
    }];
    assert.commandWorked(db.createView("coll_view", collName, pipeline));
    let coll = db.getCollection(collName);

    // Insert a document.
    let doc = {_id: 0, a: 1};
    assert.commandWorked(coll.insert(doc));

    // Logout of the admin user so that we can log into the other user.
    admin.logout();

    // Authenticate as the user we created earlier and run a find on the view. Since the
    // $$USER_ROLES server parameter is disabled, the find should fail.
    db.auth("user", "pwd");
    assert.commandFailedWithCode(db.runCommand({find: "coll_view", filter: {}}),
                                 varNotAvailableErr);

    db.logout();
}

// Start up a mongod, and disable the parameter at runtime.
const mongodDisabledAtRuntime = MongoRunner.runMongod({auth: ""});
runTest(mongodDisabledAtRuntime, true);
MongoRunner.stopMongod(mongodDisabledAtRuntime);

// Start up a mongod with the parameter disabled.
const mongodDisabledAtStartup =
    MongoRunner.runMongod({auth: "", setParameter: {enableAccessToUserRoles: false}});
runTest(mongodDisabledAtStartup, false);
MongoRunner.stopMongod(mongodDisabledAtStartup);
}());

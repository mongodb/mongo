// Tests that $$USER_ROLES works as expected in view creation and queries on the view (on both a
// standalone mongod and a sharded cluster).
// @tags: [featureFlagUserRoles]

(function() {
"use strict";

const dbName = "test";
const collName = "accounts";

function runTest(conn, shardingTest = null) {
    // Create a user on the admin database with the root role so that we can create users with other
    // roles to other databases.
    let admin = conn.getDB("admin");
    assert.commandWorked(admin.runCommand({createUser: "admin", pwd: "admin", roles: ["root"]}));
    admin.auth("admin", "admin");

    if (shardingTest) {
        // Shard the collection upon which the view will be created.
        assert.commandWorked(shardingTest.getDB("admin").runCommand({enableSharding: dbName}));
        shardingTest.shardColl(shardingTest.getDB(dbName).getCollection(collName), {_id: 1});
    }

    const db = conn.getDB(dbName);

    // Create a custom role "csm".
    assert.commandWorked(db.runCommand({
        createRole: "csm",
        roles: [],
        privileges: [],
    }));

    // Create a user that has the custom csm role.
    assert.commandWorked(db.runCommand({
        createUser: "user1",
        pwd: "pwd",
        roles: [{role: "csm", db: dbName}, {role: "read", db: dbName}],
    }));

    // Create a user that does not have the custom csm role.
    assert.commandWorked(db.runCommand({
        createUser: "user2",
        pwd: "pwd",
        roles: [{role: "read", db: dbName}],
    }));

    // Create a view on top of the "accounts" collection that hides information about the contract
    // (contractDetails) from anyone who does not have the csm role.
    let pipeline = [{
        $set: {
            "contractDetails": {
                $cond: {
                    if: {$in: ["csm", '$$USER_ROLES.role']},
                    then: "$contractDetails",
                    else: "$$REMOVE"
                }
            }
        }
    }];
    assert.commandWorked(db.createView("accounts_view", collName, pipeline));
    let accounts = db.getCollection(collName);

    // Insert a document that has the "contractDetails" field.
    let doc = {
        _id: 0,
        accountHolder: "MongoDB",
        contractDetails: {
            contractCreatedOn: new Date("2022-01-15T12:00:00Z"),
            contractExpires: new Date("2023-01-15T12:00:00Z"),
        }
    };
    assert.commandWorked(accounts.insert(doc));

    // Logout of the admin user so that we can log into the other users so we can access those roles
    // with $$USER_ROLES below.
    admin.logout();

    // Authenticate as the user who has the custom csm role and execute a find command to see the
    // document in the "accounts_view" view. This user should see the entire document, including
    // the contract details.
    db.auth("user1", "pwd");
    let result = db.accounts_view.find().toArray();
    assert.eq([doc], result);
    db.logout();

    // Authenticate as the user who does not have the csm role and execute the same find command.
    // This user should not see the "contractDetails" field.
    db.auth("user2", "pwd");
    result = db.accounts_view.find().toArray();
    let docWithoutDetails = {
        _id: 0,
        accountHolder: "MongoDB",
    };
    assert.eq([docWithoutDetails], result);
    db.logout();
}

jsTest.log("Test standalone");
const mongod = MongoRunner.runMongod({auth: ""});
runTest(mongod);
MongoRunner.stopMongod(mongod);

jsTest.log("Test sharded cluster");
const st = new ShardingTest({
    mongos: 1,
    config: 1,
    shards: 2,
    keyFile: 'jstests/libs/key1',
});

runTest(st.s, st);
st.stop();
}());

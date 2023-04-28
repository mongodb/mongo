// Tests that $$USER_ROLES is able to be used in an "update" and "findAndModify" commands.
// @tags: [featureFlagUserRoles, requires_fcv_70]

(function() {
"use strict";

const dbName = "test";
const collName = "coll";

function initialize(db) {
    let engDoc = {
        _id: 0,
        allowedRoles: ["eng-app-prod", "eng-app-stg", "read"],
        allowedRole: "read",
        comment: "only for engineering team",
        teamMembers: ["John", "Ashley", "Gina"],
        yearlyEduBudget: 15000,
        yearlyTnEBudget: 2000,
        salesWins: 1000
    };

    let salesDoc = {
        _id: 1,
        allowedRoles: ["sales-person"],
        allowedRole: "observe",
        comment: "only for sales team",
        salesWins: 1000
    };

    let testUpdate = {_id: 2, allowedRole: "test", teamMembersRights: ["testUpdate"]};

    let testFindAndModify = {
        _id: 3,
        allowedRole: "write",
        teamMembersRights: ["testFindAndModify"]
    };

    let coll = db.getCollection(collName);
    assert.commandWorked(coll.insertMany([engDoc, salesDoc, testUpdate, testFindAndModify]));
}

// Test accessing $$USER_ROLES in the query portion of "update" command.
function runUpdateQuery(db) {
    let coll = db.getCollection(collName);

    let pre = coll.findOne(
        {$expr: {$eq: [{$setIntersection: ["$allowedRoles", "$$USER_ROLES.role"]}, []]}});
    var preSalesWins = pre.salesWins;

    assert.commandWorked(coll.update(
        {$expr: {$eq: [{$setIntersection: ["$allowedRoles", "$$USER_ROLES.role"]}, []]}},
        {$inc: {salesWins: 1000}},
        {multi: true}));

    let post = coll.findOne(
        {$expr: {$eq: [{$setIntersection: ["$allowedRoles", "$$USER_ROLES.role"]}, []]}});
    var postSalesWins = post.salesWins;

    assert.eq(postSalesWins, preSalesWins + 1000);
}

// Test accessing $$USER_ROLES in the update portion of "update" command.
function runUpdateUpdate(db) {
    let coll = db.getCollection(collName);

    assert.commandWorked(
        coll.update({_id: 2}, [{$set: {"teamMembersRights": "$$USER_ROLES.role"}}]));

    let post = coll.findOne({_id: 2});

    let expectedResult = {
        _id: 2,
        allowedRole: "test",
        teamMembersRights: ["readWriteAnyDatabase", "read"]
    };

    assert.eq(post, expectedResult);
}

// Test accessing $$USER_ROLES in the query portion of "findAndModify" command.
function runFindAndModifyQuery(db) {
    let coll = db.getCollection(collName);

    let pre = coll.findOne({$expr: {allowedRole: "$$USER_ROLES.role"}});
    var preSalesWins = pre.salesWins;

    db.coll.findAndModify({
        query: {allowedRole: "read", $expr: {allowedRole: "$$USER_ROLES.role"}},
        update: {$inc: {salesWins: 1000}}
    });

    let post = coll.findOne({$expr: {allowedRole: "$$USER_ROLES.role"}});
    var postSalesWins = post.salesWins;

    assert.eq(postSalesWins, preSalesWins + 1000);
}

// Test accessing $$USER_ROLES in the update portion of "findAndModify" command.
function runFindAndModifyUpdate(db) {
    let coll = db.getCollection(collName);

    coll.findAndModify({
        query: {allowedRole: "write"},
        update: [{$set: {"teamMembersRights": "$$USER_ROLES.role"}}]
    });

    let post = coll.findOne({_id: 3});

    let expectedResult = {
        _id: 3,
        allowedRole: "write",
        teamMembersRights: ["readWriteAnyDatabase", "read"]
    };

    assert.eq(post, expectedResult);
}

function runTest(conn, st = null) {
    // Create a user on the admin database.
    let admin = conn.getDB("admin");
    assert.commandWorked(admin.runCommand({createUser: "admin", pwd: "admin", roles: ["root"]}));
    admin.auth("admin", "admin");

    if (st) {
        // Shard the collection that will be used in the update and findAndModify commands.
        assert.commandWorked(conn.getDB("admin").runCommand({enableSharding: dbName}));
        st.shardColl(conn.getDB(dbName).getCollection(collName), {allowedRole: 1});
    }

    const db = conn.getDB(dbName);
    let coll = db.getCollection(collName);

    // Create a user that has roles on more than one database. The readWriteAnyDatabase is
    // necessary for the inserts that follow to work.
    assert.commandWorked(db.runCommand({
        createUser: "user",
        pwd: "pwd",
        roles: [{role: "readWriteAnyDatabase", db: "admin"}, {role: "read", db: dbName}]
    }));

    // Logout of the admin user so that we can log into the other user so we can access those
    // roles with $$USER_ROLES below.
    admin.logout();
    db.auth("user", "pwd");

    initialize(db);

    runUpdateQuery(db);

    runUpdateUpdate(db);

    runFindAndModifyQuery(db);

    runFindAndModifyUpdate(db);

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

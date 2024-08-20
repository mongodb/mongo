// Tests that $$USER_ROLES works as expected in a find command and an aggregate command (on both a
// standalone mongod and a sharded cluster).
// @tags: [requires_fcv_70]

import {ShardingTest} from "jstests/libs/shardingtest.js";

const dbName = "test";
const findCollName = "find_coll";
const aggCollName = "team_budget";

function runFind(db) {
    // We need to create a collection for the following findOne() to run upon and we need to insert
    // a document in that collection so that the findOne() actually has a document to project the
    // $$USER_ROLES onto.
    let coll = db.getCollection(findCollName);
    assert.commandWorked(coll.insert({a: 1}));

    let result = coll.findOne({}, {myRoles: "$$USER_ROLES"});
    assert.eq(
        [
            {_id: "admin.readWriteAnyDatabase", role: "readWriteAnyDatabase", db: "admin"},
            {_id: "test.read", role: "read", db: dbName}
        ],
        result.myRoles);
}

function runAgg(db) {
    let engDoc = {
        _id: 0,
        allowedRoles: ["eng-app-prod", "eng-app-stg", "read"],
        comment: "only for engineering team",
        teamMembers: ["John", "Ashley", "Gina"],
        yearlyEduBudget: 15000,
        yearlyTnEBudget: 2000
    };

    let salesDoc = {
        _id: 1,
        allowedRoles: ["sales-person"],
        comment: "only for sales team",
        salesWins: 1000,
    };

    let coll = db.getCollection(aggCollName);
    assert.commandWorked(coll.insertMany([engDoc, salesDoc]));

    // This will match the documents where the intersection between the allowedRoles field and the
    // user's roles is not empty, i.e. the user's role allows them to see the document in the
    // results. In this case, only the engDoc has the the "read" role that was assigned to the user,
    // so only the engDoc will appear in the results.
    let pipeline = [{
        $match:
            {$expr: {$not: {$eq: [{$setIntersection: ["$allowedRoles", "$$USER_ROLES.role"]}, []]}}}
    }];
    let res = coll.aggregate(pipeline).toArray();
    assert.eq([engDoc], res);

    // Insert a document that has the currently authenticated user's roles in the allowedRoles
    // field, and thus will match the following subpipeline.
    let readDoc = {_id: 2, allowedRoles: ["readWriteAnyDatabase", "read"]};
    assert.commandWorked(coll.insert(readDoc));
    const subpipeline = [{$match: {$expr: {$eq: ["$allowedRoles", "$$USER_ROLES.role"]}}}];

    // Ensure that $$USER_ROLES can be present in a $lookup subpipeline.
    let findColl = db.getCollection(findCollName);
    const lookupPipeline = [
        {$lookup: {from: aggCollName, pipeline: subpipeline, as: "docThatMatchesRoles"}},
        {$project: {_id: 0, docThatMatchesRoles: 1}}
    ];
    const lookupRes = findColl.aggregate(lookupPipeline).toArray();
    assert.eq(
        [{"docThatMatchesRoles": [{"_id": 2, "allowedRoles": ["readWriteAnyDatabase", "read"]}]}],
        lookupRes);

    // Ensure that $$USER_ROLES can be present in a $unionWith subpipeline. The result set should
    // include the one document from findColl and the document from the unioned collection inserted
    // above where the allowedRoles field has the currently authenticated user's roles.
    const unionWithPipeline =
        [{$unionWith: {coll: aggCollName, pipeline: subpipeline}}, {$project: {_id: 0}}];
    const unionWithRes = findColl.aggregate(unionWithPipeline).toArray();
    assert.eq([{a: 1}, {allowedRoles: ["readWriteAnyDatabase", "read"]}], unionWithRes);
}

function runTest(conn, shardingTest = null) {
    // Create a user on the admin database with the root role so that we can create users with other
    // roles to other databases.
    let admin = conn.getDB("admin");
    assert.commandWorked(admin.runCommand({createUser: "admin", pwd: "admin", roles: ["root"]}));
    admin.auth("admin", "admin");

    if (shardingTest) {
        // Shard the two collections that will be used in the find and aggregate commands.
        assert.commandWorked(st.getDB("admin").runCommand({enableSharding: dbName}));
        st.shardColl(st.getDB(dbName).getCollection(findCollName), {_id: 1});
        st.shardColl(st.getDB(dbName).getCollection(aggCollName), {_id: 1});
    }

    const db = conn.getDB(dbName);

    // Create a user that has roles on more than one database. The readWriteAnyDatabase is necessary
    // for the inserts that follow to work.
    assert.commandWorked(db.runCommand({
        createUser: "user",
        pwd: "pwd",
        roles: [{role: "readWriteAnyDatabase", db: "admin"}, {role: "read", db: dbName}]
    }));

    // Logout of the admin user so that we can log into the other user so we can access those roles
    // with $$USER_ROLES below.
    admin.logout();
    db.auth("user", "pwd");

    runFind(db);

    runAgg(db);

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
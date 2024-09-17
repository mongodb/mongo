/**
 * Tests that direct shard connections are correctly allowed and disallowed using authentication.
 *
 * @tags: [featureFlagFailOnDirectShardOperations, requires_fcv_73]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({name: jsTestName(), keyFile: "jstests/libs/key1", shards: 2});

const dbName = 'test';
const collName = 'foo';

const shardConn = st.rs0.getPrimary();
const shardAdminDB = shardConn.getDB("admin");
const shardAdminTestDB = shardConn.getDB(dbName);
const userConn = new Mongo(st.shard0.host);
const userTestDB = userConn.getDB(dbName);

jsTest.log("Setup users for test");
shardAdminDB.createUser({user: "admin", pwd: 'x', roles: ["root"]});
assert(shardAdminDB.auth("admin", 'x'), "Authentication failed");
shardAdminDB.createUser(
    {user: "user", pwd: "y", roles: ["readWriteAnyDatabase", "directShardOperations"]});
assert(userConn.getDB("admin").auth("user", 'y'), "Authentication failed");

assert.commandWorked(shardAdminDB.runCommand(
    {setParameter: 1, logComponentVerbosity: {sharding: {verbosity: 2}, assert: {verbosity: 1}}}));

jsTest.log("Create a collection initially.");
assert.commandWorked(shardAdminTestDB.createCollection(collName));
let bulk = shardAdminTestDB.getCollection(collName).initializeUnorderedBulkOp();
for (let i = 0; i < 10; i++) {
    bulk.insert({_id: i});
}
assert.commandWorked(bulk.execute());

jsTest.log("Run cursor tests");
{
    let findCursor =
        assert.commandWorked(userTestDB.runCommand({find: collName, batchSize: 1})).cursor;

    assert.commandWorked(shardAdminDB.runCommand(
        {revokeRolesFromUser: "user", roles: [{role: "directShardOperations", db: 'admin'}]}));

    assert.commandFailedWithCode(
        userTestDB.runCommand({getMore: findCursor.id, collection: collName}),
        ErrorCodes.Unauthorized);
    assert.commandWorked(
        userTestDB.runCommand({killCursors: collName, cursors: [NumberLong(findCursor.id)]}));

    assert.commandWorked(shardAdminDB.runCommand(
        {grantRolesToUser: "user", roles: [{role: "directShardOperations", db: 'admin'}]}));
}

jsTest.log("Run session and transaction tests");
function testTransactionCommand(userConn, cmdToTest, shouldFail, isAdmin) {
    let session = assert.commandWorked(userConn.adminCommand({startSession: 1}));
    assert.commandWorked(userConn.runCommand({
        insert: collName,
        documents: [{_id: ObjectId()}],
        lsid: session.id,
        stmtIds: [NumberInt(0)],
        txnNumber: NumberLong(0),
        startTransaction: true,
        autocommit: false,
    }));
    cmdToTest.lsid = session.id;

    assert.commandWorked(shardAdminDB.runCommand(
        {revokeRolesFromUser: "user", roles: [{role: "directShardOperations", db: 'admin'}]}));

    let testDB = isAdmin ? userConn.getSiblingDB('admin') : userConn;
    if (shouldFail) {
        assert.commandFailedWithCode(testDB.runCommand(cmdToTest), ErrorCodes.Unauthorized);
    } else {
        assert.commandWorked(testDB.runCommand(cmdToTest));
    }
    assert.commandWorked(testDB.runCommand({killSessions: [session.lsid]}));

    assert.commandWorked(shardAdminDB.runCommand(
        {grantRolesToUser: "user", roles: [{role: "directShardOperations", db: 'admin'}]}));
}
// Transaction cannot be continued
testTransactionCommand(userTestDB,
                       {
                           insert: collName,
                           documents: [{_id: ObjectId()}],
                           stmtIds: [NumberInt(1)],
                           txnNumber: NumberLong(0),
                           autocommit: false
                       },
                       true /* shouldFail */,
                       false /* isAdmin */);
// But it can be committed or aborted. This is fine because no data movement commands will be
// able to commit while there are ongoing transactions due to all locks being held throughout the
// transaction.
testTransactionCommand(userTestDB,
                       {commitTransaction: 1, txnNumber: NumberLong(0), autocommit: false},
                       false /* shouldFail */,
                       true /* isAdmin */);
testTransactionCommand(userTestDB,
                       {
                           abortTransaction: 1,
                           txnNumber: NumberLong(0),
                           autocommit: false,
                       },
                       false /* shouldFail */,
                       true /* isAdmin */);

jsTest.log("Logout of final users");
// Reset users and logout for next set of tests.
userTestDB.logout();
shardAdminTestDB.dropUser("user");
shardAdminDB.logout();

st.stop();

/**
 * Tests that direct shard connections with addShard and moveCollection does not cause data loss.
 *
 * @tags: [featureFlagFailOnDirectShardOperations,
 * featureFlagTrackUnshardedCollectionsOnShardingCatalog]
 */

const st = new ShardingTest({name: jsTestName(), keyFile: "jstests/libs/key1", shards: 1});

const dbName = 'test';
const collName = 'foo';

const shardConn = st.rs0.getPrimary();
const shardAdminDB = shardConn.getDB("admin");

// Set up connections, userTestDB will be the connection performing the direct reads and writes.
shardAdminDB.createUser({user: "admin", pwd: 'x', roles: ["root"]});
assert(shardAdminDB.auth("admin", 'x'), "Authentication failed");
shardAdminDB.getSiblingDB(dbName).createUser({user: "user", pwd: "y", roles: ["readWrite"]});

// Set up shard to be added and mongoS connection to add the shard through.
var newShard = new ReplSetTest({name: "additionalShard", nodes: 1});
newShard.startSet({keyFile: "jstests/libs/key1", shardsvr: ""});
newShard.initiate();
let mongosAdminUser = st.s.getDB('admin');
if (!TestData.configShard) {
    mongosAdminUser.createUser({user: "globalAdmin", pwd: 'a', roles: ["root"]});
    assert(mongosAdminUser.auth("globalAdmin", "a"), "Authentication failed");
} else {
    assert(mongosAdminUser.auth("admin", "x"), "Authentication failed");
}

function runInsertViaDirectConnection(hostName, dbName, collName) {
    let conn = new Mongo(hostName);
    let userTestDB = conn.getDB(dbName);
    assert(userTestDB.auth("user", "y"), "Authentication failed");

    let counter = 0;
    const startTimeMs = Date.now();
    while (Date.now() - startTimeMs < 15 * 1000) {
        const cmdStartTimeMs = Date.now();
        assert.commandWorkedOrFailedWithCode(
            userTestDB.runCommand({insert: collName, documents: [{_id: counter++}]}),
            ErrorCodes.Unauthorized);
        const cmdEndTimeMs = Date.now();
        sleep(Math.max(0, 100 - (cmdEndTimeMs - cmdStartTimeMs)));
    }
}

// Create the collection initially.
assert.commandWorked(mongosAdminUser.getSiblingDB(dbName).runCommand({create: collName}));

// Begin the insertion thread via a direct connection.
const insertThread = new Thread(runInsertViaDirectConnection, st.shard0.host, dbName, collName);
insertThread.start();

// Adding the second shard should block all direct write operations, including the ongoing inserts.
// Moving the collection ensures that checkmetadataconsistency will fail if the inserts continue
// executing.
assert.commandWorked(mongosAdminUser.runCommand({addShard: newShard.getURL()}));
assert.commandWorked(
    mongosAdminUser.runCommand({moveCollection: dbName + '.' + collName, toShard: newShard.name}));

insertThread.join();

// Drop extra users and logout to ensure that hooks run successfully.
shardAdminDB.dropUser("user");
shardAdminDB.logout();
mongosAdminUser.logout();

// Stop the sharding test before the additional shard to ensure the test hooks run successfully.
st.stop();
newShard.stopSet();

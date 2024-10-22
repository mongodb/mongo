import {assertDropCollection} from "jstests/libs/collection_drop_recreate.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

// Tests renaming the system.buckets collection.
// Set up the test database.
const dbName = "test";
const collName = "mongosync.tmp.UUID123";
const bucketsCollName = `system.buckets.${collName}`;
const targetBucketsCollName = "system.buckets.manual";

function renameBucketsCollection(adminDB, username, shouldSucceed) {
    // Create collection under admin user
    assert.eq(1, adminDB.auth("admin", "admin"));

    const testDB = adminDB.getSiblingDB(dbName);
    assertDropCollection(testDB, bucketsCollName);
    assertDropCollection(testDB, targetBucketsCollName);
    testDB[bucketsCollName].drop();
    testDB[targetBucketsCollName].drop();

    assert.commandWorked(
        testDB.createCollection(bucketsCollName, {timeseries: {timeField: "time"}}));
    adminDB.logout();

    // Try rename with test users
    jsTestLog("Testing system.buckets renaming with username: " + username);
    assert(adminDB.auth(username, 'password'));

    // No privilege grants the ability to rename a system.buckets collection to a non-bucket
    // namespace.
    assert.commandFailed(testDB.adminCommand({
        renameCollection: `${testDB}.${bucketsCollName}`,
        to: `${testDB}.${collName}`,
        dropTarget: false
    }));

    const res = testDB.adminCommand({
        renameCollection: `${testDB}.${bucketsCollName}`,
        to: `${testDB}.${targetBucketsCollName}`,
        dropTarget: true
    });

    assert.eq((shouldSucceed) ? 1 : 0,
              res.ok,
              "Rename collection failed or succeeded unexpectedly:" + tojson(res));

    adminDB.logout();
}

function runTest(conn) {
    const adminDB = conn.getDB("admin");

    // Create the admin user.
    adminDB.createUser({user: 'admin', pwd: 'admin', roles: ['root']});
    assert.eq(1, adminDB.auth("admin", "admin"));

    // Create roles with ability to rename system.buckets collections.
    adminDB.createRole({
        role: "renameBucketsOnly",
        privileges: [{
            resource: {db: '', system_buckets: ''},
            actions: [
                "createIndex",
                "dropCollection",
                "find",
                "insert",
            ]
        }],
        roles: []
    });

    // Create test users.
    adminDB.createUser(
        {user: 'userAdmin', pwd: 'password', roles: ['userAdminAnyDatabase', 'renameBucketsOnly']});

    // Create read and write users.
    adminDB.createUser({
        user: 'readWriteAdmin',
        pwd: 'password',
        roles: ['readWriteAnyDatabase', 'renameBucketsOnly']
    });

    // Create strong users.
    adminDB.createUser({user: 'restore', pwd: 'password', roles: ['restore', 'renameBucketsOnly']});
    adminDB.createUser({user: 'root', pwd: 'password', roles: ['root', 'renameBucketsOnly']});
    adminDB.createUser(
        {user: 'rootier', pwd: 'password', roles: ['__system', 'renameBucketsOnly']});
    adminDB.createUser(
        {user: 'reader', pwd: 'password', roles: ['readAnyDatabase', 'renameBucketsOnly']});

    adminDB.logout();

    // Expect renaming system.buckets collection to succeed.
    renameBucketsCollection(adminDB, 'restore', true);
    renameBucketsCollection(adminDB, 'root', true);
    renameBucketsCollection(adminDB, 'rootier', true);

    // Second test case should fail for user with inadequate role.
    renameBucketsCollection(adminDB, 'reader', false);
    renameBucketsCollection(adminDB, 'readWriteAdmin', false);
    renameBucketsCollection(adminDB, 'userAdmin', false);
}

jsTestLog("ReplicaSet: Testing rename timeseries collection");
{
    const rst = new ReplSetTest({nodes: 1, auth: "", keyFile: 'jstests/libs/key1'});
    rst.startSet();

    rst.initiate();
    rst.awaitReplication();
    runTest(rst.getPrimary());
    rst.stopSet();
}

jsTestLog("Sharding: Testing rename timeseries collection");
{
    const st = new ShardingTest({
        shards: 1,
        mongos: 1,
        config: 1,
        keyFile: "jstests/libs/key1",
        other: {rsOptions: {auth: ""}}
    });

    runTest(st.s);

    st.stop();
}

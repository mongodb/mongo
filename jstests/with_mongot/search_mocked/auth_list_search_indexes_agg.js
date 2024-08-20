/**
 * End-to-end testing that the $listSearchIndexes aggregation stage can only be run with the
 * listSearchIndexes privileges, or with a built-in role.
 *
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {MongotMock} from "jstests/with_mongot/mongotmock/lib/mongotmock.js";

const dbName = jsTestName();
const collName = "collection";

const mongotMock = new MongotMock();
mongotMock.start();
const mockConn = mongotMock.getConnection();
const adminLogErr = "Authentication failed should be able to login to adminDB.";

function createSuperUser(conn) {
    const adminDB = conn.getDB("admin");
    assert.commandWorked(
        adminDB.runCommand({createUser: "super", pwd: "super", roles: ["__system"]}));
    assert(adminDB.auth("super", "super"), adminLogErr);
    assert(adminDB.logout());
}

function listSearchIndexesPrivilegeErrors(conn) {
    // Test that $listSearchIndexes errors when run without the listSearchIndexes privilege.
    // Set up a user without any role or privilege.
    const adminDB = conn.getDB("admin");
    const testDB = adminDB.getSiblingDB(dbName);
    assert(adminDB.auth("super", "super"), adminLogErr);
    assert.commandWorked(testDB.createCollection(collName));
    assert.commandWorked(testDB.runCommand({
        createUser: "user_no_priv",
        pwd: "pwd",
        roles: [],
    }));
    assert(adminDB.logout());
    assert(testDB.logout());

    // Assert the aggregation fails.
    assert(testDB.auth("user_no_priv", "pwd"), adminLogErr);
    assert.commandFailedWithCode(
        testDB.runCommand({aggregate: collName, pipeline: [{$listSearchIndexes: {}}], cursor: {}}),
        ErrorCodes.Unauthorized);
    assert(testDB.logout());
}

function listSearchIndexPrivilegeWorks(conn) {
    const adminDB = conn.getDB("admin");
    const testDB = adminDB.getSiblingDB(dbName);

    // Test that $listSearchIndexes succeeds when run with the listSearchIndexes privilege.
    // Set up a user with the $listSearchIndex privilege.
    assert(adminDB.auth("super", "super"), adminLogErr);
    assert.commandWorked(testDB.runCommand({
        createRole: "search_idx_priv",
        roles: [],
        privileges: [{resource: {db: dbName, collection: ""}, actions: ["listSearchIndexes"]}]
    }));
    assert.commandWorked(testDB.runCommand({
        createUser: "user_with_priv",
        pwd: "pwd",
        roles: [{role: "search_idx_priv", db: dbName}]
    }));
    assert(adminDB.logout());

    // Assert the aggregation succeeds.
    const manageSearchIndexCommandResponse = {
        ok: 1,
        cursor: {
            id: 0,
            ns: "db-name.coll-name",
            firstBatch: [{
                id: "index-Id",
                name: "index-name",
                status: "ACTIVE",
                definition: {
                    mappings: {
                        dynamic: true,
                    },
                    synonyms: [{"synonym-mapping": "thing"}],
                }
            }]
        }
    };
    mongotMock.setMockSearchIndexCommandResponse(manageSearchIndexCommandResponse);
    assert(testDB.auth("user_with_priv", "pwd"));
    assert.commandWorked(
        testDB.runCommand({aggregate: collName, pipeline: [{$listSearchIndexes: {}}], cursor: {}}));
    assert(testDB.logout());

    // Test that $listSearchIndexes succeeds when run with the 'read' built-in role.
    // Set up a user with the 'read' role.
    assert(adminDB.auth("super", "super"), adminLogErr);
    assert.commandWorked(testDB.runCommand(
        {createUser: "user_read", pwd: "pwd", roles: [{role: "read", db: dbName}]}));
    assert(adminDB.logout());
    // Assert the aggregation succeeds.
    assert(testDB.auth("user_read", "pwd"));
    mongotMock.setMockSearchIndexCommandResponse(manageSearchIndexCommandResponse);
    assert.commandWorked(
        testDB.runCommand({aggregate: collName, pipeline: [{$listSearchIndexes: {}}], cursor: {}}));
    assert(testDB.logout());
}

(function runOnShardedCluster() {
    let st = new ShardingTest({
        mongos: 1,
        shards: 1,
        keyFile: "jstests/libs/key1",
        other: {
            mongosOptions: {setParameter: {searchIndexManagementHostAndPort: mockConn.host}},
            rs0: {
                // Need the shard to have a stable secondary to test commands against.
                nodes: [{}, {rsConfig: {priority: 0}}],
                setParameter: {searchIndexManagementHostAndPort: mockConn.host},
            },
        }
    });

    // This must be run first since it sets a global super user that the other functions need.
    createSuperUser(st.s);
    listSearchIndexesPrivilegeErrors(st.s);
    listSearchIndexPrivilegeWorks(st.s);
    st.stop();
})();

(function runOnReplicaSet() {
    const rst = new ReplSetTest({
        nodes: 1,
        keyFile: "jstests/libs/key1",
        nodeOptions: {setParameter: {searchIndexManagementHostAndPort: mockConn.host}}
    });
    rst.startSet();
    rst.initiate();
    const primary = rst.getPrimary();

    // This must be run first since it sets a global super user that the other functions need.
    createSuperUser(primary);
    listSearchIndexesPrivilegeErrors(primary);
    listSearchIndexPrivilegeWorks(primary);
    rst.stopSet();
})();

mongotMock.stop();

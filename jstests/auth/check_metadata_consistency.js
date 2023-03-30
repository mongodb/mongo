/*
 * Tests to validate the privileges of checkMetadataConsistency command.
 *
 * @tags: [featureFlagCheckMetadataConsistency]
 */

(function() {
"use strict";

// Helper function to assert that the checkMetadataConsistency command succeeds
function assertAuthCommandWorked(adminDb, conn, user) {
    assert(adminDb.logout());
    assert(adminDb.auth(user, "pwd"));
    const inconsistencies = conn.checkMetadataConsistency().toArray();
    assert.eq(1, inconsistencies.length);
    assert.eq("MisplacedCollection", inconsistencies[0].type);
}

// Helper function to assert that the checkMetadataConsistency command fails
function assertAuthCommandFailed(adminDb, conn, user) {
    assert(adminDb.logout());
    assert(adminDb.auth(user, "pwd"));
    assert.commandFailedWithCode(
        conn.runCommand({checkMetadataConsistency: 1}),
        ErrorCodes.Unauthorized,
        "user should no longer have privileges to execute checkMetadataConsistency command.");
}

// Configure initial sharding cluster
const st = new ShardingTest({keyFile: "jstests/libs/key1", useHostname: false});

const shardAdmin = st.shard0.getDB("admin");
shardAdmin.createUser({user: "admin", pwd: "x", roles: ["root"]});
shardAdmin.auth("admin", "x");

const adminDb = st.s.getDB("admin");
adminDb.createUser({user: "admin", pwd: "x", roles: ["root"]});
adminDb.auth("admin", "x");

const dbName = "testCheckMetadataConsistencyDB";
const db = st.s.getDB(dbName);

// Insert a hidden unsharded collection inconsistency.
assert.commandWorked(
    adminDb.adminCommand({enableSharding: dbName, primaryShard: st.shard1.shardName}));
assert.commandWorked(
    st.shard0.getDB(dbName).runCommand({insert: "coll", documents: [{_id: "foo"}]}));

(function createRolesToTest() {
    assert.commandWorked(adminDb.runCommand({
        createRole: "clusterLevelRole",
        roles: [],
        privileges: [{resource: {cluster: true}, actions: ["checkMetadataConsistency"]}]
    }));

    assert.commandWorked(adminDb.runCommand({
        createRole: "databaseLevelRole",
        roles: [],
        privileges:
            [{resource: {db: dbName, collection: ""}, actions: ["checkMetadataConsistency"]}]
    }));

    assert.commandWorked(adminDb.runCommand({
        createRole: "collectionLevelRole",
        roles: [],
        privileges:
            [{resource: {db: dbName, collection: "coll"}, actions: ["checkMetadataConsistency"]}]
    }));

    assert.commandWorked(adminDb.runCommand({
        createRole: "roleWithAllNonSystemCollectionsPrivileges",
        roles: [],
        privileges: [{resource: {db: "", collection: ""}, actions: ["checkMetadataConsistency"]}]
    }));

    assert.commandWorked(adminDb.runCommand({
        createRole: "roleWithNotRelatedAction",
        roles: [],
        privileges: [{resource: {cluster: true}, actions: ["allCollectionStats"]}]
    }));
})();

(function createUsersToTest() {
    assert.commandWorked(adminDb.runCommand({
        createUser: "clusterManagerUser",
        pwd: "pwd",
        roles: [{role: "clusterManager", db: "admin"}]
    }));

    assert.commandWorked(adminDb.runCommand({
        createUser: "clusterAdminUser",
        pwd: "pwd",
        roles: [{role: "clusterAdmin", db: "admin"}]
    }));

    assert.commandWorked(adminDb.runCommand({
        createUser: "userWithClusterLevelRole",
        pwd: "pwd",
        roles: [{role: "clusterLevelRole", db: "admin"}]
    }));

    assert.commandWorked(adminDb.runCommand({
        createUser: "userWithDatabaseLevelRole",
        pwd: "pwd",
        roles: [{role: "databaseLevelRole", db: "admin"}]
    }));

    assert.commandWorked(adminDb.runCommand({
        createUser: "userWithCollectionLevelRole",
        pwd: "pwd",
        roles: [{role: "collectionLevelRole", db: "admin"}]
    }));

    assert.commandWorked(adminDb.runCommand({
        createUser: "userWithAllNonSystemCollectionsPrivileges",
        pwd: "pwd",
        roles: [{role: "roleWithAllNonSystemCollectionsPrivileges", db: "admin"}]
    }));

    assert.commandWorked(adminDb.runCommand({
        createUser: "userWithUnrelatedRole",
        pwd: "pwd",
        roles: [{role: "hostManager", db: "admin"}]
    }));

    assert.commandWorked(adminDb.runCommand({
        createUser: "userWithUnrelatedAction",
        pwd: "pwd",
        roles: [{role: "roleWithNotRelatedAction", db: "admin"}]
    }));

    assert.commandWorked(
        adminDb.runCommand({createUser: "userWithNoRoles", pwd: "pwd", roles: []}));
})();

shardAdmin.logout();
adminDb.logout();

(function testClusterLevelModePrivileges() {
    assertAuthCommandWorked(adminDb, adminDb, "clusterManagerUser");
    assertAuthCommandWorked(adminDb, adminDb, "clusterAdminUser");
    assertAuthCommandWorked(adminDb, adminDb, "userWithClusterLevelRole");

    assertAuthCommandFailed(adminDb, adminDb, "userWithAllNonSystemCollectionsPrivileges");
    assertAuthCommandFailed(adminDb, adminDb, "userWithDatabaseLevelRole");
    assertAuthCommandFailed(adminDb, adminDb, "userWithCollectionLevelRole");
    assertAuthCommandFailed(adminDb, adminDb, "userWithUnrelatedAction");
    assertAuthCommandFailed(adminDb, adminDb, "userWithUnrelatedRole");
    assertAuthCommandFailed(adminDb, adminDb, "userWithNoRoles");
})();

(function testDatabaseLevelModePrivileges() {
    assertAuthCommandWorked(adminDb, db, "clusterManagerUser");
    assertAuthCommandWorked(adminDb, db, "clusterAdminUser");
    assertAuthCommandWorked(adminDb, db, "userWithClusterLevelRole");
    assertAuthCommandWorked(adminDb, db, "userWithDatabaseLevelRole");
    assertAuthCommandWorked(adminDb, db, "userWithAllNonSystemCollectionsPrivileges");

    assertAuthCommandFailed(adminDb, db, "userWithCollectionLevelRole");
    assertAuthCommandFailed(adminDb, db, "userWithUnrelatedAction");
    assertAuthCommandFailed(adminDb, db, "userWithUnrelatedRole");
    assertAuthCommandFailed(adminDb, db, "userWithNoRoles");
})();

(function testCollectionLevelModePrivileges() {
    // TODO: SERVER-74078: Implement collection level mode for checkMetadataConsistency.
})();

st.stop();
})();

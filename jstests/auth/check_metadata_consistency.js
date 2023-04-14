/*
 * Tests to validate the privileges of checkMetadataConsistency command.
 *
 * @tags: [
 *    featureFlagCheckMetadataConsistency,
 *    requires_fcv_70,
 * ]
 */

(function() {
"use strict";

const kClusterLevel = "clusterLevel";
const kDatabaseLevel = "databaseLevel";
const kCollectionLevel = "collectionLevel";

// Helper function to assert that the checkMetadataConsistency command succeeds
function assertAuthCommandWorked(adminDb, conn, user, level) {
    assert(adminDb.logout());
    assert(adminDb.auth(user, "pwd"));
    const cmd = () => {
        if (level === kClusterLevel || level === kDatabaseLevel) {
            return conn.checkMetadataConsistency().toArray();
        } else {
            return conn.coll.checkMetadataConsistency().toArray();
        }
    };
    const inconsistencies = cmd();
    assert.eq(1, inconsistencies.length);
    assert.eq("MisplacedCollection", inconsistencies[0].type);
}

// Helper function to assert that the checkMetadataConsistency command fails
function assertAuthCommandFailed(adminDb, conn, user, level) {
    assert(adminDb.logout());
    assert(adminDb.auth(user, "pwd"));

    const cmd = () => {
        if (level === kClusterLevel || level === kDatabaseLevel) {
            return conn.runCommand({checkMetadataConsistency: 1});
        } else {
            return conn.runCommand({checkMetadataConsistency: "coll"});
        }
    };

    assert.commandFailedWithCode(
        cmd(),
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
    assertAuthCommandWorked(adminDb, adminDb, "clusterManagerUser", kClusterLevel);
    assertAuthCommandWorked(adminDb, adminDb, "clusterAdminUser", kClusterLevel);
    assertAuthCommandWorked(adminDb, adminDb, "userWithClusterLevelRole", kClusterLevel);

    assertAuthCommandFailed(
        adminDb, adminDb, "userWithAllNonSystemCollectionsPrivileges", kClusterLevel);
    assertAuthCommandFailed(adminDb, adminDb, "userWithDatabaseLevelRole", kClusterLevel);
    assertAuthCommandFailed(adminDb, adminDb, "userWithCollectionLevelRole", kClusterLevel);
    assertAuthCommandFailed(adminDb, adminDb, "userWithUnrelatedAction", kClusterLevel);
    assertAuthCommandFailed(adminDb, adminDb, "userWithUnrelatedRole", kClusterLevel);
    assertAuthCommandFailed(adminDb, adminDb, "userWithNoRoles", kClusterLevel);
})();

(function testDatabaseLevelModePrivileges() {
    assertAuthCommandWorked(adminDb, db, "clusterManagerUser", kDatabaseLevel);
    assertAuthCommandWorked(adminDb, db, "clusterAdminUser", kDatabaseLevel);
    assertAuthCommandWorked(adminDb, db, "userWithClusterLevelRole", kDatabaseLevel);
    assertAuthCommandWorked(adminDb, db, "userWithDatabaseLevelRole", kDatabaseLevel);
    assertAuthCommandWorked(
        adminDb, db, "userWithAllNonSystemCollectionsPrivileges", kDatabaseLevel);

    assertAuthCommandFailed(adminDb, db, "userWithCollectionLevelRole", kDatabaseLevel);
    assertAuthCommandFailed(adminDb, db, "userWithUnrelatedAction", kDatabaseLevel);
    assertAuthCommandFailed(adminDb, db, "userWithUnrelatedRole", kDatabaseLevel);
    assertAuthCommandFailed(adminDb, db, "userWithNoRoles", kDatabaseLevel);
})();

(function testCollectionLevelModePrivileges() {
    assertAuthCommandWorked(adminDb, db, "clusterManagerUser", kCollectionLevel);
    assertAuthCommandWorked(adminDb, db, "clusterAdminUser", kCollectionLevel);
    assertAuthCommandWorked(adminDb, db, "userWithClusterLevelRole", kCollectionLevel);
    assertAuthCommandWorked(adminDb, db, "userWithDatabaseLevelRole", kCollectionLevel);
    assertAuthCommandWorked(adminDb, db, "userWithCollectionLevelRole", kCollectionLevel);
    assertAuthCommandWorked(
        adminDb, db, "userWithAllNonSystemCollectionsPrivileges", kCollectionLevel);

    assertAuthCommandFailed(adminDb, db, "userWithUnrelatedAction", kCollectionLevel);
    assertAuthCommandFailed(adminDb, db, "userWithUnrelatedRole", kCollectionLevel);
    assertAuthCommandFailed(adminDb, db, "userWithNoRoles", kCollectionLevel);
})();

st.stop();
})();

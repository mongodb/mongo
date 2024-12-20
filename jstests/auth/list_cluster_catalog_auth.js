/*
 * Test to validate the privileges of using $listClusterCatalog stage.
 *
 * @tags: [
 *   # TODO (SERVER-98651) remove the tag
 *   requires_fcv_81,
 * ]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";

const AUTHORIZED = true;
const UNAUTHORIZED = false;

const st = new ShardingTest({shards: 1, keyFile: 'jstests/libs/key1'});
const mongos = st.s;

// Dbs
const kDb1 = "test1";
const kDb2 = "test2";
const kColl = "coll";

const adminDb = mongos.getDB("admin");
const db1 = mongos.getDB(kDb1);
const db2 = mongos.getDB(kDb2);

// Create a super user
adminDb.createUser({user: 'super', pwd: 'super', roles: jsTest.adminUserRoles});
assert(adminDb.logout());

// Helper used to create roles and users.
function runAsSuperUser(fn) {
    assert(adminDb.auth("super", "super"));
    fn();
    adminDb.logout();
}

// Helper used to run test cases for a specific user.
function authAndRun(db, username, fn) {
    assert(db.auth(username, "pwd"));
    fn();
    assert(db.logout());
}

// Helper used to validate the $listClusterCatalog stage authorization and result.
function isPresent(stageResult, dbName) {
    return stageResult.find((obj) => {
        return obj.db === dbName;
    }) !== undefined;
}

function commandWorked(db, expectedDbs) {
    assert.commandWorked(
        db.runCommand({aggregate: 1, pipeline: [{$listClusterCatalog: {}}], cursor: {}}));
    let stageResult = db.aggregate([{$listClusterCatalog: {}}]).toArray();
    expectedDbs.forEach((dbName) => {
        assert(isPresent(stageResult,dbName),`The db ${dbName} is expected to be returned by $listClusterCatalog, but it was not found. Stage Result: ${tojson(stageResult)}`);
    });
}

function notAuthorized(db) {
    assert.commandFailedWithCode(
        db.runCommand({aggregate: 1, pipeline: [{$listClusterCatalog: {}}], cursor: {}}),
        [ErrorCodes.Unauthorized]);
}

// Setup The test
const kListCollectionsRole = "listCollectionsRole";
const kListClusterCatalogRole = "listClusterCatalog";
runAsSuperUser(() => {
    // Insert some collections to fill the catalog
    assert.commandWorked(db1.createCollection(kColl));
    assert.commandWorked(db2.createCollection(kColl));

    // Create all the roles needed
    assert.commandWorked(db1.runCommand({
        createRole: kListCollectionsRole,
        privileges: [{resource: {db: kDb1, collection: ""}, actions: ["listCollections"]}],
        roles: []
    }));
    assert.commandWorked(db2.runCommand({
        createRole: kListCollectionsRole,
        privileges: [{resource: {db: kDb2, collection: ""}, actions: ["listCollections"]}],
        roles: []
    }));
    assert.commandWorked(adminDb.runCommand({
        createRole: kListClusterCatalogRole,
        privileges: [{resource: {cluster: true}, actions: ["listClusterCatalog"]}],
        roles: []
    }));
});

/* Role per database:
 * admin: `none`
 * db1:   `none`
 * db2:   `none`
 */
jsTest.log("Test no privileges for $listClusterCatalog");
{
    const kUserNoPriv = "user_no_priv";
    runAsSuperUser(() => {
        assert.commandWorked(adminDb.runCommand({createUser: kUserNoPriv, pwd: "pwd", roles: []}));
    });
    authAndRun(adminDb, kUserNoPriv, () => {
        notAuthorized(db1);
        notAuthorized(db2);
        notAuthorized(adminDb);
    });
}

/*
 * Role per database:
 * admin: `none`
 * db1:   `read`
 * db2:   `none`
 */
jsTest.log("Test read privileges for $listClusterCatalog on a user db");
{
    const kUserReadPriv = "user_read_priv";
    runAsSuperUser(() => {
        assert.commandWorked(db1.runCommand(
            {createUser: kUserReadPriv, pwd: "pwd", roles: [{role: 'read', db: kDb1}]}));
    });
    authAndRun(db1, kUserReadPriv, () => {
        commandWorked(db1, [kDb1]);
        notAuthorized(db2);
        notAuthorized(adminDb);
    });
}

/*
 * Role per database:
 * admin: `read`
 * db1:   `none`
 * db2:   `none`
 */
jsTest.log("Test read role for $listClusterCatalog on the admin db");
{
    const kUserReadPriv = "user_read_priv2";
    runAsSuperUser(() => {
        assert.commandWorked(
            adminDb.runCommand({createUser: kUserReadPriv, pwd: "pwd", roles: ['read']}));
    });
    authAndRun(adminDb, kUserReadPriv, () => {
        notAuthorized(db1);
        notAuthorized(db2);
        notAuthorized(adminDb);
    });
}

/*
 * Role per database:
 * admin: `clusterMonitor`
 * db1:   `none`
 * db2:   `none`
 */
jsTest.log("Test clusterMonitor role for $listClusterCatalog with no permissions on user dbs");
{
    const kUserClusterPriv = "user_cluster_priv";
    runAsSuperUser(() => {
        assert.commandWorked(adminDb.runCommand({
            createUser: kUserClusterPriv,
            pwd: "pwd",
            roles: [{role: 'clusterMonitor', db: 'admin'}]
        }));
    });
    authAndRun(adminDb, kUserClusterPriv, () => {
        notAuthorized(db1);
        notAuthorized(db2);
        commandWorked(adminDb, [kDb1, kDb2]);
    });
}

/*
 * Role per database:
 * admin: `clusterMonitor`
 * db1:   `read`
 * db2:   `none`
 */
jsTest.log("Test clusterMonitor role for $listClusterCatalog with read role on one db");
{
    const kUserClusterPriv = "user_cluster_priv2";
    runAsSuperUser(() => {
        assert.commandWorked(adminDb.runCommand({
            createUser: kUserClusterPriv,
            pwd: "pwd",
            roles: [{role: 'clusterAdmin', db: 'admin'}, {role: 'read', db: kDb1}]

        }));
    });
    authAndRun(adminDb, kUserClusterPriv, () => {
        commandWorked(db1, [kDb1]);
        notAuthorized(db2);
        commandWorked(adminDb, [kDb1, kDb2]);
    });
}

/*
 * Role per database:
 * admin: `none`
 * db1:   `actions: ["listCollections"]`
 * db2:   `none`
 */
jsTest.log("Test `listCollections` privileges for $listClusterCatalog on 1 db");
{
    const kUserListCollsPriv = "user_list_collections_priv";
    runAsSuperUser(() => {
        assert.commandWorked(db1.runCommand(
            {createUser: kUserListCollsPriv, pwd: "pwd", roles: [kListCollectionsRole]}));
    });
    authAndRun(db1, kUserListCollsPriv, () => {
        commandWorked(db1, [kDb1]);
        notAuthorized(db2);
        notAuthorized(adminDb);
    });
}

/*
 * Role per database:
 * admin: `clusterMonitor`
 * db1:   `actions: ["listCollections"]`
 * db2:   `none`
 */
jsTest.log("Test clusterMonitor role for $listClusterCatalog with specific permissions on dbs");
{
    const kUserClusterPriv = "user_cluster_priv4";
    runAsSuperUser(() => {
        assert.commandWorked(adminDb.runCommand({
            createUser: kUserClusterPriv,
            pwd: "pwd",
            roles: [{role: 'clusterMonitor', db: "admin"}, {role: kListCollectionsRole, db: kDb1}]
        }));
    });
    authAndRun(adminDb, kUserClusterPriv, () => {
        commandWorked(db1, [kDb1]);
        notAuthorized(db2);
        commandWorked(adminDb, [kDb1, kDb2]);
    });
}

/*
 * Role per database:
 * admin: `actions: ["listClusterCatalog"]`
 * db1:   `none`
 * db1:   `none`
 */
jsTest.log("Test minimum privileges requirements for $listClusterCatalog");
{
    const kUserClusterPriv = "user_cluster_priv6";
    runAsSuperUser(() => {
        assert.commandWorked(adminDb.runCommand({
            createUser: kUserClusterPriv,
            pwd: "pwd",
            roles: [{role: kListClusterCatalogRole, db: "admin"}]
        }));
    });
    authAndRun(adminDb, kUserClusterPriv, () => {
        notAuthorized(db1);
        notAuthorized(db2);
        commandWorked(adminDb, [kDb1, kDb2]);
    });
}

st.stop();

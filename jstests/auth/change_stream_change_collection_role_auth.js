/**
 * Tests that when RBAC (Role-Based Access Control) is enabled on the cluster, then the user of the
 * root role can issue 'find', 'insert', 'update', 'remove' commands on the
 * 'config.system.change_collection' collection.
 * @tags: [
 *  featureFlagServerlessChangeStreams,
 *  featureFlagSecurityToken,
 *  assumes_read_preference_unchanged,
 *  requires_replication,
 *  requires_fcv_62,
 * ]
 */
import {runCommandWithSecurityToken} from "jstests/libs/multitenancy_utils.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const kPassword = "password";
const kVTSKey = 'secret';
const kKeyFile = "jstests/libs/key1";
const kTenantId = ObjectId();
const kToken = _createTenantToken({tenant: kTenantId});

// Start a replica-set test with one-node and authentication enabled.
const replSetTest = new ReplSetTest({
    name: "shard",
    nodes: 1,
    useHostName: true,
    waitForKeys: false,
    serverless: true,
});

replSetTest.startSet({
    keyFile: kKeyFile,
    setParameter: {
        multitenancySupport: true,
        testOnlyValidatedTenancyScopeKey: kVTSKey,
    }
});
replSetTest.initiate();
const primary = replSetTest.getPrimary();
const tokenConn = new Mongo(primary.host);
const adminDb = primary.getDB('admin');

// A dictionary of users against which authorization of change collection will be verified.
// The format of dictionary is:
//   <key>: user-name,
//   <value>:
//     'isTenantUser': Tenant users will authenticate with a SecurityToken.
//     'db': Authentication db for the user.
//     'roles': Roles assigned to the user.
//     'privileges': Privileges assigned to the user (a role with those privileges will be created).
//     'testPrivileges': Which privileges we should expect to have when performing tests.
const users = {
    root: {
        isTenantUser: false,
        db: "admin",
        roles: ["__system"],
        testPrivileges: {
            insert: true,
            find: true,
            update: true,
            remove: true,
        }
    },
    tenantRoot: {
        isTenantUser: true,
        db: "admin",
        roles: [],
        privileges:
            [{resource: {anyResource: true}, actions: ['insert', 'find', 'update', 'remove']}],
        testPrivileges: {
            insert: true,
            find: true,
            update: true,
            remove: true,
        }
    },
    tenantDbAdmin: {
        isTenantUser: true,
        db: "admin",
        roles: [{role: 'dbAdminAnyDatabase', db: 'admin'}],
        testPrivileges: {
            insert: false,
            find: false,
            update: false,
            remove: false,
        }
    },
    tenantReadOnly: {
        isTenantUser: true,
        db: "admin",
        roles: [{role: 'readAnyDatabase', db: 'admin'}],
        testPrivileges: {
            insert: false,
            find: false,
            update: false,
            remove: false,
        }
    },
    tenantReadWrite: {
        isTenantUser: true,
        db: "admin",
        roles: [{role: 'readWriteAnyDatabase', db: 'admin'}],
        testPrivileges: {
            insert: false,
            find: false,
            update: false,
            remove: false,
        }
    },
    test: {
        isTenantUser: true,
        db: "test",
        roles: [{"role": "readWrite", "db": "test"}, {"role": "readWrite", "db": "config"}],
        testPrivileges: {
            insert: false,
            find: false,
            update: false,
            remove: false,
        },
    },
};

function isRootUser(userName) {
    return userName === "root";
}

// Helper to authenticate and login the user on the provided connection.
function login(userName) {
    const userDbName = users[userName].db;

    if (!users[userName].isTenantUser) {
        // User is not attached to a tenant -> use good old auth() on primary.
        jsTestLog(`Trying to log on ${userDbName} as ${userName}`);
        const authDB = primary.getDB(userDbName);
        assert(authDB.auth(userName, kPassword));
        return authDB;
    } else {
        // Otherwise, authenticate using a Security Token (on db: '$external').
        jsTestLog(`Trying to log on ${userDbName} for tenant: ${kTenantId} as ${userName}`);
        const securityToken =
            _createSecurityToken({user: userName, db: "$external", tenant: kTenantId}, kVTSKey);
        tokenConn._setSecurityToken(securityToken);
        const tokenDB = tokenConn.getDB(userDbName);
        assert.commandWorked(tokenDB.runCommand({connectionStatus: 1}));
        return tokenDB;
    }
}

// Helper to create users from the 'users' dictionary on the provided connection.
function createUsers() {
    function createUser(userName) {
        // A user-creation will happen by 'root' user. If 'userName' is not 'root', then login first
        // as 'root'.
        const user = users[userName];
        jsTestLog(`Creating user ${userName} on tenant:${kTenantId} for db:${user.db}`);

        if (isRootUser(userName)) {
            assert.commandWorked(primary.getDB(user.db).runCommand(
                {createUser: userName, pwd: kPassword, roles: user.roles}));
        } else {
            // Must be authenticated as a user with ActionType::useTenant in order to use unsigned
            // security token.
            const adminDb = login("root");
            let createUserCommand = {createUser: userName, roles: user.roles};
            if (user.privileges !== undefined) {
                // If we have to set privileges for the user, then create first a role with those
                // privileges, and add it to the user roles.
                const userRoleName = 'roleFor_' + userName;
                const createRoleForUserCommand = {
                    createRole: userRoleName,
                    privileges: user.privileges,
                    roles: []
                };
                assert.commandWorked(
                    runCommandWithSecurityToken(kToken, adminDb, createRoleForUserCommand));
                createUserCommand.roles.push({role: userRoleName, db: 'admin'});
            }
            assert.commandWorked(runCommandWithSecurityToken(
                kToken, adminDb.getSiblingDB("$external"), createUserCommand));
            adminDb.logout();
        }

        // Verify that the user has been created by logging-in and then logging out.
        const db = login(userName);
        db.logout();
    }

    // Create the list of users on the specified connection.
    // 'root' user will be created first, as it will be used to authenticate to create the remaining
    // users.
    createUser("root");
    for (const userName of Object.keys(users)) {
        if (userName !== "root") {
            createUser(userName);
        }
    }
}

// Helper to verify if the logged-in user is authorized to invoke 'actionFunc'.
// The parameter 'isAuthorized' determines if the authorization should pass or fail.
function assertActionAuthorized(actionFunc, isAuthorized) {
    try {
        actionFunc();

        // Verify if the authorization is expected to pass.
        assert.eq(isAuthorized, true, "authorization passed unexpectedly");
    } catch (ex) {
        // Verify if the authorization is expected to fail.
        assert.eq(isAuthorized, false, "authorization failed unexpectedly, details: " + ex);

        // Verify that the authorization failed with the expected error code.
        assert.eq(ex.code,
                  ErrorCodes.Unauthorized,
                  "expected operation should fail with code: " + ErrorCodes.Unauthorized +
                      ", found: " + ex.code + ", details: " + ex);
    }
}

const changeCollectionName = "system.change_collection";

// Helper to verify if the logged-in user is authorized to issue 'find' command.
// The parameter 'authDb' is the authentication db for the user, and 'numDocs' determines the
// least number of documents to be retrieved.
function findChangeCollectionDoc(authDb, token, numDocs = 1) {
    const configDB = authDb.getSiblingDB("config");
    let command = {find: changeCollectionName, filter: {_id: 0}};
    const result = assert.commandWorked(runCommandWithSecurityToken(token, configDB, command));
    assert.eq(result.cursor.firstBatch.length, numDocs, result);
}

// Helper to verify if the logged-in user is authorized to issue 'insert' command.
// The parameter 'authDb' is the authentication db for the user.
function insertChangeCollectionDoc(authDb, token) {
    const configDB = authDb.getSiblingDB("config");
    let command = {insert: changeCollectionName, documents: [{_id: 0}]};
    assert.commandWorked(runCommandWithSecurityToken(token, configDB, command));
    findChangeCollectionDoc(authDb, token, 1 /* numDocs */);
}

// Helper to verify if the logged-in user is authorized to issue 'update' command.
// The parameter 'authDb' is the authentication db for the user.
function updateChangeCollectionDoc(authDb, token) {
    const configDB = authDb.getSiblingDB("config");
    let command = {update: changeCollectionName, updates: [{q: {_id: 0}, u: {$set: {x: 0}}}]};
    assert.commandWorked(runCommandWithSecurityToken(token, configDB, command));
    findChangeCollectionDoc(authDb, token, 1 /* numDocs */);
}

// Helper to verify if the logged-in user is authorized to issue 'remove' command.
// The parameter 'authDb' is the authentication db for the user.
function removeChangeCollectionDoc(authDb, token) {
    const configDB = authDb.getSiblingDB("config");
    let command = {delete: changeCollectionName, deletes: [{q: {_id: 0}, limit: 1}]};
    assert.commandWorked(runCommandWithSecurityToken(token, configDB, command));
    findChangeCollectionDoc(authDb, token, 0 /* numDocs */);
}

createUsers();

{
    // Connect with the 'root' user.
    let testPrimary = login("root");

    // Enable change streams to ensure the creation of change collections.
    assert.commandWorked(
        runCommandWithSecurityToken(kToken, adminDb, {setChangeStreamState: 1, enabled: true}));

    // And logout.
    testPrimary.logout();
}

{
    // Connect with the 'test' user.
    let testDB = login("test");
    const collName = jsTestName();

    // Create a collection, insert a document and logout.
    assert.commandWorked(testDB.createCollection(collName));
    assert.commandWorked(testDB.runCommand({insert: collName, documents: [{_id: 0, a: 1, b: 1}]}));

    testDB.logout();
}

{
    let rootDB = login("root");

    // Check that Change Collection was NOT created outside the tenant.
    const configDB = rootDB.getSiblingDB("config");
    let result = assert.commandWorked(
        configDB.runCommand({listCollections: 1, filter: {name: changeCollectionName}}));
    assert.eq(result.cursor.firstBatch.length, 0, result);

    // Check that Change Collection was created in the tenant.
    result = assert.commandWorked(runCommandWithSecurityToken(
        kToken, configDB, {listCollections: 1, filter: {name: changeCollectionName}}));
    assert.eq(result.cursor.firstBatch.length, 1, result);

    rootDB.logout();
}

// Test the privileges for every user and operation in the 'testPrivileges' Map.
for (const userName of Object.keys(users)) {
    const userDB = login(userName);
    // If the user is NOT a tenant user (without any associated tenant), we need to specify on
    // which tenant we are acting; otherwise the tenant of the user will be used.
    const token = !users[userName].isTenantUser ? kToken : tokenConn._securityToken;

    for (const [op, isAuthorized] of Object.entries(users[userName]['testPrivileges'])) {
        let opFunc;
        switch (op) {
            case 'insert':
                opFunc = insertChangeCollectionDoc.bind(null, userDB, token);
                break;
            case 'find':
                opFunc = findChangeCollectionDoc.bind(null, userDB, token);
                break;
            case 'update':
                opFunc = updateChangeCollectionDoc.bind(null, userDB, token);
                break;
            case 'remove':
                opFunc = removeChangeCollectionDoc.bind(null, userDB, token);
                break;
            default:
                assert(false);
        }

        jsTestLog(`${userName} is performing ${op} on ${changeCollectionName}`);
        assertActionAuthorized(opFunc, isAuthorized);
    }

    userDB.logout();
}

replSetTest.stopSet();
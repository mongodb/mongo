/**
 * Overrides the runCommand method to set security token on connection, so that the requests send by
 * client will pass the signed tenant information to server through the security token.
 */
import {OverrideHelpers} from "jstests/libs/override_methods/override_helpers.js";
import {
    assertExpectedDbNameInResponse,
    removeTenantPrefixFromResponse
} from "jstests/libs/override_methods/tenant_aware_response_checker.js";

const kUserName = "userTenant1";
const kTenantId = ObjectId();

function createTenantUser(userName, tenantId) {
    let prepareRootUser = (adminDb) => {
        // Create a user which has root role so that it can be authenticated as a user with
        // ActionType::useTenant in order to use $tenant.
        let res = adminDb.runCommand(
            {createUser: 'root', pwd: 'pwd', roles: ['root'], comment: 'skipOverride'});
        if (res.ok === 1) {
            assert.commandWorked(res);
            print('Create a user "root" with the useTenant privilege successfully.');
        } else {
            // If 'username' already exists, then attempts to create a user with the same name
            // will fail with error code 51003.
            assert.commandFailedWithCode(res, 51003);
        }
    };

    const adminDb = db.getSiblingDB('admin');
    prepareRootUser(adminDb);
    assert(adminDb.auth('root', 'pwd'));

    // Create a user for tenant and then set the security token on the connection.
    print(`Create a tenant user "${userName}", tenant:  ${tenantId}`);
    assert.commandWorked(db.getSiblingDB('$external').runCommand({
        createUser: userName,
        '$tenant': tenantId,
        roles:
            [{role: 'dbAdminAnyDatabase', db: 'admin'}, {role: 'readWriteAnyDatabase', db: 'admin'}]
    }));

    adminDb.logout();
}

function prepareSecurityToken(conn) {
    if (typeof conn._securityToken == 'undefined') {
        print(`Inject security token to the connection: "${tojsononeline(conn)}", user: "${
            kUserName}", tenant: ${kTenantId}`);
        const key = TestData.testOnlyValidatedTenancyScopeKey;
        assert.eq(
            typeof key, 'string', 'testOnlyValidatedTenancyScopeKey not configured in TestData');
        const securityToken =
            _createSecurityToken({user: kUserName, db: '$external', tenant: kTenantId}, key);
        conn._setSecurityToken(securityToken);
    }
}

const kCmdsAllowedWithSecurityToken = new Set([
    `abortTransaction`,
    `aggregate`,
    `buildinfo`,
    `buildinfo`,
    `collMod`,
    `collStats`,
    `collstats`,
    `commitTransaction`,
    `configureFailPoint`,
    `count`,
    `create`,
    `createIndexes`,
    `currentOp`,
    `dbStats`,
    `delete`,
    `deleteIndexes`,
    `distinct`,
    `drop`,
    `dropDatabase`,
    `dropIndexes`,
    `explain`,
    `features`,
    `filemd5`,
    `find`,
    `findAndModify`,
    `findandmodify`,
    `geoNear`,
    `geoSearch`,
    `getMore`,
    `getParameter`,
    `hello`,
    `insert`,
    `isMaster`,
    `ismaster`,
    `listCollections`,
    `listCommands`,
    `listDatabases`,
    `listIndexes`,
    `ping`,
    `renameCollection`,
    `removeQuerySettings`,
    `resourceUsage`,
    `rolesInfo`,
    `serverStatus`,
    `setQuerySettings`,
    `startSession`,
    `statusMetrics`,
    `update`,
    `usersInfo`,
    `validate`,
    // The following commands are going to be allowed in serverless mode but stil not ready for
    // per-tenant yet.
    // `killCursors`,
    // `connectionStatus`,
    // `connPoolStats`,
    // `top`,
    `killop`,
    // `endSessions`,
]);

function isAllowedWithSecurityToken(cmdName) {
    return kCmdsAllowedWithSecurityToken.has(cmdName);
}

// Override the runCommand to inject security token and check for the response has the right nss and
// db name.
function runCommandWithResponseCheck(
    conn, dbName, cmdName, cmdObj, originalRunCommand, makeRunCommandArgs) {
    if (!isAllowedWithSecurityToken(cmdName)) {
        throw new Error(
            "Refusing to run a test that issues commands that are not allowed with security token, " +
            " CmdName: " + cmdName + ", CmdObj: " + tojson(cmdObj));
    }

    prepareSecurityToken(conn);

    let expectPrefix = false;
    if (TestData.expectPrefix) {
        expectPrefix = TestData.expectPrefix;
        cmdObj = Object.assign(cmdObj, {"expectPrefix": TestData.expectPrefix});
    }

    // Actually run the provided command.
    let res = originalRunCommand.apply(conn, makeRunCommandArgs(cmdObj));
    const prefix = kTenantId + "_";
    const prefixedDbName = prefix + dbName;

    assertExpectedDbNameInResponse(res, dbName, prefixedDbName, tojsononeline(res), expectPrefix);
    removeTenantPrefixFromResponse(res, prefix);
    return res;
}

createTenantUser(kUserName, kTenantId);

OverrideHelpers.prependOverrideInParallelShell(
    "jstests/libs/override_methods/inject_security_token.js");
OverrideHelpers.overrideRunCommand(runCommandWithResponseCheck);

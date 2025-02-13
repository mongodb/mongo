/**
 * Simulates the mongoq requests by overriding the runCommand method to set security token on
 * connection, so that the requests send by client will pass the tenant information to server
 * through the security token.
 */
import {runCommandWithSecurityToken} from "jstests/libs/multitenancy_utils.js";
import {OverrideHelpers} from "jstests/libs/override_methods/override_helpers.js";
import {
    getTenantIdForDatabase,
    removeTenantIdAndMaybeCheckPrefixes,
} from "jstests/serverless/libs/tenant_prefixing.js";

const kUserName = "userTenant1";
const kTenantId = ObjectId(TestData.tenantId);
let securityToken = undefined;

function prepareTenantUser(userName, tenantId) {
    let assertResponse = (res) => {
        if (res.ok === 1) {
            assert.commandWorked(res);
            jsTest.log.info(
                `Create a user "${userName}" with the useTenant privilege successfully.`);
        } else {
            // If 'username' already exists, then attempts to create a user with the same name
            // will fail with error code 51003.
            assert.commandFailedWithCode(res, 51003);
        }
    };

    const adminDb = db.getSiblingDB('admin');

    // Create a user which has root role root role implicitely grand ActionType::useTenant
    // in order to use a security token.
    let res = adminDb.runCommand(
        {createUser: 'root', pwd: 'pwd', roles: ['root'], comment: 'skipOverride'});
    assertResponse(res);
    assert(adminDb.auth('root', 'pwd'));

    // Create a user for tenant for setting security token on connections.
    jsTest.log.info(`Create a tenant user "${userName}", tenant:  ${tenantId}`);
    const unsignedToken = _createTenantToken({tenant: tenantId});
    res = runCommandWithSecurityToken(unsignedToken, db.getSiblingDB('$external'), {
        createUser: userName,
        roles:
            [{role: 'dbAdminAnyDatabase', db: 'admin'}, {role: 'readWriteAnyDatabase', db: 'admin'}]
    });
    assertResponse(res);
    adminDb.logout();
}

function createSecurityToken(userName, tenantId) {
    if (TestData.useSignedSecurityToken) {
        jsTest.log.info(`Create signed security token, user: ${userName}, tenant: ${tenantId}`);
        const key = TestData.testOnlyValidatedTenancyScopeKey;
        assert.eq(
            typeof key, 'string', 'testOnlyValidatedTenancyScopeKey not configured in TestData');
        securityToken =
            _createSecurityToken({user: userName, db: '$external', tenant: tenantId}, key);
    } else {
        jsTest.log.info(`Create unsigned security token , tenant: ${tenantId}`);
        securityToken = _createTenantToken({tenant: tenantId, expectPrefix: false});
    }
}

const kCmdsAllowedWithSignedSecurityToken = new Set([
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

function isAllowedWithSignedSecurityToken(cmdName) {
    return kCmdsAllowedWithSignedSecurityToken.has(cmdName);
}

// Override the runCommand to inject security token and check for the response has the right nss and
// db name.
function runCommandForMongoq(
    conn, dbName, cmdName, cmdObj, originalRunCommand, makeRunCommandArgs) {
    const useSignedSecurityToken = !!TestData.useSignedSecurityToken;
    const tenantId = getTenantIdForDatabase(dbName);

    if (useSignedSecurityToken) {
        if (!isAllowedWithSignedSecurityToken(cmdName)) {
            throw new Error(
                `Refusing to run a test that run commands not allowed with security token, cmdName: ${
                    cmdName}, cmdObj: ${tojson(cmdObj)}`);
        }
    }

    if (typeof conn._securityToken == 'undefined') {
        conn._setSecurityToken(securityToken);
    }

    // Actually run the provided command.
    let resObj = originalRunCommand.apply(conn, makeRunCommandArgs(cmdObj));

    // Remove the tenant prefix from errMsg in the result since tests
    // assume the command was run against the original database.
    let checkPrefixOptions = {
        checkPrefix: true,
        expectPrefix: false,
        tenantId,
        dbName,
        cmdName,
        debugLog: "Failed to check tenant prefix in response : " + tojsononeline(resObj) +
            ". The request command obj is " + tojsononeline(cmdObj)
    };

    removeTenantIdAndMaybeCheckPrefixes(resObj, checkPrefixOptions);
    return resObj;
}

if (TestData.useSignedSecurityToken) {
    prepareTenantUser(kUserName, kTenantId);
}
createSecurityToken(kUserName, kTenantId);

OverrideHelpers.prependOverrideInParallelShell("jstests/libs/override_methods/simulate_mongoq.js");
OverrideHelpers.overrideRunCommand(runCommandForMongoq);

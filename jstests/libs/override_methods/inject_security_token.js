/**
 * Overrides the runCommand method to set security token on connection, so that the requests send by
 * client will pass the signed tenant information to server through the security token.
 */
(function() {
'use strict';

load("jstests/libs/override_methods/override_helpers.js");  // For 'OverrideHelpers'.

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
        const securityToken =
            _createSecurityToken({user: kUserName, db: '$external', tenant: kTenantId});
        conn._setSecurityToken(securityToken);
    }
}

function getDbName(nss) {
    if (nss.length === 0 || !nss.includes(".")) {
        return ns;
    }
    return nss.split(".")[0];
}

function checkDbNameInString(str, requestDbName, logError) {
    if (requestDbName.length === 0) {
        return;
    }
    if (requestDbName.includes("_")) {
        return;
    }
    assert.eq(false, str.includes("_" + requestDbName), logError);
}

function checkReponse(res, requestDbName, logError) {
    // We expect the response db name matches request.
    for (let k of Object.keys(res)) {
        let v = res[k];
        if (typeof v === "string") {
            if (k === "dbName" || k == "db" || k == "dropped") {
                assert.eq(v, requestDbName, logError);
            } else if (k === "namespace" || k === "ns") {
                assert.eq(getDbName(v), requestDbName, logError);
            } else if (k === "errmsg" || k == "name") {
                checkDbNameInString(v, requestDbName, logError);
            }
        } else if (Array.isArray(v)) {
            v.forEach((item) => {
                if (typeof item === "object" && item !== null)
                    checkReponse(item, requestDbName, logError);
            });
        } else if (typeof v === "object" && v !== null && Object.keys(v).length > 0) {
            checkReponse(v, requestDbName, logError);
        }
    }
}

// Override the runCommand to inject security token and check for the response has the right nss and
// db name.
function runCommandWithResponseCheck(
    conn, dbName, cmdName, cmdObj, originalRunCommand, makeRunCommandArgs) {
    prepareSecurityToken(conn);

    // Actually run the provided command.
    let res = originalRunCommand.apply(conn, makeRunCommandArgs(cmdObj));
    const logUnmatchedDbName = (dbName, resObj) => {
        return `Response db name does not match sent db name "${dbName}", response: ${
            tojsononeline(resObj)}`;
    };

    checkReponse(res, dbName, logUnmatchedDbName(dbName, res));
    return res;
}

createTenantUser(kUserName, kTenantId);

OverrideHelpers.prependOverrideInParallelShell(
    "jstests/libs/override_methods/inject_security_token.js");
OverrideHelpers.overrideRunCommand(runCommandWithResponseCheck);
}());

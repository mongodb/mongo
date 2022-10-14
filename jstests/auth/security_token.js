// Test passing security token with op messages.
// @tags: [requires_replication, requires_sharding]

(function() {
'use strict';

const tenantID = ObjectId();
const kLogLevelForToken = 5;
const kAcceptedSecurityTokenID = 5838100;
const kLogMessageID = 5060500;
const kLogoutMessageID = 6161506;
const kStaleAuthenticationMessageID = 6161507;
const isMongoStoreEnabled = TestData.setParameters.featureFlagMongoStore;

if (!isMongoStoreEnabled) {
    assert.throws(() => MongoRunner.runMongod({
        setParameter: "multitenancySupport=true",
    }));
    return;
}

function assertNoTokensProcessedYet(conn) {
    assert.eq(false,
              checkLog.checkContainsOnceJson(conn, kAcceptedSecurityTokenID, {}),
              'Unexpected security token has been processed');
}

function makeTokenAndExpect(user, db) {
    const authUser = {user: user, db: db, tenant: tenantID};

    const token = _createSecurityToken(authUser);
    jsTest.log('Using security token: ' + tojson(token));

    // Clone and rewrite OID and BinData fields to be roundtrip-safe.
    const expect = Object.assign({}, token);
    expect.authenticatedUser = Object.assign({}, token.authenticatedUser);
    expect.authenticatedUser.tenant = {'$oid': tenantID.str};
    expect.sig = {'$binary': {base64: token.sig.base64(), subType: '0'}};

    return [token, {token: expect}];
}

function runTest(conn, enabled, rst = undefined) {
    const admin = conn.getDB('admin');

    // Must be authenticated as a user with ActionType::useTenant in order to use $tenant
    assert.commandWorked(admin.runCommand({createUser: 'admin', pwd: 'pwd', roles: ['root']}));
    assert(admin.auth('admin', 'pwd'));

    // Create a tenant-local user.
    const createUserCmd =
        {createUser: 'user1', "$tenant": tenantID, pwd: 'pwd', roles: ['readWriteAnyDatabase']};
    const countUserCmd = {count: "system.users", query: {user: 'user1'}, "$tenant": tenantID};
    if (enabled) {
        assert.commandWorked(admin.runCommand(createUserCmd));

        // Confirm the user exists on the tenant authz collection only, and not the global
        // collection.
        assert.eq(admin.system.users.count({user: 'user1'}),
                  0,
                  'user1 should not exist on global users collection');
        const usersCount = assert.commandWorked(admin.runCommand(countUserCmd));
        assert.eq(usersCount.n, 1, 'user1 should exist on tenant users collection');
    } else {
        assert.commandFailed(admin.runCommand(createUserCmd));
    }

    if (rst) {
        rst.awaitReplication();
    }

    // Dial up the logging to watch for tenant ID being processed.
    const originalLogLevel =
        assert.commandWorked(admin.setLogLevel(kLogLevelForToken)).was.verbosity;

    const tokenConn = new Mongo(conn.host);
    const tokenDB = tokenConn.getDB('admin');

    // Basic OP_MSG command.
    tokenConn._setSecurityToken({});
    assert.commandWorked(tokenDB.runCommand({ping: 1}));
    assertNoTokensProcessedYet(conn);

    // Test that no token equates to unauthenticated.
    assert.commandFailed(tokenDB.runCommand({features: 1}));

    // Passing a security token with unknown fields will fail at the client
    // while trying to construct a signed security token.
    const kIDLParserUnknownField = 40415;
    tokenConn._setSecurityToken({invalid: 1});
    assert.throwsWithCode(() => tokenDB.runCommand({ping: 1}), kIDLParserUnknownField);
    assertNoTokensProcessedYet(conn);

    const [token, expect] = makeTokenAndExpect('user1', 'admin');
    tokenConn._setSecurityToken(token);

    if (enabled) {
        // Basic use.
        assert.commandWorked(tokenDB.runCommand({features: 1}));

        // Connection status, verify that the user/role info is returned without serializing tenant.
        const authInfo = assert.commandWorked(tokenDB.runCommand({connectionStatus: 1})).authInfo;
        jsTest.log(authInfo);
        assert.eq(authInfo.authenticatedUsers.length, 1);
        assert(0 === bsonWoCompare(authInfo.authenticatedUsers[0], {user: 'user1', db: 'admin'}));
        assert.eq(authInfo.authenticatedUserRoles.length, 1);
        assert(0 ===
               bsonWoCompare(authInfo.authenticatedUserRoles[0],
                             {role: 'readWriteAnyDatabase', db: 'admin'}));

        // Look for "Accepted Security Token" message with explicit tenant logging.
        jsTest.log('Checking for: ' + tojson(expect));
        checkLog.containsJson(conn, kAcceptedSecurityTokenID, expect, 'Security Token not logged');

        // Negative test, logMessage requires logMessage privilege on cluster (not granted)
        assert.commandFailed(tokenDB.runCommand({logMessage: 'This is a test'}));

        assert.commandWorked(tokenConn.getDB('test').coll1.insert({x: 1}));

        const log = checkLog.getGlobalLog(conn).map((l) => JSON.parse(l));

        // We successfully dispatched 3 commands as a token auth'd user.
        // The failed command did not dispatch because they are forbidden in multitenancy.
        // We should see three post-operation logout events.
        const logoutMessages = log.filter((l) => (l.id === kLogoutMessageID));
        assert.eq(logoutMessages.length,
                  3,
                  'Unexpected number of logout messages: ' + tojson(logoutMessages));

        // None of those authorization sessions should remain active into their next requests.
        const staleMessages = log.filter((l) => (l.id === kStaleAuthenticationMessageID));
        assert.eq(
            staleMessages.length, 0, 'Unexpected stale authentications: ' + tojson(staleMessages));
    } else {
        assert.commandWorked(
            admin.runCommand({createUser: 'user1', pwd: 'pwd', roles: ['readWriteAnyDatabase']}));
        // Attempting to pass a valid looking security token will fail if not enabled.
        assert.commandFailed(tokenDB.runCommand({features: 1}));
    }

    // Restore logging and conn token before shutting down.
    assert.commandWorked(admin.setLogLevel(originalLogLevel));
}

function runTests(enabled) {
    const opts = {
        auth: '',
        setParameter: "multitenancySupport=" + (enabled ? 'true' : 'false'),
    };
    {
        const standalone = MongoRunner.runMongod(opts);
        assert(standalone !== null, "MongoD failed to start");
        runTest(standalone, enabled);
        MongoRunner.stopMongod(standalone);
    }

    {
        const rst = new ReplSetTest({nodes: 2, nodeOptions: opts});
        rst.startSet({keyFile: 'jstests/libs/key1'});
        rst.initiate();
        runTest(rst.getPrimary(), enabled, rst);
        rst.stopSet();
    }
    // Do not test sharding since mongos must have an authenticated connection to
    // all mongod nodes, and this conflicts with proxying tokens which we'll be
    // performing in mongoq.
}

runTests(true);
runTests(false);
})();
